# RISC-V Full-System Simulator (function-model)

> 在 AI-Huma 项目里，这个仓库是**架构行为的标准**：trace-model 由它提供执行“事实”，RTL 每提交一条指令都与它逐条对比；它也负责 SPEC 的 BBV / SimPoint / 检查点生成（`scripts/spec/`）。项目总览见总仓库 `README.md` 和 `docs/GUIDE.md`。

轻量、高性能的 RISC-V 全系统功能模拟器（M/S/U 三个特权级，Linux 可启动）。寄存器宽度在编译时选择：

| | rv32（默认，`XLEN=32`） | rv64（`XLEN=64`，可加 `FP=1`） |
|---|---|---|
| 整数/乘除/原子 | RV32IMA | RV64IMA（含 `*W`、`lwu/ld/sd`、`amo*.d`、`lr/sc.d`） |
| 位操作 | Zba/Zbb/Zbs、Zbc、Zbkb 的 `pack/packh`（无 `brev8/zip/xperm`），合称 "B" | 同左，另有 `*W` 变体 |
| 浮点 | 标准 **F/D**（默认 `FP=1`）：32 个 64 位浮点寄存器、`fcsr`、`mstatus.FS/SD`，softfloat，NaN-boxing 和 Spike 一致；rv32 没有 `fmv.x.d/fmv.d.x` 和 `fcvt.l/lu`，`fld/fsd` 是 8 字节访问（两个字）。`FP=0` 是没有任何浮点的核。Zfinx 已删除 | 同左 |
| 压缩指令 C | `RVC=1` 开关（默认 0，不带 C） | 同左 |
| 地址翻译 | Sv32 | Sv39 |
| 检查点格式 | 版本 6（F/D）；`FP=0` 是版本 3（2 仍可读） | 版本 5（F/D）；`FP=0` 是版本 4（`CPU_state` 带浮点寄存器时其后跟 `fcsr`） |
| Spike 对拍 ISA | `rv32imafdb_zbc_zbkb_zbkx`（`FP=0`：`rv32imab_…`） | `rv64imafdb_zbc_zbkb_zbkx`（`FP=0`：`rv64imab_…`） |
| Linux | 5.15.170 + OpenSBI，启动到 `rv32 login:` | 同版本内核，启动到 `rv64 login:` |

两个宽度共用同一份源码（`include/xlen.h` 定义 `xlen_t`）；默认的 `XLEN=32` 行为与 rv64 迁移之前一致。物理地址空间两个宽度相同（见下文），不支持 4 GiB 以上的物理地址。

两类 Difftest：

- standalone 模式下基于 Spike 的 `--diff`（见 `docs/SPIKE_DIFFTEST.md`）
- 供主模拟器链接使用的 `librefcpu.a` 静态库接口（见 `docs/DIFFTEST.md`）

---

## 快速开始

需要 `g++`（C++20）和 `zlib` 开发库；不带 Spike 的构建不需要别的依赖（SoftFloat 用仓库内的 `lib/softfloat/softfloat.a`）。

### 1. 编译

```bash
make -j4                    # rv32，不带 Spike，产物 a.out
make no_spike -j4           # 同上，但产物叫 a.out.nospike（脚本和 linux/ 文档里用这个名字）
make XLEN=64 -j4            # rv64，不带 Spike
make FP=0 -j4               # 没有浮点的核（默认是 FP=1：F/D）
make RVC=1 -j4              # 带 C 扩展（压缩指令，默认 RVC=0；见下文“C 扩展”）
make librefcpu.a            # 给上层主模拟器链接的静态库（不依赖 Spike；XLEN 同样可选）
make clean                  # 删除 a.out a.out.nospike librefcpu.a *.o *.d
```

注意：
- **`.o` 文件不区分 XLEN 和 FP**。在同一目录里切换 `XLEN` 或 `FP` 之前必须 `make clean`，否则目标文件会混链（头文件依赖不会感知命令行的 `-DXLEN` / `-DFP`）。
- `make with_spike` 把 Spike 链进 `a.out`，需要 `SPIKE_SRC`（riscv-isa-sim 源码）和 `SPIKE_BUILD`（已构建好的目录），并且会和普通构建共用 `.o`。**推荐改用 `scripts/build_spike.sh`**，它在仓库外目录 `build-spike/` 构建，不污染普通构建：
  ```bash
  SPIKE_SRC=<riscv-isa-sim 源码> SPIKE_BUILD=<其构建目录> bash scripts/build_spike.sh             # rv32，产物 build-spike/a.out
  XLEN=64 SPIKE_SRC=... SPIKE_BUILD=... OUT=build-spike64 bash scripts/build_spike.sh             # rv64
  XLEN=64 FP=1 SPIKE_SRC=... SPIKE_BUILD=... OUT=build-spike64fp bash scripts/build_spike.sh       # rv64 + F/D
  ```
- 不带 Spike 的二进制收到 `--diff` 只打印 warning 并关闭 difftest。
- SoftFloat：`SPIKE=0` 时链接 `lib/softfloat/softfloat.a`（F/D 用到）；如果出现 ABI/行为不兼容，用 Berkeley SoftFloat 重新编译并替换该文件，头文件在 `lib/softfloat/include`。

### 2. 运行

镜像是一个平铺的二进制文件，加载到 DDR 起点 `0x80000000`；复位 PC 为 `0`，那里的启动 stub 跳到 `0x80000000`。

```bash
./a.out --image path/to/image.bin                # 或简写 ./a.out path/to/image.bin（只有一个参数时当作镜像）
```

standalone 的行为：
- 默认打开 UART 打印；`REF_UNBUF=1` 让输出在重定向时也实时刷新。
- 停止条件：`ebreak`（打印退出码；OpenSBI 探测用的 semihosting 序列不算）、客户机的 SBI 关机调用（`poweroff`）、非法访存、`--max-insts` / `REF_MAX_INSTS` 到达上限。`wfi` 按可退休的 NOP 处理，不会停止。
- 访存地址只允许落在 RAM 和已实现的 MMIO 白名单中，非法地址立即报错退出。

### 3. 启动 Linux

镜像由 `linux/mkimage.sh` 生成，细节见 [linux/README.md](linux/README.md)。预先构建好的内核/OpenSBI/rootfs 目录：rv32 `~/personal/llm4trace/linux_rv32`，rv64 `~/personal/AI-Huma-data/linux_rv64`。

```bash
# rv32
make no_spike -j4
MEM=0x10000000 bash linux/mkimage.sh ~/personal/llm4trace/linux_rv32 /tmp/linux32.bin
REF_UNBUF=1 ./a.out.nospike --image /tmp/linux32.bin         # 约 0.12 G 条指令后到 "rv32 login:"

# rv64（另一个目录，或先 make clean）
make clean && make XLEN=64 no_spike -j4
KOFF=0x200000 DTS=ttg64.dts MEM=0x10000000 bash linux/mkimage.sh ~/personal/AI-Huma-data/linux_rv64 /tmp/linux64.bin
REF_UNBUF=1 ./a.out.nospike --image /tmp/linux64.bin         # 到 "rv64 login:"，约 1 秒（快速路径）
```

登录提示之后系统空转，不会自己退出；用 `REF_MAX_INSTS=<n>` 限制，到上限会打印 `Simulation Timeout`（对只想看到登录提示的运行这是正常结束方式）。

### 4. Spike 对拍

用 Spike 构建（见上）后：

```bash
REF_UNBUF=1 REF_MAX_INSTS=<n> build-spike/a.out --image <image.bin> --diff
```

rv64 的定向测试（约 33 万条指令的 ISA 测试、Sv39 测试，以及 trace-model 的 cpu-tests）一次完成：

```bash
SPIKE_SRC=... SPIKE_BUILD=... bash tests/rv64/run.sh <工作目录>     # 需要 trace-model 检出在 ../trace-model（或 TRACE=<路径>）和 RISC-V 交叉编译器
```

官方 riscv-tests 的 `rv64uf`（11 个）和 `rv64ud`（12 个）自检用例用 `tests/rv64/riscv-tests-fp.sh <工作目录>` 跑（`RISCV_TESTS` 指向带 `isa/` 的检出，默认 `~/personal/simulator/baremetal/riscv-tests`）：每个既要自检通过（`a0 = 0` 后 `ebreak`），又要和 Spike 逐条一致；23 个全部通过。

同一个脚本在最后还会构建 `FP=1` 的 Spike 对拍版本，跑三个浮点测试（`FP_TESTS=0` 跳过）：`rv64-fp`（约 13 万行汇编、35 万条指令：F/D 每条指令 × 5 种舍入模式 × 特殊操作数，含 NaN-boxing、动态舍入、`fcsr` 访问、`flw/fld/fsw/fsd`）、`rv64-fp-trap`（`mstatus.FS` 为 Off 时的非法指令、保留的舍入模式/格式/编码、FS/SD 随浮点状态变化）和 `fp-kernels`（编译器生成的矩阵乘、Horner、牛顿迭代、整数和浮点互转）；每条指令之后由 Spike 逐条比对 x、f 寄存器和全部 CSR，三个全部一致。

结果：rv64 共 54 个程序，52 个一致，剩下 2 个是已知的平台差异（`software-interrupt-vectored` 的 `mip.MSIP`、`timer-interrupt-mask` 的 `mip.MTIP`：这两个中断源在平台里，Spike 里没有）。rv32 的结果和已知差异同样见 `docs/SPIKE_DIFFTEST.md`。

---

## 命令行参数

| 参数 | 说明 |
| :--- | :--- |
| `--image <file>` | 要加载的平铺二进制镜像（DDR 起点）。 |
| `--flash-image <file>` | 加载到启动窗口（地址 0 起，最大 1 MiB），用于前门恢复的恢复程序（`tools/ckpt2stub.py`）。 |
| `--sdcard-image <file>` | SD 卡镜像（OCSDC 设备）。 |
| `--reset-pc <addr>` | 复位 PC，默认 0。 |
| `--mode <mode>` | `normal`（默认）、`bbv`、`ckpt`、`restore`、`special-ckpt`。 |
| `--diff` | Spike 对拍（**仅 normal 模式、仅 Spike 构建有效**，其余情况打印 warning 并关闭）。 |
| `--out-bbv <file>` | `bbv` 模式：BBV 输出文件。 |
| `--points <file>` | `ckpt` 模式：SimPoint 的 `.points` 文件；`special-ckpt` 模式：绝对指令数目标列表。 |
| `--ckpt-dir <dir>` | `ckpt` / `special-ckpt` 模式：检查点输出目录。 |
| `--restore-file <file>` | `restore` 模式：要恢复的检查点。 |
| `--restore-mem-only` | `restore` 模式：只恢复内存，CPU 和设备从复位开始（前门恢复，见 `docs/FRONTDOOR_RESTORE.md`）。 |
| `--max-insts <n>` | 最多执行 n 条指令（`restore` 模式从恢复点起算）。 |

### 环境变量

| 变量 | 作用 |
| :--- | :--- |
| `REF_UNBUF` | 输出不缓冲（重定向到文件时也能实时看到控制台）。 |
| `REF_MAX_INSTS` | 所有模式的指令数上限（默认不设上限），到达打印 `Simulation Timeout`。 |
| `REF_INTERVAL` | BBV / 检查点的区间长度，按用户态指令数计，默认 1e8；`bbv` 与 `ckpt` 必须一致。 |
| `REF_FORCE_REF_ONLY` | 非 0 时打开 `ref_only`（参考模型专用行为）。 |
| `REF_NO_FAST` | 关闭基本块快速路径，走通用循环（用于对照验证）。 |
| `REF_STATE_DIGEST`、`REF_STATE_DIGEST_BLOCK`、`REF_STATE_DIGEST_FULL`、`REF_DIGEST_LIST` | 状态摘要，用于比较快速路径与通用路径、前门与后门恢复（`tools/frontdoor_check.sh`）。 |
| `REF_PC_EVERY`、`REF_PC_RING`、`REF_TRAP_TRACE`、`REF_TRAP_EVERY`、`REF_UART_TRACE`、`REF_STOP_AT_SHELL`、`REF_FAST_STATS` 等 | 调试开关，见 `exec.cpp`。 |

---

## 地址空间布局（rv32、rv64 相同）

当前实现采用“**256 MiB SDRAM + 2 GiB DDR + 离散 IO word 存储**”，物理地址严格白名单检查：

### RAM（两段独立分配）
- SDRAM：`0x40000000 - 0x4fffffff`（256 MiB）
- DDR：`0x80000000 - 0xffffffff`（2 GiB）
- 两段之间的空洞不是 RAM，访问触发非法访存错误

### IO / Boot（离散存储）
- `0x00000000 - 0x000fffff`：启动窗口（启动 stub、`--flash-image`；`0x1000` 附近是 Boot ROM stub）
- `0x10000000 - 0x100000ff`：UART（ns16550a）
- `0x10000004`：OpenSBI 兼容寄存器（初始化写 `0x00006000`）
- `0x1fb00000 - 0x1fb00fff`：XPS INTC
- `0x1fd0e000 - 0x1fd0e0ff`：Timer（`+0` 低位读取返回 `sim_time`，`+4` 高位返回 0，`+8` mtimecmp）
- `0x1fe10000 - 0x1fe100ff`：OCSDC

低地址和外设地址按 `word_addr -> word_data` 离散存储，读未初始化的 IO word 返回 `0`。不在 RAM 或 MMIO 白名单内的访存直接报错：
`[RefCPU] illegal store: addr=0x..., size=... (not in RAM or implemented MMIO)`

---

## Checkpoint 格式

`zlib gzip`（写模式 `wb1`，后缀 `.gz`），按顺序写入：

1. `header`（16 字节）：`magic = "Rem\0"`（`uint32`）、`format_ver`（`uint16`）、特权级（`uint16`）、`ram_size`（`uint32`，DDR 字节数）、`io_range_count`（`uint32`）
2. `sdram_size`（`uint32_t`，256 MiB；版本 2 没有这个字段）
3. `CPU_state`（POD 原样二进制，**大小随 XLEN 变**：rv32 为 236 字节，rv64 为 464 字节）
4. `interval_inst_count`（`uint64_t`）
5. DDR 原始字节流（`ram_size` 字节，当前 2 GiB）
6. SDRAM 原始字节流（256 MiB；版本 2 没有）
7. `io_ranges + io_data`（重复 `io_range_count` 次）：`base`、`size`（各 `uint32_t`），其后是 `size` 字节原始数据。顺序固定为 `BOOT -> UART -> XPS INTC -> TIMER -> OCSDC`

### 版本
- 版本 2：rv32，没有 SDRAM 段，仍可恢复（SDRAM 清零）
- 版本 3：rv32（当前）
- 版本 4：rv64（当前，`FP=0`）
- 版本 5：rv64 + F/D（`FP=1`）：`CPU_state` 里 `gpr` 后多 32 个 `uint64_t` 浮点寄存器（整体 720 字节），其后紧跟 `uint32_t fcsr`，再是 `interval_inst_count`；`FP=0` 的模拟器读它会报版本不支持，`tools/ckpt2stub.py` 能读，并在前门恢复程序里用 `fmv.d.x` 和 `csrw fcsr` 还原浮点状态。

两个宽度的 `CPU_state` 布局不同，所以互不兼容：rv64 的模拟器读 rv32 检查点会报 `Unsupported checkpoint version: 3`，反之亦然。

### 恢复

```bash
./a.out --image linux.bin --mode restore --restore-file path/to/ckpt.gz --max-insts 100000000
```

流程：`init()` 分配 DDR 和 SDRAM；校验 header（magic、版本、`ram_size`、`io_range_count`）；覆盖 `CPU_state` 和 `interval_inst_count`；覆盖 DDR 和 SDRAM；校验 IO 布局（base+size）并恢复 `io_data`。

自行解析时：先校验 magic 和版本，按上面顺序用 `gzread`；`CPU_state` 的布局以 `include/ref.h` 为准（同编译器/ABI 假设）。`tools/ckpt2stub.py` 是一份现成的 Python 解析器（rv32 / rv64 都支持）。

---

## 核心接口

### `Ref_cpu`（`include/ref.h`、`exec.cpp`）
- `init(reset_pc, image, size)`：分配内存、加载镜像。
- `exec(config)`：主循环，按 `SimConfig` 处理运行模式、BBV、检查点和 Difftest。
- `RISCV()`：取指、译码、分发；`RV32IM()`（rv64 下是 RV64IM + Zba/Zbb/Zbs/Zbc）、`RV32A()`、`RV32CSR()`、`RVFD()`（OP-FP 和四种融合乘加；`flw/fld/fsw/fsd` 走 `RV32IM()` 的 load/store 代码，只是换了寄存器堆）分别处理整数、原子、CSR、浮点。
- 基本块快速路径（`exec.cpp`）：XLEN 通用，`--diff` 不经过它。
- `save_checkpoint() / restore_checkpoint()`（`simpoint.cpp`）：见上。

### `SpikeRef`（`include/spike_ref.h`，仅 Spike 构建）
- `step(n)`、`reg_check(state, priv)`、`sync_state(state, priv)`。

### `refcpu_api`（供主模拟器链接）
- 头文件 `include/api/refcpu_api.h`，产物 `librefcpu.a`，不依赖 Spike，不走 `main.cpp`；默认静默，`refcpu_set_uart_print(ctx, true)` 打开 UART 打印。
- `RefCpuState` 里的寄存器是 `refcpu_xlen_t`（rv32 为 32 位，rv64 为 64 位），所以链接方必须用同一个 `XLEN` 编译。

---

## 目录结构

| 路径 | 内容 |
|---|---|
| `main.cpp` | 命令行解析 |
| `exec.cpp` | 指令执行、MMU（Sv32/Sv39）、CSR、设备（UART/XPS INTC/OCSDC/Timer）、快速路径、Spike 对拍循环 |
| `simpoint.cpp` | BBV 生成、检查点保存与恢复 |
| `refcpu_api.cpp`、`include/api/refcpu_api.h` | 静态库接口 |
| `include/` | `xlen.h`（XLEN 相关类型和掩码）、`ref.h`、`CSR.h`、`RISCV.h`（地址与操作码）、`spike_ref.h` |
| `lib/` | `softfloat/`（`softfloat.a` 和头文件）、`include/`（Spike/fesvr/fdt 头文件，Spike 构建用） |
| `linux/` | 设备树、rv64 内核配置片段、`mkimage.sh`（拼 Linux 镜像） |
| `tools/` | `ckpt2stub.py`（检查点转前门恢复程序）、`frontdoor_check.sh` |
| `tests/rv64/` | rv64 对拍用的定向测试生成器和 `run.sh` |
| `scripts/` | `build_spike.sh`、`run_bbv.sh`、`run_ckpt.sh`；`scripts/spec/` 是现行的 SPECint2006 检查点流水线 |
| `SimPoint/simpoint` | SimPoint 3.2 的 x86 Linux 二进制（Mac 上要自己编译，见 `scripts/spec/README.md`） |
| `docs/` | 下面列出的文档 |
| `build-spike/` | `scripts/build_spike.sh` 的默认输出（不进 git） |

---

## C 扩展（`RVC=1`，2026-11-13，分支 `exp/rvc`）

编译开关 `RVC=1`（`XLEN=32` 和 `64` 都支持，和 F/D 可以同时开；默认 0，`RVC=0` 的行为与之前完全一致）。

- 取指单位是半字：`pc` 只要求 2 字节对齐，一条 32 位指令可以从任意半字开始，第二个半字可以在下一个 cache line 或下一页（独立翻译，缺页时 `mtval` = `pc+2`）。
- 16 位指令在取指时展开成等价的 32 位指令（`include/rvc.h`，一个纯函数），后面的执行代码不变；`ilen`（2 或 4，`RefCpuStepInfo.inst_len` 给上层；`instruction` 是展开后的 32 位）用于 `pc+ilen`、`jal/jalr` 的链接值、`ecall`/`ebreak` 之后的 pc。非法或保留的 16 位编码产生非法指令异常，`mtval` 是 16 位原编码（`inst_raw`）。
- `misa.C` 置位；`jalr` 目标只清第 0 位；`mepc`/`sepc` 只清第 0 位；`medeleg` 第 0 位（取指地址未对齐）只读 0。
- 快速路径（基本块缓存）也支持压缩指令：每条指令记字节偏移和长度，压缩指令展开后按 32 位指令解码；块遇到跨页的 32 位指令、48 位编码或非法编码就停下交给 `RISCV()`。带 C 的 Linux 启动实测快速路径约 300 MIPS（15 亿条指令 4.6 s），只走 `RISCV()` 单步约 50 MIPS。快速路径与单步路径的状态摘要（`REF_STATE_DIGEST_BLOCK` / `REF_NO_FAST`，含 CSR）在 Linux 前 1 亿条指令和 58 个测试程序上逐块一致。
- 不支持：48 位及更长的指令编码（当作非法指令）、`misa` 的 C 位不可写（不能关掉）、Zc*（Zcb/Zcmp 等）。
- Spike 对拍的 ISA 串加 `c`（`rv64imafdcb_…`）；`scripts/build_spike.sh` 认 `RVC=1`。
- 测试 `tests/rv64/rvc.sh <工作目录>`（`XLEN=32|64`）：`tests/rv64/gen_rvc.py` 生成的定向测试（全部 16 位编码里非访存、非跳转的在随机寄存器值上执行；全部访存编码，含 FS 开 / 关的浮点；`c.j` / `c.jal` / `c.beqz` / `c.bnez` 前后向多种偏移、`c.jr` / `c.jalr` 每个寄存器；32 位指令跨 cache line / 跨页；Sv39 下第二个半字缺页），trace-model 的 cpu-tests 带 C 编译，官方 `riscv-tests` 的 `rv<XLEN>uc`，每条指令与 Spike 比较。cpu-tests 里有已知的 5 个不通过项：都是测试源码假定 4 字节指令（中断向量表的 4 字节槽位、`trap-precise-store` 用 `+8` 跳过 `ecall; sw`），以及不带 C 时也有的两个平台差异。
- Linux：`linux/ttg64gc.dts`（`riscv,isa = "rv64imafdc"`）；内核和 OpenSBI 带 C 编译（`CONFIG_RISCV_ISA_C=y`、`PLATFORM_RISCV_ISA=rv64imafdc_zicsr_zifencei`），用户态仍是 `rv64g`（不带 C）；镜像 `~/personal/AI-Huma-data/linux_rv64gc/`。

---

## 已知限制

- F/D 是默认配置（`FP=1`，rv32 和 rv64），`FP=0` 去掉全部浮点；还没有的有：半精度（Zfh）、RTL 里的 F/D、F/D 的 SPEC 和 Linux 用户态验证。Zfinx 已经删除。
- rv32 的前门恢复（`tools/ckpt2stub.py`）只能还原 NaN-boxed 的单精度浮点寄存器（`fmv.w.x`），双精度值会被警告并跳过（rv32 没有 `fmv.d.x`）。
- `mstatus` 的写掩码保留了 WPRI 位 0、4 和 XS（15:16），和 Spike 不同；只有往 `mstatus` 写全 1 才看得出来，不影响软件。
- 两个宽度都**没有**压缩指令 C。
- rv64 的 MMIO 访问：function-model 里对 MMIO 的 `sd/ld` 读回还没有做。
- SPEC 的 rv64 版本还没有重新生成：`scripts/spec/` 和 `~/personal/AI-Huma-data/spec-rv32imab` 都是 rv32imab；rv64 只有 Linux 启动和定向测试的检查点。
- Spike 对拍的两个平台差异（见上）；`--diff` 不经过快速路径，所以比普通运行慢得多。
- 物理地址限制为 32 位的上述窗口；Sv39 翻译出 4 GiB 以上的物理地址按“地址不在 RAM”处理。
- rv64 `bitmanip-boundary`（trace-model）里写死了 rv32 的 `rev8` 编码，不能在 rv64 上跑。

---

## 文档与脚本位置

- [docs/DIFFTEST.md](docs/DIFFTEST.md)：Difftest 用法（standalone 与静态库）。
- [docs/SPIKE_DIFFTEST.md](docs/SPIKE_DIFFTEST.md)：Spike 对拍的构建、结果、已知差异（rv32 与 rv64）。
- [docs/SIMPOINT_SPECINT2006.md](docs/SIMPOINT_SPECINT2006.md)：SimPoint 原理，现行流程与上游流程的对照。
- [scripts/spec/README.md](scripts/spec/README.md)：现行 SPECint2006 检查点流程（rv32imab、Linux 镜像、ref 输入），数据在 `~/personal/AI-Huma-data/spec-rv32imab`。
- [docs/FRONTDOOR_RESTORE.md](docs/FRONTDOOR_RESTORE.md)：前门检查点恢复（rv32 与 rv64）。
- [linux/README.md](linux/README.md)：Linux 镜像（rv32 与 rv64）。
- `scripts/run_bbv.sh`、`scripts/run_ckpt.sh`：上游的裸机镜像批量脚本，**在仓库根目录运行**（里面的 `./a.out`、`./SimPoint/simpoint` 是相对根目录的路径）。
