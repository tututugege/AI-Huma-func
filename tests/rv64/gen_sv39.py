#!/usr/bin/env python3
"""Generate a directed Sv39 test (assembly) for function-model vs Spike (--diff is the oracle).

M-mode code builds page tables and runs loads/stores through MPRV (S and U data privilege, SUM/MXR combinations) over leaf
PTEs with all kinds of permission/A/D/reserved-bit variants, 2 MiB / 1 GiB superpages (aligned and misaligned),
non-canonical addresses, and instruction fetches through mret into S/U mode.  The trap handler logs mcause/mtval and resumes.
usage: gen_sv39.py <out-dir>      writes rv64-sv39.c and rv64-sv39-helper.S
"""
import sys, os
V,R,W,X,U,G,A,D = 1,2,4,8,0x10,0x20,0x40,0x80
PBMT = 1 << 61
VARIANTS = [V|R|A, V|R|W|A|D, V|R|W|X|A|D, V|X|A, V|W|X|A|D, V|R|W|U|A|D, V|R|U|A, V|R|W|D, V|R|W|A,
            V|R|W|X|U|A|D, 0, V|R|W|A|D|G, (V|R|W|A|D)|PBMT, V|X|U|A, V|R|X|A, V|R|W|U|X|D]
N = len(VARIANTS)
MPRV, SUM, MXR = 1 << 17, 1 << 18, 1 << 19
MPP_MASK = 3 << 11

o = []
A_ = o.append
A_("""    .section .text
    .align 2
    .globl rv64_sv39_body
rv64_sv39_body:
    mv s11, ra
    li s10, 0
    li s9, 0
    la t2, trap
    csrw mtvec, t2
    la s8, 1f            # M-mode landing label for the fetch tests (set again before each use)
1:
    # ---- page tables ----
    la s0, root
    la s1, l1
    la s2, l0
    la s3, data_pages
    la s4, code_page
    la s5, mega
""")
def pte_ptr(reg_src, tmp="t2"):
    return f"    srli {tmp}, {reg_src}, 12\n    slli {tmp}, {tmp}, 10\n    ori {tmp}, {tmp}, 1\n"
def pte_leaf(reg_src, flags, tmp="t2", extra=0):
    s = f"    srli {tmp}, {reg_src}, 12\n    slli {tmp}, {tmp}, 10\n"
    if extra:
        s += f"    li t3, {flags | extra}\n    or {tmp}, {tmp}, t3\n"
    else:
        s += f"    ori {tmp}, {tmp}, {flags}\n"
    return s
# root: [1] -> l1 (VA 0x40000000), [2] = gigapage identity (VA 0x80000000), [3] = misaligned gigapage
A_(pte_ptr("s1") + "    sd t2, 8(s0)\n    li t4, 0x808\n    add t4, s0, t4\n    sd t2, 0(t4)")   # root[1] and root[0x101] (the non-canonical alias of VA 0x40000000)
A_("    li t2, 0x20000000\n    slli t2, t2, 0\n    li t2, %d\n    sd t2, 16(s0)" % (((0x80000000 >> 12) << 10) | V|R|W|X|A|D))
A_("    li t2, %d\n    sd t2, 24(s0)" % ((((0x80000000 + 0x1000) >> 12) << 10) | V|R|W|X|A|D))   # misaligned gigapage
# l1: [0] -> l0 (data variants at 0x40000000, code variants at 0x40100000 via l0b), [2] mega RW, [3] misaligned mega, [4] mega X-only
A_(pte_ptr("s2") + "    sd t2, 0(s1)")
A_("    la t4, l0b")
A_(pte_ptr("t4") + "    sd t2, 8(s1)")                                   # l1[1] -> l0b : VA 0x40200000 code variants
A_(pte_leaf("s5", V|R|W|A|D) + "    sd t2, 16(s1)")                    # l1[2] : VA 0x40400000 mega RW
A_("    li t4, 0x1000\n    add t4, s5, t4")
A_(pte_leaf("t4", V|R|W|A|D) + "    sd t2, 24(s1)")                    # l1[3] : misaligned mega -> fault
A_(pte_leaf("s5", V|X|A) + "    sd t2, 32(s1)")                        # l1[4] : VA 0x40800000 mega X-only
# l0 (data pages) and l0b (code pages)
for i, fl in enumerate(VARIANTS):
    A_(f"    li t4, {i*4096}\n    add t4, s3, t4")
    hi = fl & PBMT
    A_(pte_leaf("t4", fl & 0xff, extra=hi) + f"    sd t2, {i*8}(s2)")
    if fl == 0:
        A_(f"    sd zero, {i*8}(s2)")
    A_(pte_leaf("s4", fl & 0xff, extra=hi) + "    la t4, l0b\n" + f"    sd t2, {i*8}(t4)")
    if fl == 0:
        A_(f"    la t4, l0b\n    sd zero, {i*8}(t4)")
# kernel-style high mapping (Linux rv64: VA 0xffffffff80000000 through root[510] -> a level-1 table -> a 2 MiB leaf)
A_("    la t4, l1k")
A_(pte_ptr("t4") + "    li t4, 4080\n    add t4, s0, t4\n    sd t2, 0(t4)")
A_(pte_leaf("s5", V|R|W|X|A|D) + "    la t4, l1k\n    sd t2, 0(t4)")
A_("    li a1, 0x1122334455667788\n    li t5, 0x0123456789abcdef")
# satp: bare first (no effect on M), then Sv39 with an ASID, then an unsupported mode (write ignored)
A_("""    srli t2, s0, 12
    li t3, 8
    slli t3, t3, 60
    or t2, t2, t3
    li t3, 0x1234
    slli t3, t3, 44
    or t2, t2, t3
    csrw satp, t2
    csrr a2, satp
""")
A_("    sfence.vma")
def fold(rd="a2"):
    A_(f"    xor s10, s10, {rd}\n    slli t6, s10, 1\n    srli s10, s10, 63\n    or s10, s10, t6")
fold("a2")
def set_status(priv, sum_, mxr):
    bits = MPRV | (priv << 11) | (SUM if sum_ else 0) | (MXR if mxr else 0)
    A_(f"    li t2, {MPRV | MPP_MASK | SUM | MXR}\n    csrc mstatus, t2\n    li t2, {bits}\n    csrs mstatus, t2")
def clear_status():
    A_(f"    li t2, {MPRV | MPP_MASK | SUM | MXR}\n    csrc mstatus, t2")
va_data = lambda i: 0x40000000 + i * 4096
def acc(priv, sum_, mxr, *insns):
    """one translated access per instruction: a trap leaves MPP = M, so mstatus is set up again before every access"""
    for ins in insns:
        set_status(priv, sum_, mxr)
        A_("    " + ins)
for priv in (1, 0):
    for sum_ in (0, 1):
        for mxr in (0, 1):
            for i in range(N):
                A_(f"    li s6, {va_data(i)}")
                acc(priv, sum_, mxr, "ld a2, 0(s6)")
                fold("a2")
                acc(priv, sum_, mxr, "sd a1, 8(s6)")
                acc(priv, sum_, mxr, "lw a3, 8(s6)")
                fold("a3")
                acc(priv, sum_, mxr, "sb a1, 17(s6)")
                acc(priv, sum_, mxr, "lhu a4, 16(s6)")
                fold("a4")
                acc(priv, sum_, mxr, "lr.w a2, (s6)", "sc.w a3, a1, (s6)", "amoadd.d a4, a1, (s6)")
                fold("a2"); fold("a3"); fold("a4")
            # superpages and bad addresses
            for va in (0x40400000, 0x40400000 + 0x1ff000, 0x40600000, 0x80000000, 0xc0000000, 0x40800000, 0x40801008,
                       0x0000004000000000, 0xffffffc000000000, (1 << 38) | 0x40000000, 0xffffffc040000000, 0x0, 0x3ffffff8, 0x40000000 - 8):
                A_(f"    li s6, {va}")
                acc(priv, sum_, mxr, "ld a2, 0(s6)")
                acc(priv, sum_, mxr, "sd a1, 0(s6)")
                acc(priv, sum_, mxr, "ld a3, 0(s6)")
                fold("a2"); fold("a3")
            clear_status()
# instruction fetch through mret
A_("    la s4, code_page\n    sd zero, 0(s5)\n    sd zero, 8(s5)")   # the X-only superpage executes mega[0]: keep it a clean illegal word
for priv in (1, 0):
    for sum_ in (0,):
        for base, label in ((0x40200000, "l1b"),):
            for i in range(N):
                A_(f"    la s8, 2{priv}{i:02d}f\n    li t2, {base + i*4096}\n    csrw mepc, t2\n    li t2, {MPP_MASK}\n    csrc mstatus, t2\n    li t2, {priv << 11}\n    csrs mstatus, t2\n    mret\n2{priv}{i:02d}:")
                A_("    nop")
            # mega X-only, misaligned and giga fetch
            for va in (0x40800000, 0x40400000, "CODE", 0xc0000000):
                getva = "la t2, code_page" if va == "CODE" else f"li t2, {va}"
                A_(f"    la s8, 3f\n    {getva}\n    csrw mepc, t2\n    li t2, {MPP_MASK}\n    csrc mstatus, t2\n    li t2, {priv << 11}\n    csrs mstatus, t2\n    mret\n3:\n    nop")
fold("s9")
# fetch through the kernel-style high mapping (2 MiB leaf at level 1)
A_("    li t2, 0x73\n    sw t2, 0(s5)\n    fence.i")
for priv in (1, 0):
    A_(f"    la s8, 6f\n    li t2, 0xffffffff80000000\n    csrw mepc, t2\n    li t2, {MPP_MASK}\n    csrc mstatus, t2\n    li t2, {priv << 11}\n    csrs mstatus, t2\n    mret\n6:\n    nop")
# satp: unsupported mode is ignored
A_("""    li t2, 9
    slli t2, t2, 60
    csrw satp, t2
    csrr a2, satp
""")
fold("a2")
A_("    csrw satp, zero\n    csrr a2, satp")
fold("a2")
A_("    mv ra, s11\n    mv a0, s10\n    ret")
# trap handler: log, resume (fetch faults / ecalls resume at the landing label in M-mode)
A_("""    .align 4
trap:
    csrrw zero, mscratch, t0
    csrr t0, mcause
    xor s9, s9, t0
    slli s9, s9, 1
    csrr t0, mtval
    add s9, s9, t0
    csrr t0, mcause
    li t1, 12
    beq t0, t1, 4f
    li t1, 8
    beq t0, t1, 4f
    li t1, 9
    beq t0, t1, 4f
    li t1, 2
    beq t0, t1, 4f
    csrr t0, mepc
    addi t0, t0, 4
    csrw mepc, t0
    j 5f
4:  csrw mepc, s8
    li t0, 0x1800
    csrs mstatus, t0
5:  csrr t0, mscratch
    mret
""")
# code page: a few instructions that complete fast; landing is via ecall
A_("""    .section .text
    .balign 4096
code_page:
    ecall
    nop
    nop
    .section .bss
    .balign 4096
root:  .space 4096
l1:    .space 4096
l0:    .space 4096
l0b:   .space 4096
l1k:   .space 4096
data_pages: .space %d
    .balign 0x200000
mega:  .space 0x200000
""" % (N * 4096))
d = sys.argv[1]; os.makedirs(d, exist_ok=True)
open(os.path.join(d, "rv64-sv39-helper.S"), "w").write("\n".join(o) + "\n")
open(os.path.join(d, "rv64-sv39.c"), "w").write("extern long rv64_sv39_body(void);\nint main(void) { rv64_sv39_body(); return 0; }\n")
print(len(o), "chunks")
