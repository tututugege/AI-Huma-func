#!/usr/bin/env python3
"""Generate directed RV64IMAB instruction tests (assembly) for function-model vs Spike.

Every instruction is executed on interesting operand values and its result is folded into a checksum; the oracle is Spike
(--diff compares all registers after every instruction), so the tests do not check values themselves.
usage: gen_isa.py <out-dir>      writes rv64-isa.c and rv64-isa-helper.S (build with trace-model/scripts/build_tests.sh)
"""
import sys, itertools, os

V = [0, 1, 2, 3, 0xff, 0x7f, 0x80, 0x7fff, 0x8000, 0xffff, 0x7fffffff, 0x80000000, 0xffffffff, 0x100000000,
     0x1ffffffff, 0x7fffffffffffffff, 0x8000000000000000, 0xffffffffffffffff, 0xfffffffffffffffe,
     0x123456789abcdef0, 0xdeadbeefcafebabe, 0x00000000ffff0000, 0xffff0000ffff0000, 31, 32, 63, 64,
     0x5555555555555555, 0xaaaaaaaaaaaaaaaa, 0x0f0f0f0f0f0f0f0f]

R3 = ("add sub sll slt sltu xor srl sra or and mul mulh mulhsu mulhu div divu rem remu addw subw sllw srlw sraw mulw "
      "divw divuw remw remuw packw andn orn xnor rol ror min minu max maxu pack packh rolw rorw add.uw sh1add sh2add sh3add "
      "sh1add.uw sh2add.uw sh3add.uw bclr bset binv bext clmul clmulr clmulh").split()
UN = "clz ctz cpop clzw ctzw cpopw sext.b sext.h zext.h rev8 orc.b".split()
IMM12 = "addi slti sltiu xori ori andi addiw".split()
IMMS = {"slli": 63, "srli": 63, "srai": 63, "rori": 63, "bseti": 63, "bclri": 63, "binvi": 63, "bexti": 63,
        "slliw": 31, "srliw": 31, "sraiw": 31, "roriw": 31, "slli.uw": 63}
IMM_VALS = [0, 1, -1, 2047, -2048, 0x555, -0x556]
SH_VALS = [0, 1, 7, 8, 15, 16, 31, 32, 33, 63]

out = []
A = out.append
A("    .section .text\n    .align 2\n    .globl rv64_isa_body\nrv64_isa_body:\n    mv s11, ra\n    li s10, 0")
def fold(rd="a2"):
    A(f"    xor s10, s10, {rd}\n    slli t6, s10, 1\n    srli s10, s10, 63\n    or s10, s10, t6")

for op in R3:
    for a, b in itertools.product(V, V):
        # keep the file a sane size: the full cross product for a small set, pairs otherwise
        pass
    pairs = list(itertools.product(V, V)) if op in ("add","sub","mul","mulh","mulhu","mulhsu","div","divu","rem","remu","addw","mulw","divw","remuw","sllw","sra","rol","pack","clmul","slt","sltu","sh1add.uw") else \
            [(a, b) for a in V for b in V[::3]]
    for a, b in pairs:
        A(f"    li a0, {a}\n    li a1, {b}\n    {op} a2, a0, a1")
        fold()
for op in UN:
    for a in V:
        A(f"    li a0, {a}\n    {op} a2, a0"); fold()
for op in IMM12:
    for a in V:
        for i in IMM_VALS:
            A(f"    li a0, {a}\n    {op} a2, a0, {i}"); fold()
for op, mx in IMMS.items():
    for a in V:
        for i in SH_VALS:
            if i > mx: continue
            A(f"    li a0, {a}\n    {op} a2, a0, {i}"); fold()
for a in V:
    A(f"    li a0, {a}\n    lui a2, 0x80000\n    lui a3, 0x7ffff\n    auipc a4, 0x12345")
    fold("a2"); fold("a3"); fold("a4")

# loads / stores on a scratch buffer
A("    la s0, scratch\n    li t0, 0")
pat = [0x0123456789abcdef, 0xfedcba9876543210, 0x8000000000000000, 0x00000000ffffffff, 0xffffffff80000000, 0x7f7f7f7f7f7f7f7f]
for k, d in enumerate(pat):
    A(f"    li a0, {d}\n    sd a0, {k*16}(s0)\n    sd a0, {k*16+8}(s0)")
    for off in range(0, 8):
        for ld in ("lb", "lbu"):
            A(f"    {ld} a2, {k*16+off}(s0)"); fold()
    for off in (0, 2, 4, 6):
        for ld in ("lh", "lhu"):
            A(f"    {ld} a2, {k*16+off}(s0)"); fold()
    for off in (0, 4):
        for ld in ("lw", "lwu"):
            A(f"    {ld} a2, {k*16+off}(s0)"); fold()
    A(f"    ld a2, {k*16}(s0)"); fold()
    # sub-word stores keep the neighbours
    A(f"    li a1, 0x1122334455667788\n    sb a1, {k*16+9}(s0)\n    sh a1, {k*16+10}(s0)\n    sw a1, {k*16+12}(s0)\n    ld a2, {k*16+8}(s0)")
    fold()
    A(f"    sd a1, {k*16+8}(s0)\n    ld a2, {k*16+8}(s0)"); fold()

# atomics
A("    la s1, amo_buf")
for w in ("w", "d"):
    for op in "amoswap amoadd amoxor amoand amoor amomin amomax amominu amomaxu".split():
        for m, o in itertools.product([0, 1, 0x7fffffff, 0x80000000, 0xffffffffffffffff, 0x8000000000000000, 0x123456789abcdef0],
                                      [0, 1, 0xffffffff, 0x80000000, 0xffffffffffffffff, 0x7fffffffffffffff, 0xdeadbeef]):
            A(f"    li a0, {m}\n    sd a0, 0(s1)\n    li a1, {o}\n    {op}.{w} a2, a1, (s1)\n    ld a3, 0(s1)")
            fold("a2"); fold("a3")
    A(f"    li a0, 0x1122334455667788\n    sd a0, 0(s1)\n    lr.{w} a2, (s1)\n    li a1, 0x99\n    sc.{w} a3, a1, (s1)\n    ld a4, 0(s1)\n    sc.{w} a5, a1, (s1)")
    fold("a2"); fold("a3"); fold("a4"); fold("a5")

# branches (taken and not taken)
for op in "beq bne blt bge bltu bgeu".split():
    for a, b in itertools.product(V[:20], V[:20]):
        A(f"    li a0, {a}\n    li a1, {b}\n    li a2, 0\n    {op} a0, a1, 1f\n    li a2, 1\n1:")
        fold()
A("    jal a2, 2f\n    li a2, 7\n2:")
fold()
A("    la a0, 3f\n    jalr a2, 0(a0)\n    li a2, 9\n3:")
fold()
A("    mv ra, s11\n    mv a0, s10\n    ret")
A("    .section .bss\n    .align 4\nscratch:\n    .space 256\namo_buf:\n    .space 64")

d = sys.argv[1]
os.makedirs(d, exist_ok=True)
open(os.path.join(d, "rv64-isa-helper.S"), "w").write("\n".join(out) + "\n")
open(os.path.join(d, "rv64-isa.c"), "w").write(
    "extern long rv64_isa_body(void);\nint main(void) { rv64_isa_body(); return 0; }\n")
print(len(out), "lines")
