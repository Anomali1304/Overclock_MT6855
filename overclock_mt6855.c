// SPDX-License-Identifier: GPL-2.0
/*
 * overclock_mt6855.c
 *
 * Beryl / MT6855 CPU + GPU backend.
 *
 * Hardware model:
 *   CPU0-5 = A55 cpufreq domain
 *   CPU6-7 = A78 cpufreq domain
 *   GPU    = IMG/PowerVR BXM-8-256 through MediaTek GPUFreq v2 / GPUEB
 *
 * IMPORTANT:
 * - No MT6789/Mali/MFGPLL code is used here.
 * - CPU writes only the existing MediaTek cpufreq-hw LUT entry 0.
 * - GPU patches the live GPUFreq v2 working table entry 0 and Beryl's
 *   internal signed OPP table entry 0. No fixed-OPP lock is used.
 *
 * This is the hardware backend source. It still needs to be compiled against
 * the exact Beryl kernel build tree/config before the .ko can be declared
 * insmod-verified.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/cpufreq.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/ioport.h>

#define DRV_NAME "overclock_mt6855"

/* Beryl mediatek-cpufreq-hw.c ABI */
#define LUT_MAX_ENTRIES 32U
#define LUT_FREQ GENMASK(11, 0)
#define LUT_ROW_SIZE 0x4

enum {
	REG_FREQ_LUT_TABLE,
	REG_FREQ_ENABLE,
	REG_FREQ_PERF_STATE,
	REG_FREQ_HW_STATE,
	REG_EM_POWER_TBL,
	REG_FREQ_LATENCY,
	REG_ARRAY_SIZE,
};

/*
 * Exact 6.12 mediatek-cpufreq-hw layout used by Android 6.12:
 * table, reg_bases[], res, base, nr_opp.
 */
struct cpufreq_mtk_mirror {
	struct cpufreq_frequency_table *table;
	void __iomem *reg_bases[REG_ARRAY_SIZE];
	struct resource *res;
	void __iomem *base;
	int nr_opp;
};

/* Beryl GPUFreq v2 ABI */
enum gpufreq_target {
	TARGET_DEFAULT = 0,
	TARGET_GPU = 1,
	TARGET_STACK = 2,
	TARGET_INVALID = 3,
};

enum gpufreq_posdiv {
	POSDIV_POWER_1 = 0,
	POSDIV_POWER_2 = 1,
	POSDIV_POWER_4 = 2,
	POSDIV_POWER_8 = 3,
	POSDIV_POWER_16 = 4,
};

struct gpufreq_opp_info {
	unsigned int freq;
	unsigned int volt;
	unsigned int vsram;
	enum gpufreq_posdiv posdiv;
	unsigned int vaging;
	unsigned int power;
};

typedef int (*fn_gpu_opp_num_t)(enum gpufreq_target);
typedef unsigned int (*fn_gpu_cur_freq_t)(enum gpufreq_target);
typedef int (*fn_gpu_cur_oppidx_t)(enum gpufreq_target);
typedef const struct gpufreq_opp_info *(*fn_gpu_get_working_table_t)(enum gpufreq_target);
typedef int (*fn_gpu_commit_t)(enum gpufreq_target, int);

static fn_gpu_opp_num_t p_gpufreq_get_opp_num;
static fn_gpu_cur_freq_t p_gpufreq_get_cur_freq;
static fn_gpu_cur_oppidx_t p_gpufreq_get_cur_oppidx;
static fn_gpu_get_working_table_t p_gpufreq_get_working_table;
static fn_gpu_commit_t p_gpufreq_commit;

static __nocfi int __kprobe_nop(struct kprobe *kp, struct pt_regs *regs)
{
	return 0;
}

static __nocfi int resolve_symbol(const char *name, unsigned long *addr)
{
	struct kprobe kp = {
		.symbol_name = name,
		.pre_handler = __kprobe_nop,
	};
	int ret;

	ret = register_kprobe(&kp);
	if (ret)
		return ret;

	*addr = (unsigned long)kp.addr;
	unregister_kprobe(&kp);
	return 0;
}

static int resolve_gpu_symbols(void)
{
	unsigned long addr;
	int ret;

	ret = resolve_symbol("gpufreq_get_opp_num", &addr);
	if (ret) return ret;
	p_gpufreq_get_opp_num = (fn_gpu_opp_num_t)addr;

	ret = resolve_symbol("gpufreq_get_cur_freq", &addr);
	if (ret) return ret;
	p_gpufreq_get_cur_freq = (fn_gpu_cur_freq_t)addr;

	ret = resolve_symbol("gpufreq_get_cur_oppidx", &addr);
	if (ret) return ret;
	p_gpufreq_get_cur_oppidx = (fn_gpu_cur_oppidx_t)addr;

	ret = resolve_symbol("gpufreq_get_working_table", &addr);
	if (ret) return ret;
	p_gpufreq_get_working_table = (fn_gpu_get_working_table_t)addr;

	ret = resolve_symbol("gpufreq_commit", &addr);
	if (ret) return ret;
	p_gpufreq_commit = (fn_gpu_commit_t)addr;

	return 0;
}


/*
 * The Beryl wrapper keeps the signed GPU OPP table behind its g_shared_status
 * pointer. The symbol is local to mtk_gpufreq_wrapper.ko, so resolve it through
 * kallsyms/kprobe rather than hard-coding a module address.
 *
 * From the shipping Beryl wrapper:
 *   ctx + 0x47c = GPU signed table
 *   ctx + 0x0c  = GPU signed-table entry count
 * each entry is 0x18 bytes / six u32 fields.
 */
#define BERYL_SIGNED_TABLE_PTR_OFF 0x47c
#define BERYL_SIGNED_TABLE_NUM_OFF 0x0c
#define BERYL_GPU_OPP_STRIDE 0x18

static unsigned long g_shared_status_addr;
static bool signed_symbols_resolved;
static bool gpu_symbols_resolved;

static int resolve_signed_table(void)
{
	return resolve_symbol("g_shared_status", &g_shared_status_addr);
}

static struct gpufreq_opp_info *gpu_signed_table(unsigned int *count)
{
	void *ctx;
	void *table;

	if (!g_shared_status_addr)
		return NULL;

	ctx = READ_ONCE(*(void **)g_shared_status_addr);
	if (!ctx)
		return NULL;

	table = *(void **)((char *)ctx + BERYL_SIGNED_TABLE_PTR_OFF);
	if (!table)
		return NULL;

	if (count)
		*count = READ_ONCE(*(unsigned int *)((char *)ctx +
							  BERYL_SIGNED_TABLE_NUM_OFF));

	return table;
}

/* ---------- state ---------- */

static DEFINE_MUTEX(oc_lock);

static unsigned int cpu_a55_rep = 0;
static unsigned int cpu_a78_rep = 6;
module_param(cpu_a55_rep, uint, 0444);
module_param(cpu_a78_rep, uint, 0444);

static unsigned int cpu_a55_target_khz;
static unsigned int cpu_a78_target_khz;
module_param(cpu_a55_target_khz, uint, 0644);
module_param(cpu_a78_target_khz, uint, 0644);

static unsigned int cpu_max_oc_percent = 60;
module_param(cpu_max_oc_percent, uint, 0644);

static unsigned int cpu_absolute_max_khz = 3000000;
module_param(cpu_absolute_max_khz, uint, 0644);

/*
 * Exact original LUT row is captured, so Reset restores the whole row rather
 * than only the frequency field.
 */
static u32 cpu_a55_orig_lut;
static u32 cpu_a78_orig_lut;
static bool cpu_a55_orig_valid;
static bool cpu_a78_orig_valid;

static unsigned int gpu_target_freq_khz;
static unsigned int gpu_target_volt_mV = 775;
module_param(gpu_target_freq_khz, uint, 0644);
module_param(gpu_target_volt_mV, uint, 0644);

static char cpu_oc_result[256] = "not applied";
static char gpu_oc_result[256] = "not applied";

static int cpu_oc_result_get(char *buf, const struct kernel_param *kp)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", cpu_oc_result);
}

static const struct kernel_param_ops cpu_result_ops = {
	.get = cpu_oc_result_get,
};

module_param_cb(cpu_oc_result, &cpu_result_ops, NULL, 0444);

static int gpu_oc_result_get(char *buf, const struct kernel_param *kp)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", gpu_oc_result);
}

static const struct kernel_param_ops gpu_result_ops = {
	.get = gpu_oc_result_get,
};

module_param_cb(gpu_oc_result, &gpu_result_ops, NULL, 0444);

/* ---------- CPU ---------- */

static bool cpu_mask_is(const cpumask_t *mask,
			unsigned int first, unsigned int last)
{
	unsigned int cpu;

	for (cpu = 0; cpu < nr_cpu_ids; cpu++) {
		bool expected = cpu >= first && cpu <= last;
		if (cpumask_test_cpu(cpu, mask) != expected)
			return false;
	}
	return true;
}

static int cpu_get_domain(unsigned int rep,
			  struct cpufreq_policy **policy_out,
			  struct cpufreq_mtk_mirror **c_out)
{
	struct cpufreq_policy *policy;
	struct cpufreq_mtk_mirror *c;

	policy = cpufreq_cpu_get(rep);
	if (!policy)
		return -ENODEV;

	if (!policy->driver_data || !policy->freq_table) {
		cpufreq_cpu_put(policy);
		return -ENODATA;
	}

	c = (struct cpufreq_mtk_mirror *)policy->driver_data;

	if (!c->reg_bases[REG_FREQ_LUT_TABLE] ||
	    !c->reg_bases[REG_FREQ_PERF_STATE]) {
		cpufreq_cpu_put(policy);
		return -ENODEV;
	}

	/* Refuse to touch a non-Beryl two-domain layout. */
	if (rep == cpu_a55_rep && !cpu_mask_is(policy->cpus, 0, 5)) {
		cpufreq_cpu_put(policy);
		return -EXDEV;
	}
	if (rep == cpu_a78_rep && !cpu_mask_is(policy->cpus, 6, 7)) {
		cpufreq_cpu_put(policy);
		return -EXDEV;
	}

	*policy_out = policy;
	*c_out = c;
	return 0;
}

static int cpu_patch_domain(unsigned int rep, unsigned int target_khz,
			    u32 *orig_lut, bool *orig_valid,
			    const char *name)
{
	struct cpufreq_policy *policy;
	struct cpufreq_mtk_mirror *c;
	u32 raw, old_state;
	unsigned int stock_khz;
	unsigned int cap_khz;
	bool moved = false;
	int ret;

	ret = cpu_get_domain(rep, &policy, &c);
	if (ret)
		return ret;

	if (c->nr_opp < 2) {
		cpufreq_cpu_put(policy);
		return -EINVAL;
	}

	stock_khz = policy->freq_table[0].frequency;

	if (!*orig_valid) {
		*orig_lut = readl_relaxed(c->reg_bases[REG_FREQ_LUT_TABLE]);
		*orig_valid = true;
	}

	if (!target_khz) {
		cpufreq_cpu_put(policy);
		return 0;
	}

	if (target_khz <= stock_khz) {
		cpufreq_cpu_put(policy);
		return -EINVAL;
	}

	cap_khz = stock_khz +
		(stock_khz * cpu_max_oc_percent) / 100U;

	if (cap_khz > cpu_absolute_max_khz)
		cap_khz = cpu_absolute_max_khz;

	if (target_khz > cap_khz || target_khz / 1000U > 0xfffU) {
		cpufreq_cpu_put(policy);
		return -ERANGE;
	}

	/*
	 * Only move away from OPP0 when the hardware is currently selecting OPP0.
	 * Never assume that index 1 is active; read the actual PERF_STATE first.
	 */
	old_state = readl_relaxed(c->reg_bases[REG_FREQ_PERF_STATE]);
	if (old_state == 0) {
		writel_relaxed(1, c->reg_bases[REG_FREQ_PERF_STATE]);
		mb();
		moved = true;
	}

	raw = readl_relaxed(c->reg_bases[REG_FREQ_LUT_TABLE]);
	raw &= ~LUT_FREQ;
	raw |= FIELD_PREP(LUT_FREQ, target_khz / 1000U);
	writel_relaxed(raw, c->reg_bases[REG_FREQ_LUT_TABLE]);
	mb();

	raw = readl_relaxed(c->reg_bases[REG_FREQ_LUT_TABLE]);
	if (FIELD_GET(LUT_FREQ, raw) * 1000U != target_khz) {
		if (moved)
			writel_relaxed(old_state, c->reg_bases[REG_FREQ_PERF_STATE]);
		cpufreq_cpu_put(policy);
		return -EIO;
	}

	/* Software table and policy ceiling must expose the replacement OPP. */
	policy->freq_table[0].frequency = target_khz;
	policy->cpuinfo.max_freq = target_khz;
	policy->max = target_khz;

	if (moved) {
		writel_relaxed(old_state, c->reg_bases[REG_FREQ_PERF_STATE]);
		mb();
	}

	pr_info(DRV_NAME ": %s %u -> %u KHz\n",
		name, stock_khz, target_khz);

	cpufreq_cpu_put(policy);
	return 0;
}

static int cpu_restore_domain(unsigned int rep, u32 orig_lut,
			      bool orig_valid, const char *name)
{
	struct cpufreq_policy *policy;
	struct cpufreq_mtk_mirror *c;
	u32 old_state;
	bool moved = false;
	int ret;

	if (!orig_valid)
		return 0;

	ret = cpu_get_domain(rep, &policy, &c);
	if (ret)
		return ret;

	if (c->nr_opp < 1) {
		cpufreq_cpu_put(policy);
		return -EINVAL;
	}

	old_state = readl_relaxed(c->reg_bases[REG_FREQ_PERF_STATE]);
	if (old_state == 0 && FIELD_GET(LUT_FREQ, readl_relaxed(
			c->reg_bases[REG_FREQ_LUT_TABLE])) == 0) {
		writel_relaxed(1, c->reg_bases[REG_FREQ_PERF_STATE]);
		mb();
		moved = true;
	}

	writel_relaxed(orig_lut, c->reg_bases[REG_FREQ_LUT_TABLE]);
	mb();

	policy->freq_table[0].frequency =
		FIELD_GET(LUT_FREQ, orig_lut) * 1000U;
	policy->cpuinfo.max_freq = policy->freq_table[0].frequency;
	policy->max = policy->freq_table[0].frequency;

	if (moved) {
		writel_relaxed(old_state, c->reg_bases[REG_FREQ_PERF_STATE]);
		mb();
	}

	pr_info(DRV_NAME ": restored %s LUT0\n", name);
	cpufreq_cpu_put(policy);
	return 0;
}


static struct gpufreq_opp_info gpu_wt_orig;
static struct gpufreq_opp_info gpu_st_orig;
static bool gpu_orig_valid;

/* ---------- GPU ---------- */

static __nocfi int gpu_apply(void)
{
	const struct gpufreq_opp_info *wt_const;
	struct gpufreq_opp_info *wt;
	struct gpufreq_opp_info *st;
	unsigned int n, sn;
	unsigned int old_freq, old_volt, old_vsram;
	unsigned int new_volt;
	int ret;
	int cur_idx;

	if (!gpu_target_freq_khz)
		return -EINVAL;

	if (!gpu_symbols_resolved) {
		ret = resolve_gpu_symbols();
		if (ret)
			return ret;
		gpu_symbols_resolved = true;
	}

	if (!signed_symbols_resolved) {
		if (resolve_signed_table())
			return -ENOSYS;
		signed_symbols_resolved = true;
	}

	n = p_gpufreq_get_opp_num(TARGET_GPU);
	if (!n || n > 256)
		return -ENODATA;

	wt_const = p_gpufreq_get_working_table(TARGET_GPU);
	if (!wt_const)
		return -ENODATA;

	wt = (struct gpufreq_opp_info *)wt_const;
	st = gpu_signed_table(&sn);
	if (!st || !sn)
		return -ENOSYS;

	if (sn != n)
		return -EINVAL;

	if (gpu_target_freq_khz <= wt[0].freq)
		return -EINVAL;

	if (gpu_target_volt_mV > 1000)
		return -ERANGE;

	new_volt = gpu_target_volt_mV ? gpu_target_volt_mV * 100U : wt[0].volt;

	old_freq = wt[0].freq;
	old_volt = wt[0].volt;
	old_vsram = wt[0].vsram;

	if (!gpu_orig_valid) {
		gpu_wt_orig = wt[0];
		gpu_st_orig = st[0];
		gpu_orig_valid = true;
	}

	/*
	 * Patch only OPP 0. The six-word/0x18-byte layout is shared by the
	 * working and signed tables in the shipping Beryl wrapper.
	 */
	wt[0].freq = gpu_target_freq_khz;
	wt[0].volt = new_volt;
	st[0].freq = gpu_target_freq_khz;
	st[0].volt = new_volt;

	/*
	 * Keep the existing VSRAM and remaining signed fields untouched. They
	 * belong to the platform's validated voltage relationship/power data.
	 */
	mb();

	cur_idx = p_gpufreq_get_cur_oppidx(TARGET_GPU);
	if (cur_idx < 0)
		cur_idx = 0;

	ret = p_gpufreq_commit(TARGET_GPU, cur_idx);
	if (ret) {
		wt[0].freq = old_freq;
		wt[0].volt = old_volt;
		wt[0].vsram = old_vsram;
		st[0].freq = old_freq;
		st[0].volt = old_volt;
		return ret;
	}

	pr_info(DRV_NAME ": GPU OPP0 %u/%u -> %u/%u KHz/%u\n",
		old_freq, old_volt, gpu_target_freq_khz, new_volt, old_vsram);

	return 0;
}

static __nocfi int gpu_reset(void)
{
	const struct gpufreq_opp_info *wt_const;
	struct gpufreq_opp_info *wt;
	struct gpufreq_opp_info *st;
	unsigned int sn;
	int ret;

	if (!gpu_orig_valid)
		return 0;

	if (!gpu_symbols_resolved) {
		ret = resolve_gpu_symbols();
		if (ret)
			return ret;
		gpu_symbols_resolved = true;
	}

	if (!signed_symbols_resolved) {
		if (resolve_signed_table())
			return -ENOSYS;
		signed_symbols_resolved = true;
	}

	wt_const = p_gpufreq_get_working_table(TARGET_GPU);
	if (!wt_const)
		return -ENODATA;

	wt = (struct gpufreq_opp_info *)wt_const;
	st = gpu_signed_table(&sn);
	if (!st || !sn)
		return -ENOSYS;

	wt[0] = gpu_wt_orig;
	st[0] = gpu_st_orig;
	mb();

	{
		int idx = p_gpufreq_get_cur_oppidx(TARGET_GPU);
		if (idx < 0)
			idx = 0;
		ret = p_gpufreq_commit(TARGET_GPU, idx);
	}
	if (!ret)
		gpu_orig_valid = false;

	pr_info(DRV_NAME ": GPU OPP0 restored to %u/%u KHz/%u\n",
		gpu_wt_orig.freq, gpu_wt_orig.volt, gpu_wt_orig.vsram);

	return ret;
}

/* ---------- public module controls ---------- */

static __nocfi int apply_set(const char *val, const struct kernel_param *kp)
{
	unsigned int trigger;
	int rcpu_a55 = 0, rcpu_a78 = 0, rgpu = 0;

	if (kstrtouint(val, 10, &trigger))
		return -EINVAL;

	if (trigger != 1)
		return 0;


	mutex_lock(&oc_lock);

	if (cpu_a55_target_khz)
		rcpu_a55 = cpu_patch_domain(
			cpu_a55_rep, cpu_a55_target_khz,
			&cpu_a55_orig_lut, &cpu_a55_orig_valid,
			"A55/CPU0-5");

	if (cpu_a78_target_khz)
		rcpu_a78 = cpu_patch_domain(
			cpu_a78_rep, cpu_a78_target_khz,
			&cpu_a78_orig_lut, &cpu_a78_orig_valid,
			"A78/CPU6-7");

	if (gpu_target_freq_khz)
		rgpu = gpu_apply();

	if (rcpu_a55 || rcpu_a78) {
		if (!rcpu_a55 && cpu_a55_orig_valid)
			cpu_restore_domain(cpu_a55_rep, cpu_a55_orig_lut,
					   cpu_a55_orig_valid, "A55/CPU0-5");
		if (!rcpu_a78 && cpu_a78_orig_valid)
			cpu_restore_domain(cpu_a78_rep, cpu_a78_orig_lut,
					   cpu_a78_orig_valid, "A78/CPU6-7");
		scnprintf(cpu_oc_result, sizeof(cpu_oc_result),
			  "FAIL: A55=%d A78=%d", rcpu_a55, rcpu_a78);
	} else {
		scnprintf(cpu_oc_result, sizeof(cpu_oc_result),
			  "OK: A55=%u A78=%u",
			  cpu_a55_target_khz, cpu_a78_target_khz);
	}

	if (rgpu) {
		if (!rcpu_a55 && !rcpu_a78) {
			cpu_restore_domain(cpu_a55_rep, cpu_a55_orig_lut,
					   cpu_a55_orig_valid, "A55/CPU0-5");
			cpu_restore_domain(cpu_a78_rep, cpu_a78_orig_lut,
					   cpu_a78_orig_valid, "A78/CPU6-7");
		}
		scnprintf(gpu_oc_result, sizeof(gpu_oc_result),
			  "FAIL: rc=%d", rgpu);
	} else if (gpu_target_freq_khz) {
		unsigned int f = p_gpufreq_get_cur_freq ?
			p_gpufreq_get_cur_freq(TARGET_GPU) : 0;
		scnprintf(gpu_oc_result, sizeof(gpu_oc_result),
			  "OK: current=%u KHz", f);
	}

	mutex_unlock(&oc_lock);
	return 0;
}

static int apply_get(char *buf, const struct kernel_param *kp)
{
	return scnprintf(buf, PAGE_SIZE, "0\n");
}

static const struct kernel_param_ops apply_ops = {
	.set = apply_set,
	.get = apply_get,
};

static int reset_set(const char *val, const struct kernel_param *kp)
{
	unsigned int trigger;
	int r1, r2, rg;

	if (kstrtouint(val, 10, &trigger))
		return -EINVAL;
	if (trigger != 1)
		return 0;

	mutex_lock(&oc_lock);

	r1 = cpu_restore_domain(cpu_a55_rep, cpu_a55_orig_lut,
				cpu_a55_orig_valid, "A55/CPU0-5");
	r2 = cpu_restore_domain(cpu_a78_rep, cpu_a78_orig_lut,
				cpu_a78_orig_valid, "A78/CPU6-7");
	rg = gpu_reset();

	scnprintf(cpu_oc_result, sizeof(cpu_oc_result),
		  "RESET: A55=%d A78=%d", r1, r2);
	scnprintf(gpu_oc_result, sizeof(gpu_oc_result),
		  "RESET: GPU=%d", rg);

	mutex_unlock(&oc_lock);
	return 0;
}

static int reset_get(char *buf, const struct kernel_param *kp)
{
	return scnprintf(buf, PAGE_SIZE, "0\n");
}

static const struct kernel_param_ops reset_ops = {
	.set = reset_set,
	.get = reset_get,
};

static __nocfi int status_get(char *buf, const struct kernel_param *kp)
{
	struct cpufreq_policy *p55 = NULL, *p78 = NULL;
	struct cpufreq_mtk_mirror *c55 = NULL, *c78 = NULL;
	unsigned int f55 = 0, f78 = 0, fg = 0;
	int rg = 0;

	if (!cpu_get_domain(cpu_a55_rep, &p55, &c55))
		f55 = p55->freq_table[0].frequency;
	if (p55)
		cpufreq_cpu_put(p55);

	if (!cpu_get_domain(cpu_a78_rep, &p78, &c78))
		f78 = p78->freq_table[0].frequency;
	if (p78)
		cpufreq_cpu_put(p78);

	if (p_gpufreq_get_cur_freq)
		fg = p_gpufreq_get_cur_freq(TARGET_GPU);
	else
		rg = -ENOSYS;

	return scnprintf(buf, PAGE_SIZE,
		"cpu_a55_max=%u\ncpu_a78_max=%u\ngpu_freq=%u\ngpu_rc=%d\n",
		f55, f78, fg, rg);
}

static const struct kernel_param_ops status_ops = {
	.get = status_get,
};

static int __init overclock_mt6855_init(void)
{
	pr_info(DRV_NAME ": init\n");
	/* Do not register kprobes or call vendor GPU code during insmod. */
	pr_info(DRV_NAME ": ready: CPU domains A55=0-5 A78=6-7, GPU=GPUFreq-v2/GPUEB (lazy)\n");
	return 0;
}

static void __exit overclock_mt6855_exit(void)
{
	mutex_lock(&oc_lock);

	gpu_reset();
	cpu_restore_domain(cpu_a55_rep, cpu_a55_orig_lut,
			   cpu_a55_orig_valid, "A55/CPU0-5");
	cpu_restore_domain(cpu_a78_rep, cpu_a78_orig_lut,
			   cpu_a78_orig_valid, "A78/CPU6-7");

	mutex_unlock(&oc_lock);

	pr_info(DRV_NAME ": exit\n");
}

static struct kernel_param_ops status_param_ops = {
	.get = status_get,
};

module_param_cb(apply, &apply_ops, NULL, 0644);
module_param_cb(reset, &reset_ops, NULL, 0644);
module_param_cb(status, &status_param_ops, NULL, 0444);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Anomali");
MODULE_DESCRIPTION("Beryl MT6855 CPU + GPUFreq v2/GPUEB tuning backend (Beryl-native GPU ABI)");
MODULE_VERSION("0.7.0");
module_init(overclock_mt6855_init);
module_exit(overclock_mt6855_exit);
