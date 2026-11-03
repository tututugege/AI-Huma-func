#!/usr/bin/env python3
"""Front-door checkpoint restore: turn a function-model checkpoint into a restore program.

The program is a flat binary for the boot window (--flash-image, address 0). The CPU starts from reset (M mode, all
registers 0) and executes it like any other code: it writes the devices (timer, UART, interrupt controller) and the
CSRs, loads the 31 registers with immediates and ends with `mret` into the checkpoint's pc and privilege. Only the
memory of the checkpoint is loaded by the simulator (--restore-mem-only); no architectural state is injected.

What cannot be reproduced exactly: mret leaves mstatus.MPP = U and MPIE = 1 and mepc = the target pc. For an S or U
target this is unobservable (every M-mode trap rewrites them). For an M-mode target (checkpoint inside OpenSBI) it is not
exact; the tool warns.

usage: ckpt2stub.py <checkpoint.gz> <out.bin> [--info]
"""
import gzip, struct, sys, zlib

TIMER_BASE, UART_BASE, INTC_BASE = 0x1FD0E000, 0x10000000, 0x1FB00000
# stored CSR array order (enum_csr in include/CSR.h)
CSR_IDX = dict(mtvec=0, mepc=1, mcause=2, mie=3, mip=4, mtval=5, mscratch=6, mstatus=7, mideleg=8, medeleg=9,
               sepc=10, stvec=11, scause=12, sscratch=13, stval=14, sstatus=15, sie=16, sip=17, satp=18, mhartid=19, misa=20)
CSR_NUM = dict(mtvec=0x305, mepc=0x341, mcause=0x342, mie=0x304, mip=0x344, mtval=0x343, mscratch=0x340, mstatus=0x300,
               mideleg=0x303, medeleg=0x302, sepc=0x141, stvec=0x105, scause=0x142, sscratch=0x140, stval=0x143, satp=0x180)
# restored in this order; sstatus/sie/sip are shadows of mstatus/mie/mip, mhartid/misa are constants
CSR_ORDER = ["mtvec", "mcause", "mtval", "mscratch", "mideleg", "medeleg", "sepc", "stvec", "scause", "sscratch",
             "stval", "satp", "mie", "mip"]

MSTATUS_MIE, MSTATUS_MPIE, MSTATUS_MPP = 0x8, 0x80, 0x1800


def read_checkpoint(path):
    f = gzip.open(path, "rb")
    magic, ver, priv, ram_size, io_count = struct.unpack("<IHHII", f.read(16))
    sdram_size = struct.unpack("<I", f.read(4))[0] if ver >= 3 else 0
    st = f.read(236)
    vals = struct.unpack("<32I21I4I??2xI", st[:128 + 84 + 16 + 2 + 2 + 4])
    gpr, csr = list(vals[0:32]), list(vals[32:53])
    pc, store_addr, store_data, store_strb, store, reserve_valid, reserve_addr = vals[53:60]
    interval = struct.unpack("<Q", f.read(8))[0]
    left = ram_size + sdram_size
    while left:  # skip the memory (mostly zeros: fast)
        n = len(f.read(min(left, 1 << 26)))
        if n == 0:
            raise SystemExit("truncated checkpoint")
        left -= n
    io = {}
    for _ in range(io_count):
        base, size = struct.unpack("<II", f.read(8))
        io[base] = f.read(size)
    return dict(priv=priv & 3, gpr=gpr, csr=csr, pc=pc, reserve_valid=bool(reserve_valid), reserve_addr=reserve_addr,
                interval=interval, io=io)


# ---- a tiny RV32 encoder: only what the restore program needs ----
def u32(x): return x & 0xFFFFFFFF
def lui(rd, imm20): return u32((imm20 << 12) | (rd << 7) | 0x37)
def addi(rd, rs1, imm): return u32(((imm & 0xFFF) << 20) | (rs1 << 15) | (rd << 7) | 0x13)
def csrw(csr, rs1): return u32((csr << 20) | (rs1 << 15) | (1 << 12) | 0x73)
def sw(rs2, rs1, off): return u32((((off >> 5) & 0x7F) << 25) | (rs2 << 20) | (rs1 << 15) | (2 << 12) | ((off & 0x1F) << 7) | 0x23)
def sb(rs2, rs1, off): return u32((((off >> 5) & 0x7F) << 25) | (rs2 << 20) | (rs1 << 15) | (0 << 12) | ((off & 0x1F) << 7) | 0x23)
def lr_w(rd, rs1): return u32((0b00010 << 27) | (rs1 << 15) | (2 << 12) | (rd << 7) | 0x2F)
MRET = 0x30200073


def li(rd, val):
    val = u32(val)
    sval = val - (1 << 32) if val & 0x80000000 else val
    if -2048 <= sval < 2048:
        return [addi(rd, 0, sval)]
    hi = ((val + 0x800) >> 12) & 0xFFFFF
    lo = u32(val - (hi << 12))
    lo = lo - (1 << 32) if lo & 0x80000000 else lo
    out = [lui(rd, hi)]
    if lo:
        out.append(addi(rd, rd, lo))
    return out


T0, T1 = 5, 6  # temporaries (x5, x6): restored with the other registers at the end


def build(ck, warn):
    prog = []
    rd_words = lambda base, off: struct.unpack("<I", ck["io"].get(base, b"\0" * 4096)[off:off + 4].ljust(4, b"\0"))[0]
    # ---- devices ----
    timer = ck["io"][TIMER_BASE]
    mtime = struct.unpack("<I", timer[0:4])[0] | (struct.unpack("<I", timer[4:8])[0] << 32)
    cmp_lo, cmp_hi = struct.unpack("<II", timer[8:16])
    uart = ck["io"][UART_BASE]
    lcr = uart[3]
    if lcr & 0x80:
        warn("UART DLAB is set in the checkpoint: divisor latches are not restored")
    uart_prog = li(T0, UART_BASE)
    for off, byte in ((3, lcr & 0x7F), (4, uart[4]), (7, uart[7]), (1, uart[1])):  # LCR, MCR, SCR, IER
        if byte or off == 3:
            uart_prog += li(T1, byte) + [sb(T1, T0, off)]
    intc = ck["io"].get(INTC_BASE, b"")
    intc_prog = []
    for off in (0x08, 0x1C):  # IER, MER
        v = struct.unpack("<I", intc[off:off + 4])[0] if len(intc) >= off + 4 else 0
        if v:
            intc_prog += li(T0, INTC_BASE) + li(T1, v) + [sw(T1, T0, off)]
    if len(intc) >= 4 and struct.unpack("<I", intc[0:4])[0]:
        warn("interrupt controller ISR is non-zero: not restored")

    # ---- CSRs ----
    csr_prog = []
    for name in CSR_ORDER:
        v = ck["csr"][CSR_IDX[name]]
        if v:
            csr_prog += li(T0, v) + [csrw(CSR_NUM[name], T0)]
    prio = ck["priv"]
    m0 = ck["csr"][CSR_IDX["mstatus"]]
    # after mret: MIE = MPIE, MPIE = 1, MPP = U, privilege = MPP. MIE stays 0 while the program runs.
    x = (m0 & ~(MSTATUS_MIE | MSTATUS_MPIE | MSTATUS_MPP)) | (prio << 11) | (MSTATUS_MPIE if m0 & MSTATUS_MIE else 0)
    if prio == 3:
        warn("M-mode checkpoint: mstatus.MPP/MPIE and mepc are not restored exactly (mret overwrites them)")
    csr_prog += li(T0, ck["pc"]) + [csrw(CSR_NUM["mepc"], T0)]
    csr_prog += li(T0, x) + [csrw(CSR_NUM["mstatus"], T0)]

    # ---- reservation and registers ----
    # The timer is written last, after every register except x5/x6 (its temporaries) is restored: a DUT counts mtime in
    # cycles, a reference in instructions, and the fewer instructions run after the timer write the smaller the
    # difference. mtime must read `mtime` at the first instruction of the target: the value written is lower by the number
    # of instructions that still run (the store itself included, mret included).
    pre = []
    if ck["reserve_valid"]:
        pre += li(T0, ck["reserve_addr"]) + [lr_w(0, T0)]
    for i in range(1, 32):
        if i in (T0, T1):
            continue
        v = ck["gpr"][i]
        if v:
            pre += li(i, v)
    post = li(T0, ck["gpr"][T0]) + li(T1, ck["gpr"][T1]) + [MRET]

    def timer_prog(adj):
        v = (mtime - adj) & ((1 << 64) - 1)
        return (li(T0, TIMER_BASE) + li(T1, cmp_lo) + [sw(T1, T0, 8)] + li(T1, cmp_hi) + [sw(T1, T0, 12)] +
                li(T1, v >> 32) + [sw(T1, T0, 4)] + li(T1, v & 0xFFFFFFFF) + [sw(T1, T0, 0)])

    # the lo store is the last instruction of timer_prog; after it `post` runs, then the target starts
    adj = len(post) + 1
    prog = uart_prog + intc_prog + csr_prog + pre + timer_prog(adj) + post
    return prog, dict(mtime=mtime, cmp=cmp_lo | (cmp_hi << 32), adj=adj, pc=ck["pc"], priv=prio)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    ck = read_checkpoint(sys.argv[1])
    warnings = []
    prog, info = build(ck, warnings.append)
    open(sys.argv[2], "wb").write(b"".join(struct.pack("<I", w) for w in prog))
    for w in warnings:
        print("warning:", w)
    print("restore program: %d instructions (%d bytes); target pc=0x%08x priv=%d; mtime=%d mtimecmp=0x%x (written %d lower)"
          % (len(prog), 4 * len(prog), info["pc"], info["priv"], info["mtime"], info["cmp"], info["adj"]))


if __name__ == "__main__":
    main()
