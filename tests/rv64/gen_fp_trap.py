#!/usr/bin/env python3
"""rv64 F/D trap and status tests (function-model FP=1 vs Spike): mstatus.FS = Off makes every FP instruction and FP CSR
access illegal, reserved rounding modes / formats trap, FS/SD track the FP state, misa shows F and D.  A machine-mode handler
folds mcause / mepc into a checksum and skips the instruction (no oracle values here, Spike is the oracle).
usage: gen_fp_trap.py <out-dir>      writes rv64-fp-trap.c and rv64-fp-trap-helper.S
"""
import sys, os
out = []
A = out.append
A("""    .section .text
    .align 2
    .globl rv64_fptrap_body
rv64_fptrap_body:
    mv s11, ra
    li s10, 0
    la t0, handler
    csrw mtvec, t0
    j start
    .align 2
handler:
    csrr t5, mcause
    xor s10, s10, t5
    csrr t5, mtval
    xor s10, s10, t5
    csrr t5, mepc
    addi t5, t5, 4
    csrw mepc, t5
    mret
start:""")
def fold(r="a2"):
    A(f"    xor s10, s10, {r}\n    slli t6, s10, 1\n    srli s10, s10, 63\n    or s10, s10, t6")
def mst(): A("    csrr a2, mstatus"); fold(); A("    csrr a2, sstatus"); fold()
def fs(v): A("    li t0, 0x6000\n    csrc mstatus, t0\n    li t0, %d\n    csrs mstatus, t0" % (v << 13))
A("    csrr a2, misa"); fold()
for v in (0, 1, 2, 3):   # every FS value: legal for v != 0
    fs(v); mst()
    for ins in ("fadd.s f1, f2, f3", "fadd.d f1, f2, f3", "flw f1, 0(sp)", "fld f1, 0(sp)", "fsw f1, -8(sp)", "fsd f1, -8(sp)",
                "fmv.x.d a2, f1", "fmv.d.x f1, a0", "csrr a2, fflags", "csrr a2, frm", "csrr a2, fcsr", "csrw fcsr, zero",
                "csrr a2, fflags", "fclass.d a2, f1", "feq.s a2, f1, f2"):
        A(f"    li a0, 5\n    li a2, 0\n    {ins}"); fold(); mst()
    fs(v); A("    li a0, 0x3f800000\n    fmv.w.x f1, a0\n    csrr a2, mstatus"); fold(); A("    csrr a2, sstatus"); fold()
fs(1); A("    csrr a2, fflags"); mst()          # a pure CSR read leaves FS Initial
A("    li a0, 0x3f800000\n    fmv.w.x f1, a0"); mst()   # an FP register write makes FS Dirty
fs(2); A("    fadd.s f2, f1, f1"); mst()
fs(1); A("    feq.s a2, f1, f1"); fold(); mst()   # no flags raised, no register written: FS stays
fs(1); A("    fmv.x.w a2, f1\n    li a0, 0x7f800001\n    fmv.w.x f2, a0"); fs(1); A("    feq.s a2, f2, f2"); mst()   # flags raised: Dirty
fs(0); A("    csrw sstatus, zero"); mst(); A("    li t0, 0x6000\n    csrs sstatus, t0"); mst(); A("    li t0, 0x6000\n    csrc sstatus, t0"); mst()
fs(3); A("    li t0, 0x7e79aa\n    csrw mstatus, t0")   # the writable bits both models agree on (the function model also keeps the WPRI bits 0 and 4 and XS); mst(); A("    csrw mstatus, zero"); mst()
fs(1)
# reserved rounding modes and formats (.word: the assembler would refuse them)
def enc(op, rd, rs1, rs2, rm, f7): return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (rm << 12) | (rd << 7) | op
OPFP = 0x53
for rm in (0, 4, 5, 6, 7):
    for f7 in (0x00, 0x01, 0x04, 0x05, 0x08, 0x09, 0x0c, 0x0d, 0x2c, 0x2d, 0x60, 0x61, 0x68, 0x69, 0x70, 0x71, 0x78, 0x79, 0x50, 0x51,
               0x10, 0x11, 0x14, 0x15, 0x20, 0x21, 0x22, 0x23, 0x2e, 0x40, 0x44, 0x7c):   # incl. half/quad formats and unused funct7
        for rs2 in (0, 1, 2, 3, 5):
            A(f"    li a2, 0\n    .word {enc(OPFP, 3, 1, rs2, rm, f7)}"); fold(); A("    csrr a2, fflags"); fold(); A("    csrw fflags, zero")
for frm in (5, 6, 7):    # dynamic rounding mode with a reserved fcsr.frm
    A(f"    li t0, {frm}\n    csrw frm, t0")
    for f7 in (0x00, 0x01, 0x0c, 0x2c, 0x60, 0x68, 0x08, 0x20):
        A(f"    .word {enc(OPFP, 3, 1, 0, 7, f7)}"); fold()
A("    csrw frm, zero")
for op in (0x43, 0x47, 0x4b, 0x4f):   # fused: reserved rm and fmt
    for rm in (0, 5, 7):
        for fmt in (0, 1, 2, 3):
            A(f"    .word {(3 << 27) | (fmt << 25) | (2 << 20) | (1 << 15) | (rm << 12) | (4 << 7) | op}"); fold()
# LOAD-FP / STORE-FP with reserved widths
for f3 in range(8):
    A(f"    .word {(f3 << 12) | (1 << 7) | 0x07 | (2 << 15)}"); A(f"    .word {(f3 << 12) | 0x27 | (2 << 15) | (3 << 20)}")
A("    mv ra, s11\n    mv a0, s10\n    ret")
d = sys.argv[1]
os.makedirs(d, exist_ok=True)
open(os.path.join(d, "rv64-fp-trap-helper.S"), "w").write("\n".join(out) + "\n")
open(os.path.join(d, "rv64-fp-trap.c"), "w").write("extern long rv64_fptrap_body(void);\nint main(void) { rv64_fptrap_body(); return 0; }\n")
print(len(out), "lines")
