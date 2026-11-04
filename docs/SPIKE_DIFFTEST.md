# Spike 逐条对比（function-model 的 ISA 验证）

## 构建与运行

```bash
SPIKE_SRC=<riscv-isa-sim 源码> SPIKE_BUILD=<已编译的构建目录> bash scripts/build_spike.sh      # 产物 build-spike/a.out（在仓库外目录构建，不污染普通构建）
REF_UNBUF=1 REF_MAX_INSTS=<n> build-spike/a.out --image <image.bin> --diff
```

Spike 的配置（`exec.cpp`、`include/spike_ref.h`）：`rv32imab_zfinx_zbc_zbkb_zbkx`、特权级 MSU、**无 PMP**、重置时已启用 sv32（否则 `mstatus.SUM/MXR` 写不进去）。
function-model 对不在白名单里的 CSR 陷入非法指令，Spike 实现这些 CSR，所以这种指令不让 Spike 执行，改为同步 function-model 的陷入状态（`force_sync`）。
设备/MMIO 访问和中断同样由 function-model 主导，同步给 Spike。

## 结果（2026-11-04）

| 项目 | 结果 |
|---|---|
| Linux 启动（`/tmp/linux_check.bin`，镜像由 `linux/mkimage.sh` 生成） | 1.35 亿条指令一路到 `rv32 login:`，**无不一致** |
| trace-model 的 gate + vm 程序（66 个，每个上限 300 万条） | 63 个一致 |

## 已知差异（3 个程序）

1. **`csr-alias-boundary`：`sie` 的语义。** function-model 的 `sie` 是 `mie & 0x333`，包含用户级中断位（8、4、0），也不按 `mideleg` 屏蔽；Spike（按规范）`sie = mie & mideleg`，且没有用户级中断位。这是从原参考核继承下来的设计选择，不是 Spike 的问题。要不要改成规范行为，会同时影响 trace-model 和 RTL 的 CSR 文件，需要决定。
2. **`timer-interrupt-mask`：`mip.MTIP`。** function-model 由 `mtimecmp` 合成 `MTIP`，Spike 没有这个平台的定时器，读 `mip` 得到 0。平台差异，对比时应以 function-model 为准。
3. **`software-interrupt-vectored`：`mip.MSIP`。** 同上，软件中断源在平台里，不在 Spike 里。

2 和 3 是平台差异（对 `mip` 的读取可以当作设备值同步），1 是 ISA 层的真实偏差。
