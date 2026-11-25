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

## rv64gc（现行默认）

- **检查点格式版本**：当前 rv64gc（含 F/D `fpr` 与 `fcsr`）为 **版本 5**（早期 rv64 无 FP 为版本 4，rv32 为版本 3，不能互读）。
- `ckpt2stub.py` 展开 64 位常量（`lui/addiw/slli/addi`），并在版本 5 中同步恢复浮点寄存器与 `fcsr`，`mstatus` 去掉只读 UXL/SXL 位。
- BBV：`global_pc_to_id` 键为 64 位 `xlen_t`；快速路径与 `REF_NO_FAST=1` 输出逐字节相同。
- 验证（`frontdoor_check.sh`）：rv64 Linux 的 U/S/M 态检查点前门与后门恢复状态摘要逐个相同。
