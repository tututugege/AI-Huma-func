#!/usr/bin/env python3
"""Generate directed RV64 F/D instruction tests (assembly) for function-model (FP=1) vs Spike.

Every FP instruction runs on interesting operands under every rounding mode; the result (raw 64-bit register, so NaN-boxing
shows) and fflags are folded into a checksum.  The oracle is Spike (--diff compares x and f registers and the CSRs after every
instruction), the tests do not check values themselves.
usage: gen_fp.py <out-dir>      writes rv64-fp.c and rv64-fp-helper.S (build with -march=rv64imafd, -mabi=lp64d)
"""
import sys, itertools, os, struct

def f32(x): return struct.unpack("<I", struct.pack("<f", x))[0]
def f64(x): return struct.unpack("<Q", struct.pack("<d", x))[0]

S = [0x00000000, 0x80000000, 0x3f800000, 0xbf800000, 0x3fc00000, f32(0.1), f32(3.0), 0x7f7fffff, 0x7f800000, 0xff800000,
     0x7fc00000, 0x7f800001, 0xffc00123, 0x00000001, 0x007fffff, 0x00800000, 0x4f000000, 0xcf000000, 0x4effffff,
     0x5f000000, 0x5f800000, 0xdf000000, 0x3f000000, 0x40490fdb, 0x3effffff, 0x4b000000, 0x4b800001]
D = [0, 0x8000000000000000, f64(1.0), f64(-1.0), f64(1.5), f64(0.1), f64(3.0), 0x7fefffffffffffff, 0x7ff0000000000000,
     0xfff0000000000000, 0x7ff8000000000000, 0x7ff0000000000001, 0xfff8000000000123, 1, 0x000fffffffffffff,
     0x0010000000000000, 0x41e0000000000000, 0xc1e0000000000000, 0x41dfffffffc00000, 0x43e0000000000000,
     0x43f0000000000000, 0xc3e0000000000000, 0x3fe0000000000000, f64(3.141592653589793), 0x4330000000000000,
     0x4340000000000001, f32(1.0) | 0xffffffff00000000, 0x000000003f800000]
I = [0, 1, -1, 2, 255, 0x7fffffff, 0x80000000, 0xffffffff, 0x100000000, -0x80000000, 0x7fffffffffffffff,
     0x8000000000000000, 0xffffffffffffffff, 0x123456789abcdef0, 0xdeadbeef, 0x1000001, 0x20000001, 0x7ffffffffffffc00]
RMS = ["rne", "rtz", "rdn", "rup", "rmm"]

out = []
A = out.append
A("    .section .text\n    .align 2\n    .globl rv64_fp_body\nrv64_fp_body:\n    mv s11, ra\n    li s10, 0\n    li t0, 0x2000\n    csrs mstatus, t0")
def fold(r="a2"):
    A(f"    xor s10, s10, {r}\n    slli t6, s10, 1\n    srli s10, s10, 63\n    or s10, s10, t6")
def flags():
    A("    csrr a3, fflags\n    csrw fflags, zero"); fold("a3")
def ldS(fr, v): A(f"    li a0, {v}\n    fmv.w.x {fr}, a0")
def ldD(fr, v): A(f"    li a0, {v & 0xffffffffffffffff}\n    fmv.d.x {fr}, a0")
def ld(fmt, fr, v): (ldS if fmt == "s" else ldD)(fr, v)
def res(): A("    fmv.x.d a2, f3"); fold(); flags()

for fmt, V in (("s", S), ("d", D)):
    for op in ("fadd", "fsub", "fmul", "fdiv"):
        for rm in RMS:
            for a, b in itertools.product(V[::2] if fmt == "d" else V[:14], V[1::2] if fmt == "d" else V[:14]):
                ld(fmt, "f1", a); ld(fmt, "f2", b)
                A(f"    {op}.{fmt} f3, f1, f2, {rm}"); res()
    for rm in RMS:
        for a in V:
            ld(fmt, "f1", a); A(f"    fsqrt.{fmt} f3, f1, {rm}"); res()
    for op in ("fmadd", "fmsub", "fnmsub", "fnmadd"):
        for rm in ("rne", "rtz", "rdn", "rup"):
            for a, b, c in itertools.product(V[2:12:2], V[2:12:3], V[:12:3]):
                ld(fmt, "f1", a); ld(fmt, "f2", b); ld(fmt, "f4", c)
                A(f"    {op}.{fmt} f3, f1, f2, f4, {rm}"); res()
    for op in ("fsgnj", "fsgnjn", "fsgnjx", "fmin", "fmax"):
        for a, b in itertools.product(V, V[:14]):
            ld(fmt, "f1", a); ld(fmt, "f2", b); A(f"    {op}.{fmt} f3, f1, f2"); res()
    for op in ("feq", "flt", "fle"):
        for a, b in itertools.product(V, V[:14]):
            ld(fmt, "f1", a); ld(fmt, "f2", b); A(f"    {op}.{fmt} a2, f1, f2"); fold(); flags()
    for a in V:
        ld(fmt, "f1", a); A(f"    fclass.{fmt} a2, f1"); fold()
        A(f"    fmv.x.{'w' if fmt == 's' else 'd'} a2, f1"); fold()
    for cv in ("w", "wu", "l", "lu"):
        for rm in RMS:
            for a in V:
                ld(fmt, "f1", a); A(f"    fcvt.{cv}.{fmt} a2, f1, {rm}"); fold(); flags()
            for i in I:
                A(f"    li a0, {i}\n    fcvt.{fmt}.{cv} f3, a0, {rm}"); res()
    for i in I[:6]:
        A(f"    li a0, {i}\n    fmv.{'w' if fmt == 's' else 'd'}.x f3, a0"); res()
# S <-> D
for rm in RMS:
    for a in D:
        ldD("f1", a); A(f"    fcvt.s.d f3, f1, {rm}"); res()
for a in S:
    ldS("f1", a); A("    fcvt.d.s f3, f1"); res()
for a in D:   # the same on registers that are not NaN-boxed (fmv.d.x of a raw word): single ops read the canonical NaN
    ldD("f1", a); ldD("f2", a); A("    fadd.s f3, f1, f2"); res(); A("    fcvt.d.s f3, f1"); res()
    A("    fsgnj.s f3, f1, f2"); res()

# dynamic rounding mode (fcsr.frm) and the fcsr access instructions
for k, rm in enumerate((0, 1, 2, 3, 4)):
    A(f"    li a0, {rm}\n    fsrm a1\n    csrr a2, frm"); fold()
    for a, b in itertools.product(S[2:10], S[2:8]):
        ldS("f1", a); ldS("f2", b); A("    fadd.s f3, f1, f2"); res(); A("    fdiv.s f3, f1, f2"); res()
        A("    fcvt.w.s a2, f1"); fold(); flags()
A("    li a0, 0xe5\n    fscsr a1\n    frsr a2"); fold(); A("    csrrw a2, fflags, zero"); fold(); A("    csrrs a2, frm, zero"); fold()
A("    csrwi fflags, 0x1f\n    csrr a2, fcsr"); fold(); A("    csrwi frm, 0\n    csrwi fflags, 0")

# loads / stores
A("    la s0, scratch")
for k, d in enumerate([0x0123456789abcdef, 0xfedcba9876543210, 0x7ff8000000000000, 0xffffffff7fc00000, 0x3ff0000000000000]):
    A(f"    li a0, {d}\n    sd a0, {k*16}(s0)")
    A(f"    fld f3, {k*16}(s0)"); res()
    A(f"    flw f3, {k*16}(s0)"); res()
    A(f"    flw f3, {k*16+4}(s0)"); res()
    A(f"    fsd f3, {k*16+8}(s0)\n    ld a2, {k*16+8}(s0)"); fold()
    A(f"    fsw f3, {k*16+8}(s0)\n    lw a2, {k*16+8}(s0)\n    ld a3, {k*16+8}(s0)"); fold(); fold("a3")
A("    mv ra, s11\n    mv a0, s10\n    ret")
A("    .section .bss\n    .align 4\nscratch:\n    .space 256")

d = sys.argv[1]
os.makedirs(d, exist_ok=True)
open(os.path.join(d, "rv64-fp-helper.S"), "w").write("\n".join(out) + "\n")
open(os.path.join(d, "rv64-fp.c"), "w").write("extern long rv64_fp_body(void);\nint main(void) { rv64_fp_body(); return 0; }\n")
print(len(out), "lines")
