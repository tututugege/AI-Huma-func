# SPECint2006 SimPoint 检查点流水线

从 SPEC 源码到 trace-model 回放用的检查点的完整流程（SimPoint 原理和上游裸机流程见 [docs/SIMPOINT_SPECINT2006.md](../../docs/SIMPOINT_SPECINT2006.md)）。**生成的数据放在 `$SPECDATA`（默认
`~/personal/AI-Huma-data/spec-rv32imab`），不放 `/tmp`（重启会清空），也不进仓库。** 公共设置在 `env.sh`。

## 目录

| `$SPECDATA/` 下 | 内容 | 由谁生成 |
|---|---|---|
| `bin/` | 12 个 SPECint 可执行文件（静态链接） | 第 1 步 |
| `img/<bench>.bin` | 每个程序一个 Linux 镜像（OpenSBI + 内核 + initramfs，ref 输入） | 第 2 步 |
| `bbv/<bench>.{bbv,log}`、`bbv/done.txt` | BBV 和运行日志（日志里有输出校验结果） | 第 3 步 |
| `sp/<bench>.{points,weights,labels}` | SimPoint 结果 | 第 4 步 |
| `ckpt/<bench>/ckpt_sp<id>_target<interval>_warmup1.gz` | 检查点 | 第 4 步 |
| `status/<bench>.txt` | 每个程序的流水线状态 | 第 4 步 |
| `tm/res/` | trace-model 回放结果（`trace-model/scripts/spec/replay.sh`） | 回放 |

## 步骤

1. **编译**（在 Lima 虚拟机 `rvbuild` 里，Buildroot 2024.02.9 的 GCC 13.3 工具链 `~/br13`）：
   ```bash
   limactl start rvbuild
   limactl shell rvbuild -- bash -c 'cd /tmp && cp <repo>/function-model/scripts/spec/build_spec.py /tmp/ &&
     OUT=$HOME/spec_out_b BUILD=$HOME/spec_build_b python3 /tmp/build_spec.py all-c 471.omnetpp 473.astar 483.xalancbmk'
   # 拷回宿主机 $SPECDATA/bin/（虚拟机里宿主机目录是只读的）
   ```
   - ISA：`ARCH` 默认 `-march=rv32ima_zicsr_zifencei_zba_zbb_zbs -mabi=ilp32`，即 rv32imab（B = Zba+Zbb+Zbs）。
     静态链接的 glibc 仍是 rv32ima 编译的。
   - **不能用 GCC 12.4**（`~/br`）：rv32 开 Zbs 时编译器内部错误（`unrecognizable insn`，16 位数据的位操作），12 个程序里 9 个编不过；只开 Zba+Zbb 没问题。
     GCC 13.3 工具链：`cp ~/br/.config ~/br13/.config`，把 `BR2_GCC_VERSION_12_X` 换成 `13_X`，
     `make -C ~/buildroot-2024.02.9 O=~/br13 HOSTCC=gcc-13 HOSTCXX=g++-13 olddefconfig toolchain`。
   - 必须带 `-fsigned-char`（RISC-V 的 char 是无符号的，SPEC 参考输出来自 x86；h264ref 不加会输出不一致）。
   - 优化级别 `-O2`，SPEC 源码在 `~/spec`（虚拟机内）/ `$LLM4TRACE/spec2006`（宿主机）。
2. **镜像**：`bash mk_images.sh` → `img/<bench>.bin`（`mk_testimg.py ref` 生成 initramfs 覆盖层，
   `linux/mkimage.sh` 拼镜像，RAM `MEM=0x70000000`）。
3. **BBV**：`bash run_bbv.sh`（12 个并行，区间 `REF_INTERVAL=10000000` 条指令）。
   SimPoint 3.2 要在本机编译：仓库里的 `function-model/SimPoint/simpoint` 是 x86 Linux 版，Mac 上跑不了。
   源码包是 `SimPoint.3.2.tar.gz`（UCSD，本机另有一份在 `~/Downloads`），解到 `~/personal/AI-Huma-data/tools/`，然后：
   ```bash
   cd SimPoint.3.2/analysiscode
   sed -i '' -E 's/Utilities::check\((input|output|in|out|file|f),/Utilities::check(!\1.fail(),/' *.cpp *.h   # 新编译器不再接受流转 bool
   make CXXFLAGS="-O3 -w"        # -> ../bin/simpoint，env.sh 的 SIMPOINT 默认指向它
   ```
4. **SimPoint + 检查点**：`bash driver.sh`（哪个程序的 BBV 跑完就启动它的 `pipeline.sh`；`MAXJ` 同时数，默认 3；`SKIP` 跳过已在处理的程序）。生成检查点要把整个程序顺序再跑一遍，单个程序只能单线程（sjeng 约 3.7 小时）。
   `pipeline.sh` 先检查 BBV 运行：正常关机、没有截断、没有 Oops、RUN 行数对、退出码全为 0、输出与 SPEC 参考一致
   （libquantum 的 CRLF、astar 的末位浮点差异是已知可容忍的）；然后：
   - SimPoint：`-maxK 100 -seedproj 0721 -seedkm 11037 -numInitSeeds 5 -iters 100 -fixedLength on`
   - 检查点：`--mode ckpt`，预热 1 个区间（`warmup1`），区间 1000 万条指令。
5. **回放**：`trace-model/scripts/spec/replay.sh quick|full`
   - quick：perlbench gcc mcf hmmer sjeng h264ref，覆盖 90% 权重的点；full：12 个程序全部点。
   - 前门恢复，预热 = 检查点区间，测量 `-c 10000000`。

## 耗时参考

- 2026-11-03（rv32ima，function-model 快速路径）：单个程序 BBV 22 分钟（mcf）到 3 小时（h264ref），12 个并行。
- function-model 的快速路径只覆盖基础指令和 M 扩展，Zba/Zbb/Zbs 指令走慢路径，rv32imab 版本会更慢。

## 历史

- 2026-11-03 第一次生成（rv32ima），数据在 `/tmp/specref`，2026-11-04 重启后丢失；脚本从会话记录里找回，放进本目录。
- 2026-11-04 用 rv32imab 重新生成，2026-11-05 完成：12 个程序共 580 个检查点；`pipeline.sh` 校验前先去掉插进 RUN 行的 BBV 打印（perlbench、sjeng 曾因此误判）。
