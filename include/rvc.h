#pragma once
// RVC (C extension): expand a 16-bit compressed instruction into the equivalent 32-bit instruction.
// Returns false for an illegal / reserved encoding (the caller raises an illegal-instruction exception with the 16-bit value as
// trap value).  HINT encodings expand to the instruction they alias (writes to x0 are discarded by the core).
// With XLEN = 32: c.jal, c.flw/c.fsw/c.flwsp/c.fswsp; with XLEN = 64: c.addiw/c.subw/c.addw/c.ld/c.sd/c.ldsp/c.sdsp.
// The double-precision forms (c.fld/c.fsd/c.fldsp/c.fsdsp) exist only with FP = 1.
#include "xlen.h"
#include <cstdint>

namespace rvc {

inline uint32_t bits(uint32_t v, int hi, int lo) { return (v >> lo) & ((1u << (hi - lo + 1)) - 1u); }
inline int32_t sext(uint32_t v, int width) { return static_cast<int32_t>(v << (32 - width)) >> (32 - width); }

inline uint32_t enc_i(uint32_t opcode, uint32_t rd, uint32_t f3, uint32_t rs1, int32_t imm) {
  return opcode | (rd << 7) | (f3 << 12) | (rs1 << 15) | ((static_cast<uint32_t>(imm) & 0xfffu) << 20);
}
inline uint32_t enc_r(uint32_t opcode, uint32_t rd, uint32_t f3, uint32_t rs1, uint32_t rs2, uint32_t f7) {
  return opcode | (rd << 7) | (f3 << 12) | (rs1 << 15) | (rs2 << 20) | (f7 << 25);
}
inline uint32_t enc_s(uint32_t opcode, uint32_t f3, uint32_t rs1, uint32_t rs2, int32_t imm) {
  const uint32_t u = static_cast<uint32_t>(imm);
  return opcode | ((u & 0x1fu) << 7) | (f3 << 12) | (rs1 << 15) | (rs2 << 20) | (((u >> 5) & 0x7fu) << 25);
}
inline uint32_t enc_b(uint32_t f3, uint32_t rs1, uint32_t rs2, int32_t imm) {
  const uint32_t u = static_cast<uint32_t>(imm);
  return 0x63u | (((u >> 11) & 1u) << 7) | (((u >> 1) & 0xfu) << 8) | (f3 << 12) | (rs1 << 15) | (rs2 << 20) |
         (((u >> 5) & 0x3fu) << 25) | (((u >> 12) & 1u) << 31);
}
inline uint32_t enc_j(uint32_t rd, int32_t imm) {
  const uint32_t u = static_cast<uint32_t>(imm);
  return 0x6fu | (rd << 7) | (((u >> 12) & 0xffu) << 12) | (((u >> 11) & 1u) << 20) | (((u >> 1) & 0x3ffu) << 21) |
         (((u >> 20) & 1u) << 31);
}

inline bool expand(uint32_t h, uint32_t &out) {
  const uint32_t q = h & 3u, f3 = bits(h, 15, 13);
  const uint32_t rd_f = bits(h, 11, 7), rs2_f = bits(h, 6, 2);          // full-width register fields
  const uint32_t rdp = bits(h, 4, 2) + 8, rs1p = bits(h, 9, 7) + 8;     // the 3-bit register fields (x8..x15)
  const uint32_t imm6u = (bits(h, 12, 12) << 5) | bits(h, 6, 2);       // {inst[12], inst[6:2]}
  const int32_t imm6 = sext(imm6u, 6);
  constexpr uint32_t LOAD = 0x03, STORE = 0x23, OPIMM = 0x13, OP = 0x33, OPIMMW = 0x1b, OPW = 0x3b, LOADFP = 0x07, STOREFP = 0x27;
  constexpr bool rv64 = (XLEN == 64);
  constexpr bool fp = (FP != 0);
  if (q == 0) {
    switch (f3) {
    case 0: {  // c.addi4spn
      const uint32_t nz = (bits(h, 10, 7) << 6) | (bits(h, 12, 11) << 4) | (bits(h, 5, 5) << 3) | (bits(h, 6, 6) << 2);
      if (nz == 0) return false;
      out = enc_i(OPIMM, rdp, 0, 2, static_cast<int32_t>(nz));
      return true;
    }
    case 1: {  // c.fld
      if (!fp) return false;
      const uint32_t off = (bits(h, 12, 10) << 3) | (bits(h, 6, 5) << 6);
      out = enc_i(LOADFP, rdp, 3, rs1p, static_cast<int32_t>(off));
      return true;
    }
    case 2: {  // c.lw
      const uint32_t off = (bits(h, 12, 10) << 3) | (bits(h, 6, 6) << 2) | (bits(h, 5, 5) << 6);
      out = enc_i(LOAD, rdp, 2, rs1p, static_cast<int32_t>(off));
      return true;
    }
    case 3: {
      if (rv64) {  // c.ld
        const uint32_t off = (bits(h, 12, 10) << 3) | (bits(h, 6, 5) << 6);
        out = enc_i(LOAD, rdp, 3, rs1p, static_cast<int32_t>(off));
        return true;
      }
      if (!fp) return false;  // c.flw
      const uint32_t off = (bits(h, 12, 10) << 3) | (bits(h, 6, 6) << 2) | (bits(h, 5, 5) << 6);
      out = enc_i(LOADFP, rdp, 2, rs1p, static_cast<int32_t>(off));
      return true;
    }
    case 5: {  // c.fsd
      if (!fp) return false;
      const uint32_t off = (bits(h, 12, 10) << 3) | (bits(h, 6, 5) << 6);
      out = enc_s(STOREFP, 3, rs1p, rdp, static_cast<int32_t>(off));
      return true;
    }
    case 6: {  // c.sw
      const uint32_t off = (bits(h, 12, 10) << 3) | (bits(h, 6, 6) << 2) | (bits(h, 5, 5) << 6);
      out = enc_s(STORE, 2, rs1p, rdp, static_cast<int32_t>(off));
      return true;
    }
    case 7: {
      if (rv64) {  // c.sd
        const uint32_t off = (bits(h, 12, 10) << 3) | (bits(h, 6, 5) << 6);
        out = enc_s(STORE, 3, rs1p, rdp, static_cast<int32_t>(off));
        return true;
      }
      if (!fp) return false;  // c.fsw
      const uint32_t off = (bits(h, 12, 10) << 3) | (bits(h, 6, 6) << 2) | (bits(h, 5, 5) << 6);
      out = enc_s(STOREFP, 2, rs1p, rdp, static_cast<int32_t>(off));
      return true;
    }
    default: return false;  // 100: reserved
    }
  }
  if (q == 1) {
    switch (f3) {
    case 0:  // c.addi / c.nop / hint
      out = enc_i(OPIMM, rd_f, 0, rd_f, imm6);
      return true;
    case 1:
      if (rv64) {  // c.addiw
        if (rd_f == 0) return false;
        out = enc_i(OPIMMW, rd_f, 0, rd_f, imm6);
        return true;
      } else {  // c.jal
        const uint32_t o = (bits(h, 12, 12) << 11) | (bits(h, 11, 11) << 4) | (bits(h, 10, 9) << 8) | (bits(h, 8, 8) << 10) |
                           (bits(h, 7, 7) << 6) | (bits(h, 6, 6) << 7) | (bits(h, 5, 3) << 1) | (bits(h, 2, 2) << 5);
        out = enc_j(1, sext(o, 12));
        return true;
      }
    case 2:  // c.li
      out = enc_i(OPIMM, rd_f, 0, 0, imm6);
      return true;
    case 3:
      if (rd_f == 2) {  // c.addi16sp
        const uint32_t nz = (bits(h, 12, 12) << 9) | (bits(h, 4, 3) << 7) | (bits(h, 5, 5) << 6) | (bits(h, 2, 2) << 5) |
                            (bits(h, 6, 6) << 4);
        if (nz == 0) return false;
        out = enc_i(OPIMM, 2, 0, 2, sext(nz, 10));
        return true;
      } else {  // c.lui
        if (imm6u == 0) return false;
        out = (static_cast<uint32_t>(imm6) << 12) | (rd_f << 7) | 0x37u;
        return true;
      }
    case 4: {
      const uint32_t f2 = bits(h, 11, 10);
      if (f2 == 0 || f2 == 1) {  // c.srli / c.srai
        if (!rv64 && bits(h, 12, 12)) return false;  // rv32: shamt[5] must be 0
        out = enc_i(OPIMM, rs1p, 5, rs1p, static_cast<int32_t>(imm6u) | (f2 == 1 ? 0x400 : 0));
        return true;
      }
      if (f2 == 2) {  // c.andi
        out = enc_i(OPIMM, rs1p, 7, rs1p, imm6);
        return true;
      }
      const uint32_t f = bits(h, 6, 5);
      if (!bits(h, 12, 12)) {  // c.sub / c.xor / c.or / c.and
        static const uint32_t f3t[4] = {0, 4, 6, 7};
        out = enc_r(OP, rs1p, f3t[f], rs1p, rdp, f == 0 ? 0x20u : 0u);
        return true;
      }
      if (rv64 && f < 2) {  // c.subw / c.addw
        out = enc_r(OPW, rs1p, 0, rs1p, rdp, f == 0 ? 0x20u : 0u);
        return true;
      }
      return false;
    }
    case 5: {  // c.j
      const uint32_t o = (bits(h, 12, 12) << 11) | (bits(h, 11, 11) << 4) | (bits(h, 10, 9) << 8) | (bits(h, 8, 8) << 10) |
                         (bits(h, 7, 7) << 6) | (bits(h, 6, 6) << 7) | (bits(h, 5, 3) << 1) | (bits(h, 2, 2) << 5);
      out = enc_j(0, sext(o, 12));
      return true;
    }
    default: {  // 6: c.beqz, 7: c.bnez
      const uint32_t o = (bits(h, 12, 12) << 8) | (bits(h, 11, 10) << 3) | (bits(h, 6, 5) << 6) | (bits(h, 4, 3) << 1) | (bits(h, 2, 2) << 5);
      out = enc_b(f3 == 6 ? 0 : 1, rs1p, 0, sext(o, 9));
      return true;
    }
    }
  }
  if (q == 2) {
    switch (f3) {
    case 0:  // c.slli
      if (!rv64 && bits(h, 12, 12)) return false;
      out = enc_i(OPIMM, rd_f, 1, rd_f, static_cast<int32_t>(imm6u));
      return true;
    case 1: {  // c.fldsp
      if (!fp) return false;
      const uint32_t off = (bits(h, 12, 12) << 5) | (bits(h, 6, 5) << 3) | (bits(h, 4, 2) << 6);
      out = enc_i(LOADFP, rd_f, 3, 2, static_cast<int32_t>(off));
      return true;
    }
    case 2: {  // c.lwsp
      if (rd_f == 0) return false;
      const uint32_t off = (bits(h, 12, 12) << 5) | (bits(h, 6, 4) << 2) | (bits(h, 3, 2) << 6);
      out = enc_i(LOAD, rd_f, 2, 2, static_cast<int32_t>(off));
      return true;
    }
    case 3: {
      if (rv64) {  // c.ldsp
        if (rd_f == 0) return false;
        const uint32_t off = (bits(h, 12, 12) << 5) | (bits(h, 6, 5) << 3) | (bits(h, 4, 2) << 6);
        out = enc_i(LOAD, rd_f, 3, 2, static_cast<int32_t>(off));
        return true;
      }
      if (!fp) return false;  // c.flwsp
      const uint32_t off = (bits(h, 12, 12) << 5) | (bits(h, 6, 4) << 2) | (bits(h, 3, 2) << 6);
      out = enc_i(LOADFP, rd_f, 2, 2, static_cast<int32_t>(off));
      return true;
    }
    case 4:
      if (!bits(h, 12, 12)) {
        if (rs2_f == 0) {  // c.jr
          if (rd_f == 0) return false;
          out = enc_i(0x67, 0, 0, rd_f, 0);
          return true;
        }
        out = enc_r(OP, rd_f, 0, 0, rs2_f, 0);  // c.mv
        return true;
      }
      if (rs2_f == 0) {
        if (rd_f == 0) { out = 0x00100073u; return true; }  // c.ebreak
        out = enc_i(0x67, 1, 0, rd_f, 0);                  // c.jalr
        return true;
      }
      out = enc_r(OP, rd_f, 0, rd_f, rs2_f, 0);  // c.add
      return true;
    case 5: {  // c.fsdsp
      if (!fp) return false;
      const uint32_t off = (bits(h, 12, 10) << 3) | (bits(h, 9, 7) << 6);
      out = enc_s(STOREFP, 3, 2, rs2_f, static_cast<int32_t>(off));
      return true;
    }
    case 6: {  // c.swsp
      const uint32_t off = (bits(h, 12, 9) << 2) | (bits(h, 8, 7) << 6);
      out = enc_s(STORE, 2, 2, rs2_f, static_cast<int32_t>(off));
      return true;
    }
    default: {  // 7
      if (rv64) {  // c.sdsp
        const uint32_t off = (bits(h, 12, 10) << 3) | (bits(h, 9, 7) << 6);
        out = enc_s(STORE, 3, 2, rs2_f, static_cast<int32_t>(off));
        return true;
      }
      if (!fp) return false;  // c.fswsp
      const uint32_t off = (bits(h, 12, 9) << 2) | (bits(h, 8, 7) << 6);
      out = enc_s(STOREFP, 2, 2, rs2_f, static_cast<int32_t>(off));
      return true;
    }
    }
  }
  return false;  // quadrant 3 is a 32-bit instruction: not expanded here
}

}  // namespace rvc
