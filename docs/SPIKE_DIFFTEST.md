# Spike 逐条对比（function-model 的 ISA 验证）

## 构建与运行

```bash
SPIKE_SRC=<riscv-isa-sim 源码> SPIKE_BUILD=<已编译的构建目录> bash scripts/build_spike.sh      # 产物 build-spike/a.out（在仓库外目录构建，不污染普通构建）
XLEN=64 OUT=build-spike64 SPIKE_SRC=... SPIKE_BUILD=... bash scripts/build_spike.sh          # rv64 版本
REF_UNBUF=1 REF_MAX_INSTS=<n> build-spike/a.out --image <image.bin> --diff
```

Spike 的配置（`exec.cpp`、`include/spike_ref.h`）：rv32 是 `rv32imab_zfinx_zbc_zbkb_zbkx`、rv64 是 `rv64imab_zbc_zbkb_zbkx`（没有浮点）；特权级 MSU、**无 PMP**、重置时已启用 sv32（rv64 为 Sv39），否则 `mstatus.SUM/MXR` 写不进去。
function-model 对不在白名单里的 CSR 陷入非法指令，Spike 实现这些 CSR，所以这种指令不让 Spike 执行，改为同步 function-model 的陷入状态（`force_sync`）。
设备/MMIO 访问和中断同样由 function-model 主导，同步给 Spike。

## 结果（rv32，2026-11-04）

| 项目 | 结果 |
|---|---|
| Linux 启动（镜像由 `linux/mkimage.sh` 生成） | 1.35 亿条指令一路到 `rv32 login:`，**无不一致** |
| trace-model 的 gate + vm 程序（66 个，每个上限 300 万条） | 64 个一致，2 个是平台差异（见下） |
| rv64（2026-11-11 起，见下） | 54 个程序 52 个一致，同样 2 个平台差异；Linux 启动到登录提示无不一致 |

## 中断 CSR 按规范（2026-11-04 起）

`mie` 只有 SSIE/MSIE/STIE/MTIE/SEIE/MEIE（0xAAA），没有用户级中断位；`mideleg` 只有 SSI/STI/SEI（0x222）；`sie`/`sip` 只显示被 `mideleg` 委托的位，
`sip` 只能写 SSIP（且要被委托）。trace-model 的 `Csr.cpp` 和 RTL 的 `csr_pkg.sv`/`csr_read.sv`/`csr_regfile.sv` 同步。
此前 `sie` 是 `mie & 0x333`（含用户级位、不看 `mideleg`），`csr-alias-boundary` 因此和 Spike 不一致；改后一致，该测试的周期数 736 → 709。

## 已知差异（2 个程序，都是平台差异）

1. **`timer-interrupt-mask`：`mip.MTIP`。** function-model 由 `mtimecmp` 合成 `MTIP`，Spike 没有这个平台的定时器，读 `mip` 得到 0。对比时应以 function-model 为准。
2. **`software-interrupt-vectored`：`mip.MSIP`。** 同上，软件中断源在平台里，不在 Spike 里。

## RV64（2026-11-11，EXP-029，已合入 main）

`XLEN=64 bash scripts/build_spike.sh` 构建 rv64 的 Spike 对拍版本（Spike 的 ISA 为 `rv64imab_zbc_zbkb_zbkx`，Sv39，无 PMP）。
`tests/rv64/run.sh <工作目录>` 一次完成：构建、生成并编译定向测试、用 `--diff` 逐条对比（需要 `SPIKE_SRC`/`SPIKE_BUILD`、`../trace-model` 检出和 RISC-V 交叉编译器；每个程序限时 300 秒）。

- `tests/rv64/gen_isa.py`：约 33 万条指令的 rv64imab 定向测试（所有 R/I 型运算、W 类、Zba/Zbb/Zbs/Zbc/Zbkb、乘除边界、`ld/sd/lwu`、`amo*.d`、`lr/sc.d`、分支），每条指令之后由 Spike 逐条比对全部寄存器和 CSR。
- `tests/rv64/gen_sv39.py`：Sv39 定向测试：16 种叶子 PTE（权限、A/D、保留位、U 位、W-only）、2 MiB 和 1 GiB 超页（对齐和不对齐）、非规范地址及其别名、MPRV + S/U 数据特权 × SUM × MXR、经 `mret` 进入 S/U 的取指，陷入后继续。
- 测试有效性用变异检查：在模型的页表遍历里分别去掉 SUM、A、D、X 取指、保留位、规范地址、W-only、超页对齐、MXR、U 位检查，九个变异全部被 Spike 对拍抓到。
- 结果：rv64 下 cpu-tests（排除浮点、`load-store`、`string`、`bitmanip-boundary`）加上述两个定向测试，只剩两个已知的平台差异（`software-interrupt-vectored` 的 MSIP、`timer-interrupt-mask` 的 MTIP）；rv32 默认构建的对拍结果与改动前一致。
- `bitmanip-boundary` 的汇编里写死了 rv32 的 `rev8` 编码（0x698），在 rv64 上是非法指令，需要为 rv64 另写。rv64 还没做的：F/D 浮点（Spike 的 rv64 ISA 里也没有）、C 扩展。
- Linux（rv64，`linux/README.md` 的 rv64 镜像）从复位一路启动到登录提示，Spike 逐条对比无不一致（修复 sstatus 写掩码之后；此前 UXL 位被内核写 sstatus 清掉，Spike 保留）。直接启动约 9 分钟里大部分是登录提示后的空转，对拍到登录提示只需几分钟。
- rv64 快速路径（2026-11-11）：基本块缓存改成 XLEN 通用版，rv64 增加 `lwu/ld/sd` 和 `*W` 类指令；rv64 的 FastBlock 另存 satp（上下文字放不下）。验证方法是与 `REF_NO_FAST=1` 比较状态摘要（`REF_STATE_DIGEST_BLOCK` / `REF_DIGEST_LIST`）：rv64 Linux 启动到登录提示的 88 个摘要、`rv64-isa`、`rv64-sv39` 逐个相同；rv32 的 67 个程序输出、Linux 20 个摘要与改动前相同，速度也没有变化（100M 条指令 8.3 秒对 8.5 秒）。rv64 Linux 启动到登录提示约 0.5 秒，不开快速路径约 1.9 秒。快速路径对拍（Spike）不经过，因为 `--diff` 走通用循环。
