# Front-door checkpoint restore (prototype)

A checkpoint normally injects the CPU state (registers, CSRs, privilege, devices) into the simulator. The front-door
restore injects only the **memory**; the CPU starts from reset and executes a small restore program that sets the state
with ordinary instructions and ends with `mret` into the checkpoint's pc and privilege. A reference model and a DUT that
run the same program end up in the same state by construction; the DUT needs no state-injection path.

```
tools/ckpt2stub.py ckpt.gz stub.bin                      # restore program for the boot window (about 70-110 instructions)
a.out.nospike --mode restore --restore-file ckpt.gz --restore-mem-only --flash-image stub.bin --max-insts N
tools/frontdoor_check.sh a.out.nospike ckpt.gz image.bin N [every]   # compare with the back-door restore
```

`--restore-mem-only` loads DDR and SDRAM only. The instruction counters and the instruction limit start at the first
`mret` (the simulator prints `[front-door] restore program done ...`); the timer offset absorbs the reset of `sim_time`.

## What the program does
1. Timer: `mtimecmp`, then `mtime` high and low (last). The value written is lower by the number of instructions that still
   run, so `mtime` equals the checkpoint's at the first instruction of the target.
2. UART LCR, MCR, SCR, IER (byte stores); interrupt controller IER and MER if non-zero.
3. CSRs (`mtvec`, `mcause`, `mtval`, `mscratch`, `mideleg`, `medeleg`, `sepc`, `stvec`, `scause`, `sscratch`, `stval`,
   `satp`, `mie`, `mip`), then `mepc` = target pc and `mstatus` with MPP = target privilege and MPIE = the checkpoint's MIE
   (MIE is 0 while the program runs). `sstatus`, `sie`, `sip` are shadows and follow.
4. A pending LR/SC reservation: `lr.w` to its address.
5. x1..x31 with immediates, then `mret`.

## What is not exact
`mret` sets mstatus.MPP = U, MPIE = 1 and mepc = the target pc. For an S or U target nothing can observe the old values
(every trap into M rewrites them). For an M-mode target they are lost, which only matters if the checkpoint lies inside
an M-mode trap handler before it has saved mepc/mstatus: the tool warns for every M-mode target. The LR/SC reservation and
UART/interrupt-controller state beyond the registers above are restored best-effort (the tool warns when it skips a
non-zero interrupt-controller ISR).

## Checked (function model, identical state digests at every sampled instruction)
libquantum at 60 M and 125 M instructions (S mode, kernel) and 400 M and 700 M (U mode), xalancbmk at 300 M, omnetpp at
1.5 G: 20 M instructions each, about 38 timer interrupts. Four M-mode checkpoints (OpenSBI): 5 M instructions each,
identical. A checkpoint at the first instruction of OpenSBI's `_trap_handler` differs, as described above.

## rv64（2026-11-11）

- 检查点格式版本：rv32 为 3（旧的 2 仍可读），**rv64 为 4**：`CPU_state` 按内存布局写出，寄存器是 64 位（464 字节，rv32 是 236 字节），所以两种格式不能互读，版本号让读的一方报错而不是读错。rv64 的模拟器读 rv32 检查点会报 `Unsupported checkpoint version: 3`。
- `ckpt2stub.py` 按版本号识别 rv64：用 `lui/addiw/slli/addi` 展开 64 位常量（对 4000 个随机和边界值用小模拟器验证过展开结果），`mstatus` 去掉只读的 UXL/SXL（高 32 位）。rv32 检查点生成的恢复程序与改动前逐字节相同。
- BBV：`global_pc_to_id` 的键改为 `xlen_t`，rv64 的 BBV 文件格式不变（基本块编号）。rv64 Linux（`REF_INTERVAL=100000`）226 个区间，快速路径与 `REF_NO_FAST=1` 的输出逐字节相同。
- 检查与结果（`frontdoor_check.sh`，每 9973 条比较一次状态摘要）：rv64 Linux 的 3 个 SimPoint 检查点（U 态，pc 在 0x3f…，需要 64 位常量）、3 个 S 态内核检查点（pc = 0xffffffff80…）、1 个 M 态（OpenSBI）检查点，前门恢复与后门恢复都逐个相同（300–501 个摘要）。
