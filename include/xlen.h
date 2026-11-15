#pragma once
#include <cstdint>
// Architectural register width of the function model: -DXLEN=64 (default; rv32 is no longer on main, see branch rv32-legacy).
#ifndef XLEN
#define XLEN 64
#endif
#if XLEN != 64
#error "only XLEN=64 is supported on main (rv32: branch rv32-legacy)"
#endif
using xlen_t = uint64_t;
using sxlen_t = int64_t;
// FP=1 (default): the F and D extensions (a separate 32 x 64-bit FP register file, fcsr, mstatus.FS/SD; FLEN = 64).  FP=0: no floating point at all (every OP-FP / LOAD-FP / STORE-FP encoding is illegal).  Zfinx no longer exists.
#ifndef FP
#define FP 1
#endif
// RVC=1: the C extension (16-bit compressed instructions; pc is 2-byte aligned, a 32-bit instruction may start at any halfword and
// cross a cache line or a page).  RVC=0 (default): no C, behaviour unchanged.
#ifndef RVC
#define RVC 0
#endif
constexpr unsigned kXlen = XLEN;
constexpr xlen_t kXlenMsb = xlen_t(1) << (XLEN - 1);   // interrupt bit of mcause/scause
constexpr unsigned kShamtMask = XLEN - 1;
// satp translation mode: Sv39 is MODE = 8 in satp[63:60].
inline bool satp_enabled(xlen_t satp) {
  return (satp >> 60) == 8;
}
// mstatus / sstatus layouts.  With F/D, SD (bit XLEN-1) follows FS; UXL = SXL = 2 (rv64) are read-only.
constexpr xlen_t kMstatusWmask = 0x00000000007FF9BBull;
#if FP
constexpr xlen_t kSstatusMask = 0x80000003000DE133ull;    // visible bits (UXL, SD included)
#else
constexpr xlen_t kSstatusMask = 0x00000003000DE133ull;    // visible bits (UXL included)
#endif
constexpr xlen_t kSstatusWmask = 0x00000000000DE133ull;   // writable bits: UXL is read-only (64-bit only)
constexpr xlen_t kMstatusReset = 0x0000000A00000000ull;
