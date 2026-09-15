# Overclock MT6855 — Beryl

Native-style CPU/GPU OPP0 replacement backend for the MT6855 Beryl vendor stack.

## What it does

- CPU: replaces the existing `mediatek-cpufreq-hw` LUT entry 0 and updates the
  live cpufreq software table/policy maximum. It does **not** append an OPP.
- GPU: patches GPUFreq-v2 working table entry 0 and the Beryl wrapper's signed
  GPU OPP table entry 0. It does **not** use `gpufreq_fix_target_oppidx()` or
  `gpufreq_fix_custom_freq_volt()` for OC.
- OPP count stays unchanged.
- Governor/DVFS remains in control after Apply.
- Reset restores the complete original CPU LUT row and GPU working/signed rows.

## Beryl-specific evidence used

The shipping `mtk_gpufreq_wrapper.ko` exports `gpufreq_get_working_table()`.
Its `gpu_signed_opp_table_proc_show()` accesses the GPU signed table through
the local `g_shared_status` context at:

- context + `0x47c`: GPU signed table pointer
- context + `0x0c`: GPU signed-table count
- entry stride: `0x18` bytes

The signed entry is six 32-bit fields. Only frequency and voltage are changed;
the validated VSRAM and remaining fields are preserved.

The CPU mirror follows the Android 6.12 `mtk_cpufreq_data` layout:

`table, reg_bases[6], res, base, nr_opp`

## Important

This source is intended for the exact Beryl Android 16 / 6.12.30 kernel ABI.
Build against the exact matching `Module.symvers`. Do not mix with a generic
GKI or another device's vendor modules.

The module is source-corrected from the earlier fixed-OPP implementation, but
a successful compile/boot/thermal validation still has to be performed on the
actual phone.
