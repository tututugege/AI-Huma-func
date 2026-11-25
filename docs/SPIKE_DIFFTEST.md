# Spike 逐条对比（function-model 的 ISA 验证）

> `main` 现行配置统一为 **`rv64gc`**（`XLEN=64 FP=1 RVC=1`）；`rv32` 已归档至 `rv32-legacy` 分支。

## 1. 构建与运行

```bash
# 本地构建带 Spike 逐条对拍的 function-model（产物在仓库外，不污染普通构建）
XLEN=64 FP=1 RVC=1 SPIKE_SRC=~/rv64spec/riscv-isa-sim SPIKE_BUILD=~/rv64spec/riscv-isa-sim/build_local \
  OUT=~/rv64spec/fm_spike64 bash scripts/build_spike.sh

# 单镜像对拍
REF_UNBUF=1 REF_MAX_INSTS=<n> ~/rv64spec/fm_spike64/a.out --image <image.bin> --diff
```

- **集群 / NFS 现成静态版**（`eda-04`~`10` 与本机通用）：`/nfs_global/S/houruyao/tools/spike/bin/fm_spike64`（源码在 `/nfs_global/S/houruyao/tools/riscv-isa-sim/`，详见总仓 `docs/SERVER.md`）。
- **随机压力对拍**：可用 `rtl/tools/fuzz/fuzz_run.py --spike-fm <a.out>` 并行扫数千个 RV64GC 随机程序（2026-11-25 实测 500/500 PASS）。

## 2. Spike 配置与已知平台差异

- Spike ISA：`rv64imafdc_zba_zbb_zbc_zbkb_zbkx_zbs`（Sv39，特权级 MSU，**无 PMP**）。
- 对不在 `function-model` 白名单里的 CSR，模型陷入非法指令，同步给 Spike（`force_sync`）；MMIO 与平台中断同样由模型主导并同步给 Spike。
- **2 个已知平台差异测试**：`timer-interrupt-mask`（`mip.MTIP`）与 `software-interrupt-vectored`（`mip.MSIP`），因 Spike 无本平台 CLINT 定时器/软件中断源，读 `mip` 为 0，以 `function-model` 为准。

## 3. 已验证范围（rv64gc）

- **定向 ISA / Sv39 / F/D / RVC**：`tests/rv64/run.sh`（`gen_isa.py`、`gen_sv39.py`、`gen_fp.py`、`gen_fp_trap.py`、`fp-kernels.c`）+ 官方 `riscv-tests` `rv64uf/ud/uc` 逐条与 Spike 一致；
- **Linux 启动**：rv64gc Linux 5.15（OpenSBI + 内核 + rootfs）从复位启动到 `rv64 login:` 全程逐条与 Spike 一致；
- **随机 Fuzzer**：`rtl/tools/fuzz/fuzz_run.py --spike-fm` 500 个随机 seed（全部 5 种 Profile）500/500 PASS。
