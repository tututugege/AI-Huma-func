#pragma once
#include <cstdint>
// Architectural register width of the function model: -DXLEN=32 (default, rv32imab) or -DXLEN=64 (rv64imab).
#ifndef XLEN
#define XLEN 32
#endif
#if XLEN == 64
using xlen_t = uint64_t;
using sxlen_t = int64_t;
#elif XLEN == 32
using xlen_t = uint32_t;
using sxlen_t = int32_t;
#else
#error "XLEN must be 32 or 64"
#endif
// FP=1 (default): the F and D extensions, rv32 and rv64 (a separate 32 x 64-bit FP register file, fcsr, mstatus.FS/SD; FLEN = 64
// in both).  FP=0: no floating point at all (every OP-FP / LOAD-FP / STORE-FP encoding is illegal).  Zfinx no longer exists.
#ifndef FP
#define FP 1
#endif
constexpr unsigned kXlen = XLEN;
constexpr xlen_t kXlenMsb = xlen_t(1) << (XLEN - 1);   // interrupt bit of mcause/scause
constexpr unsigned kShamtMask = XLEN - 1;
// satp translation mode: Sv32 enabled by bit 31 (rv32); Sv39 is MODE = 8 in satp[63:60] (rv64).
inline bool satp_enabled(xlen_t satp) {
#if XLEN == 64
  return (satp >> 60) == 8;
#else
  return (satp & 0x80000000u) != 0;
#endif
}
// mstatus / sstatus layouts.  With F/D, SD (bit XLEN-1) follows FS; UXL = SXL = 2 (rv64) are read-only.
#if XLEN == 64
constexpr xlen_t kMstatusWmask = 0x00000000007FF9BBull;
#if FP
constexpr xlen_t kSstatusMask = 0x80000003000DE133ull;    // visible bits (UXL, SD included)
#else
constexpr xlen_t kSstatusMask = 0x00000003000DE133ull;    // visible bits (UXL included)
#endif
constexpr xlen_t kSstatusWmask = 0x00000000000DE133ull;   // writable bits: UXL is read-only (64-bit only)
constexpr xlen_t kMstatusReset = 0x0000000A00000000ull;
#else
#if FP
constexpr xlen_t kMstatusWmask = 0x007FF9BBu;     // SD (bit 31) is read-only: it follows FS
constexpr xlen_t kSstatusMask = 0x800DE133u;
constexpr xlen_t kSstatusWmask = 0x000DE133u;
#else
constexpr xlen_t kMstatusWmask = 0x807FF9BBu;
constexpr xlen_t kSstatusMask = 0x800DE133u;
constexpr xlen_t kSstatusWmask = 0x800DE133u;
#endif
constexpr xlen_t kMstatusReset = 0;
#endif
