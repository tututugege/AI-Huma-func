#!/usr/bin/env python3
"""C extension tests (function-model RVC=1 vs Spike, compared after every instruction).  No oracle values: Spike is the oracle.

Programs (each is rvc-<name>.c + rvc-<name>-helper.S; build with trace-model/scripts/build_tests.sh and a "c" in ARCH):
  rvc-alu      every 16-bit encoding that is neither a load/store nor a control transfer (valid, reserved and HINT ones),
               executed on random register values; an illegal one traps and the M-mode handler skips it (mtval = the 16-bit encoding)
  rvc-mem      every load/store encoding (c.lw/c.ld/c.sw/c.sd, the sp forms, and with FP the double/single forms) with
               distinct base registers and a random data buffer;  fd-rvc-mem = the same with mstatus.FS on (FP loads/stores run),
               rvc-mem with FS off (they trap illegal)
  rvc-flow     c.j / c.jal / c.beqz / c.bnez forward and backward over many offsets (taken and not taken, at 2- and 4-byte
               alignments), c.jr / c.jalr for every register, jalr / jal / branches to 2-byte aligned targets
  rvc-straddle 32-bit instructions that start at the last halfword of a cache line / a 4 KiB page (bare mode)
  rvc-sv39     (rv64) a 32-bit instruction that starts at the last halfword of a page under Sv39 in S-mode: the second halfword's page
               is valid / not executable / invalid / user-only / A=0: fetch page fault with mtval = pc + 2
usage: gen_rvc.py <out-dir> [xlen]
"""
import sys, os, random

XLEN = int(sys.argv[2]) if len(sys.argv) > 2 else 64
LD, SD = ("ld", "sd") if XLEN == 64 else ("lw", "sw")
W = XLEN // 8
rnd = random.Random(20261113)
OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)

INTERESTING = [0, 1, 2, 3, 7, 8, 0xff, 0x100, 0x7ff, 0x800, 0xfff, 0x7fff, 0x8000, 0xffff, 0x7fffffff, 0x80000000, 0xffffffff,
               0x100000000, 0x7fffffffffffffff, 0x8000000000000000, 0xffffffffffffffff, 0xfffffffffffffffe, 0xfffffffffffff000,
               0x00000000fffff000, 0x123456789abcdef0, 0xdeadbeefcafebabe, 0x5555555555555555, 0xaaaaaaaaaaaaaaaa]
M = (1 << XLEN) - 1

def rv():
    return (rnd.choice(INTERESTING) if rnd.random() < 0.5 else rnd.getrandbits(XLEN)) & M

def write(name, text, main_call=None):
    open(os.path.join(OUT, f"{name}-helper.S"), "w").write(text)
    open(os.path.join(OUT, f"{name}.c"), "w").write(
        f"extern long {name.replace('-', '_')}_body(void);\nint main(void) {{ {name.replace('-', '_')}_body(); return 0; }}\n")

def prologue(fn, fs=None):
    s = f"    .section .text\n    .align 2\n    .globl {fn}\n{fn}:\n    la t0, savera\n"
    s += "".join(f"    {SD} x{r}, {r * W}(t0)\n" for r in range(1, 32) if r != 5)
    s += "    la t0, handler\n    csrw mtvec, t0\n"
    if fs is not None:
        s += "    li t0, 0x6000\n    csrc mstatus, t0\n" + (f"    li t0, {fs << 13}\n    csrs mstatus, t0\n" if fs else "")
    s += "    j start\n    .align 2\nhandler:\n    csrr t5, mtval\n    andi t6, t5, 3\n    addi t6, t6, -3\n    csrr t5, mepc\n" \
         "    beqz t6, 1f\n    addi t5, t5, 2\n    j 2f\n1:  addi t5, t5, 4\n2:  csrw mepc, t5\n    mret\nstart:\n"
    return s

def epilogue(extra=""):
    return f"""    la t0, savera
""" + "".join(f"    {LD} x{r}, {r * W}(t0)\n" for r in range(1, 32) if r != 5) + f"""    ret
{extra}    .section .bss
    .align 4
savera:
    .space 256
"""

def is_flow(h):
    q, f3 = h & 3, h >> 13
    if q == 1 and f3 in (5, 6, 7): return True
    if q == 1 and f3 == 1 and XLEN == 32: return True          # c.jal
    if q == 2 and f3 == 4 and ((h >> 2) & 31) == 0: return True  # c.jr / c.jalr / c.ebreak
    return False

def is_mem(h):
    q, f3 = h & 3, h >> 13
    return (q == 0 and f3 in (1, 2, 3, 5, 6, 7)) or (q == 2 and f3 in (1, 2, 3, 5, 6, 7))

def table_text(rows, width=32):
    t = ["    .section .data\n    .align 3\ntab:"]
    for _ in range(rows):
        t.append("    .dword " + ",".join(str(rv() & 0xffffffffffffffff) for _ in range(width)))
    return "\n".join(t) + "\n"

# ---------------------------------------------------------------------------------------------------------- alu
def gen_alu():
    cases = [h for h in range(0x10000) if (h & 3) != 3 and not is_flow(h) and not is_mem(h)]
    rows = 64
    s = prologue(f"rvc_alu_body")
    for i, h in enumerate(cases):
        s += f"    la t6, tab + {(i % rows) * 256}\n"
        for r in range(1, 31):
            s += f"    {LD} x{r}, {r * 8}(t6)\n"
        s += f"    .hword {h}\n    .hword 0x0001\n"
    s += epilogue(table_text(rows, 32))
    write("rvc-alu", s)
    return len(cases)

# ---------------------------------------------------------------------------------------------------------- mem
def gen_mem(name, fs):
    cases = [h for h in range(0x10000) if (h & 3) != 3 and is_mem(h)]
    s = prologue(f"{name.replace('-', '_')}_body", fs)
    if fs:
        for f in range(32):
            s += f"    la t0, mbuf + {(f * 8) % 1024}\n    fld f{f}, 0(t0)\n"
    chk = 0
    def checksum():
        # read the whole buffer back (order-sensitive fold): a store to a wrong address or with wrong data shows up in a register
        nonlocal chk
        chk += 1
        return (f"    la t0, mbuf\n    li t2, 256\n    li t1, 0\nC{chk}:\n    {LD} t3, 0(t0)\n    slli t4, t1, 1\n    add t1, t1, t4\n"
                f"    add t1, t1, t3\n    addi t0, t0, 8\n    addi t2, t2, -1\n    bnez t2, C{chk}\n")
    for idx, h in enumerate(cases):
        if idx and idx % 16 == 0:
            s += checksum()
        s += "    la x31, mbuf\n"
        for i, r in enumerate(range(8, 16)):
            s += f"    addi x{r}, x31, {16 * i}\n"
        s += "    addi x2, x31, 512\n"
        s += f"    .hword {h}\n    .hword 0x0001\n"
    s += checksum()
    data = ["    .section .data\n    .align 3\nmbuf:"]
    for _ in range(256):
        data.append(f"    .dword {rnd.getrandbits(64)}")
    s += epilogue("\n".join(data) + "\n")
    write(name, s)
    return len(cases)

# ---------------------------------------------------------------------------------------------------------- flow
def c_j(o, jal=False):
    # c.j / c.jal imm[11|4|9:8|10|6|7|3:1|5] -> inst[12:2]
    u = o & 0xfff
    b = lambda n: (u >> n) & 1
    f = (b(11) << 12) | (b(4) << 11) | (((u >> 8) & 3) << 9) | (b(10) << 8) | (b(6) << 7) | (b(7) << 6) | (((u >> 1) & 7) << 3) | (b(5) << 2)
    return ((1 if jal else 5) << 13) | f | 1

def c_br(o, rs, bnez):
    u = o & 0x1ff
    b = lambda n: (u >> n) & 1
    return ((7 if bnez else 6) << 13) | (b(8) << 12) | (((u >> 3) & 3) << 10) | ((rs - 8) << 7) | (((u >> 6) & 3) << 5) | (((u >> 1) & 3) << 3) | (b(5) << 2) | 1

def gen_flow():
    s = prologue("rvc_flow_body")
    n = [0]
    def lab():
        n[0] += 1
        return f"L{n[0]}"
    def nop16(): return "    .hword 0x0001\n"
    def add10(): return "    .hword 0x0505\n"       # c.addi a0, 1
    def add9(): return "    .hword 0x0485\n"        # c.addi s1, 1
    def jr_a0(): return "    .hword 0x8502\n"       # c.jr a0
    # forward c.beqz / c.bnez, both conditions, both alignments
    for bnez in (0, 1):
        for taken in (0, 1):
            for o in range(2, 255, 2):
                for al in (0, 1):
                    s += "    li x8, %d\n" % ((1 if bnez else 0) if taken else (0 if bnez else 1))
                    if al: s += nop16()
                    s += f"    .hword {c_br(o, 8, bnez)}\n"
                    for k in range(1, o // 2):
                        s += add10()
                    s += add9()
    # backward c.beqz / c.bnez: jump forward to the branch, taken backwards to a c.jr that leaves; not taken falls through
    for bnez in (0, 1):
        for taken in (0, 1):
            for o in range(-256, 0, 2):
                for al in (0, 1):
                    end = lab()
                    s += f"    la a0, {end}\n    li x8, %d\n" % ((1 if bnez else 0) if taken else (0 if bnez else 1))
                    s += "    .option push\n    .option norvc\n    j %sB\n    .option pop\n" % end
                    if al: s += nop16()
                    # T = branch address + o ; between T and the branch there are -o/2 halfwords (T included)
                    s += jr_a0()
                    for k in range(1, -o // 2): s += nop16()
                    s += f"{end}B:\n    .hword {c_br(o, 8, bnez)}\n    .hword 0x0001\n{end}:\n"
    # c.j forward / backward (sampled), c.jal on rv32 likewise
    offs = sorted(set(list(range(2, 130, 2)) + [1 << k for k in range(1, 11)] + [(1 << k) + 2 for k in range(1, 11)] +
                      [2046, 2044, 2042, 1022, 1024 + 6, 1366, 682, 1638, 410] + list(range(130, 2046, 38))))
    for jal in ((0, 1) if XLEN == 32 else (0,)):
        for o in offs:
            for al in (0, 1):
                if al: s += nop16()
                s += f"    .hword {c_j(o, jal)}\n"
                for k in range(1, o // 2): s += add10()
                s += add9()
        for o in offs:
            for al in (0, 1):
                end = lab()
                s += f"    la a0, {end}\n"
                s += "    .option push\n    .option norvc\n    j %sB\n    .option pop\n" % end
                if al: s += nop16()
                s += jr_a0()
                for k in range(1, o // 2): s += nop16()
                s += f"{end}B:\n    .hword {c_j(-o, jal)}\n    .hword 0x0001\n{end}:\n"
    # c.jr / c.jalr for every register, targets at 2- and 4-byte alignment, with bit 0 set (cleared by the jump)
    for r in range(1, 32):
        for al in (0, 1):
            for bit0 in (0, 1):
                t = lab()
                s += f"    la x{r}, {t}\n"
                if bit0: s += f"    addi x{r}, x{r}, 1\n"
                s += "    .hword %d\n" % (0x8002 | (r << 7))            # c.jr xr
                s += add10() * 3
                if al: s += nop16()
                s += f"{t}:\n" + add9()
                t = lab()
                s += f"    la x{r}, {t}\n"
                if bit0: s += f"    addi x{r}, x{r}, 1\n"
                s += "    .hword %d\n" % (0x9002 | (r << 7))            # c.jalr xr
                s += add10() * 3
                if al: s += nop16()
                s += f"{t}:\n" + add9()
    # 32-bit control transfers to 2-byte aligned targets (legal with C)
    for al in (0, 1):
        for kind in ("jal", "jalr", "beq", "bne", "blt", "bgeu"):
            t = lab()
            if kind == "jal": s += f"    .option push\n    .option norvc\n    jal a2, {t}\n    .option pop\n"
            elif kind == "jalr": s += f"    la a3, {t}\n    .option push\n    .option norvc\n    jalr a2, 3(a3)\n    .option pop\n"   # (a3 + 3) & ~1 = a3 + 2 if a3 is 4-aligned...
            else: s += f"    li a3, 1\n    li a4, 1\n    .option push\n    .option norvc\n    {kind} a3, a4, {t}\n    .option pop\n"
            s += add10() * 3
            if al: s += nop16()
            s += f"{t}:\n" + add9()
    s += epilogue()
    write("rvc-flow", s)

# ---------------------------------------------------------------------------------------------------------- straddle
def gen_straddle():
    s = prologue("rvc_straddle_body")
    ins = ["addi a0, a0, 1", "lui a1, 0x12345", "auipc a2, 1", "add a3, a3, a0", "csrr a4, mscratch", "csrw mscratch, a0", "mul a5, a0, a0",
           "slli a6, a0, 5", "lw a7, 0(s2)", "sw a0, 0(s2)", "fence", "fence.i", "ld s3, 0(s2)" if XLEN == 64 else "lw s3, 0(s2)"]
    s += "    la s2, sbuf\n"
    k = 0
    def at(boundary, body, name):
        # jump to a 32-bit instruction whose first halfword is the last halfword before `boundary` (page 4096 / line 64)
        nonlocal s
        s += f"    la t0, X{name}\n    jr t0\n"
        s += f"    .balign {boundary}\n    .skip {boundary - 2}\nX{name}:\n"
        s += body
        s += "    .option push\n    .option norvc\n"
        s += f"    j E{name}\n    .option pop\n"
        s += f"E{name}:\n"
    for b in (64, 4096):
        for i in ins:
            k += 1
            at(b, f"    .option push\n    .option norvc\n    {i}\n    .option pop\n    addi a1, a1, 1\n", f"{k}")
        # jal / branches / jalr straddling
        k += 1
        at(b, f"    .option push\n    .option norvc\n    jal a2, T{k}\n    .option pop\n    addi a0, a0, 7\nT{k}:\n    addi a1, a1, 1\n", f"{k}")
        k += 1
        at(b, f"    li a3, 1\n    .option push\n    .option norvc\n    beq a3, a3, T{k}\n    .option pop\n    addi a0, a0, 7\nT{k}:\n    addi a1, a1, 1\n", f"{k}")
    s += epilogue("    .section .bss\n    .align 4\nsbuf:\n    .space 64\n")
    write("rvc-straddle", s)


# ---------------------------------------------------------------------------------------------------------- sv39 straddle
def gen_sv39():
    V, R, W_, X, U, A, D = 1, 2, 4, 8, 0x10, 0x40, 0x80
    s = "    .section .text\n    .align 2\n    .globl rvc_sv39_body\nrvc_sv39_body:\n"
    s += "    la t0, savera\n" + "".join(f"    {SD} x{r}, {r * W}(t0)\n" for r in range(1, 32) if r != 5)
    s += "    la t0, trap\n    csrw mtvec, t0\n    j start\n    .align 2\ntrap:\n    li t0, 0x1800\n    csrs mstatus, t0\n    csrw mepc, s8\n    mret\nstart:\n"
    s += "    la s0, root\n    la s1, l1\n    la s2, l0\n"
    s += "    srli t2, s1, 12\n    slli t2, t2, 10\n    ori t2, t2, 1\n    sd t2, 8(s0)\n"      # root[1] -> l1 (VA 0x40000000)
    s += "    srli t2, s2, 12\n    slli t2, t2, 10\n    ori t2, t2, 1\n    sd t2, 0(s1)\n"      # l1[0] -> l0
    s += f"    la t4, pageA\n    srli t2, t4, 12\n    slli t2, t2, 10\n    ori t2, t2, {V | R | X | A}\n    sd t2, 0(s2)\n"   # l0[0] = page A (S-mode executable)
    s += "    srli t2, s0, 12\n    li t1, 1\n    slli t1, t1, 63\n    or t2, t2, t1\n    csrw satp, t2\n"
    variants = [V | X | A, V | R | A, 0, V | X | U | A, V | X, V | R | X | A | D, V | R | W_ | A | D]
    for k, fl in enumerate(variants):
        s += "    la t4, pageB\n"
        if fl == 0:
            s += "    sd zero, 8(s2)\n"
        else:
            s += f"    srli t2, t4, 12\n    slli t2, t2, 10\n    ori t2, t2, {fl}\n    sd t2, 8(s2)\n"
        s += "    sfence.vma\n    li t0, 0x1800\n    csrc mstatus, t0\n    li t0, 0x800\n    csrs mstatus, t0\n"
        s += f"    li t0, 0x40000ffe\n    csrw mepc, t0\n    la s8, D{k}\n    mret\nD{k}:\n"
    s += "    csrw satp, zero\n"
    s += "    la t0, savera\n" + "".join(f"    {LD} x{r}, {r * W}(t0)\n" for r in range(1, 32) if r != 5) + "    ret\n"
    s += "    .balign 4096\npageA:\n    .skip 4094\n    .hword 0x0513\n"                    # first half of addi a0, a0, 1
    s += "pageB:\n    .hword 0x0015\n    .hword 0x0073\n    .hword 0x0000\n    .skip 4090\n"  # its second half, then ecall
    s += "    .section .bss\n    .balign 4096\nroot:\n    .space 4096\nl1:\n    .space 4096\nl0:\n    .space 4096\nsavera:\n    .space 256\n"
    write("rvc-sv39", s)

n_alu = gen_alu()
n_mem = gen_mem("rvc-mem", 0)
gen_mem("fd-rvc-mem", 1)
gen_flow()
gen_straddle()
if XLEN == 64: gen_sv39()
print("rvc-alu", n_alu, "encodings; rvc-mem", n_mem, "encodings")
