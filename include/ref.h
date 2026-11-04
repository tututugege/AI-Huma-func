#pragma once
#include "RISCV.h"
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <unordered_map>
#include <vector>

#ifdef ENABLE_SPIKE
class SpikeRef;
#endif

#define FORCE_INLINE __attribute__((always_inline)) inline

#define RISCV_MODE_U 0b00
#define RISCV_MODE_S 0b01
#define RISCV_MODE_M 0b11

#define BITMASK(bits) ((1ull << (bits)) - 1)
#define BITS(x, hi, lo)                                                        \
  (((x) >> (lo)) & BITMASK((hi) - (lo) + 1)) // similar to x[hi:lo] in verilog
#define SEXT(x, len)                                                           \
  ({                                                                           \
    struct {                                                                   \
      int64_t n : len;                                                         \
    } __x = {.n = (int64_t)x};                                                 \
    (uint64_t)__x.n;                                                           \
  })

#define immI(i) SEXT(BITS(i, 31, 20), 12)
#define immU(i) (SEXT(BITS(i, 31, 12), 20) << 12)
#define immS(i) ((SEXT(BITS(i, 31, 25), 7) << 5) | BITS(i, 11, 7))
#define immJ(i)                                                                \
  ((SEXT(BITS(i, 31, 31), 1) << 20) | (BITS(i, 19, 12) << 12) |                \
   (BITS(i, 20, 20) << 11) | (BITS(i, 30, 21) << 1))
#define immB(i)                                                                \
  ((SEXT(BITS(i, 31, 31), 1) << 12) | (BITS(i, 7, 7) << 11) |                  \
   (BITS(i, 30, 25) << 5) | (BITS(i, 11, 8) << 1))

// ================= CSR Bit Masks (Standard RISC-V) =================
#ifndef MSTATUS_MIE
#define MSTATUS_MIE (1 << 3)
#endif
#ifndef MSTATUS_MPIE
#define MSTATUS_MPIE (1 << 7)
#endif
#ifndef MSTATUS_SIE
#define MSTATUS_SIE (1 << 1)
#endif
#ifndef MSTATUS_SPIE
#define MSTATUS_SPIE (1 << 5)
#endif
#ifndef MSTATUS_MPP
#define MSTATUS_MPP (3 << 11) // Bits 11-12
#endif
#ifndef MSTATUS_SPP
#define MSTATUS_SPP (1 << 8) // Bit 8
#endif

#ifndef MIP_SSIP
#define MIP_SSIP (1 << 1)
#endif
#ifndef MIP_MSIP
#define MIP_MSIP (1 << 3)
#endif
#ifndef MIP_STIP
#define MIP_STIP (1 << 5)
#endif
#ifndef MIP_MTIP
#define MIP_MTIP (1 << 7)
#endif
#ifndef MIP_SEIP
#define MIP_SEIP (1 << 9)
#endif
#ifndef MIP_MEIP
#define MIP_MEIP (1 << 11)
#endif

// 获取 MPP 的值 (0=U, 1=S, 3=M)
#define GET_MPP(x) ((x >> 11) & 0x3)
// 获取 SPP 的值
#define GET_SPP(x) ((x >> 8) & 0x1)

// SV32 Page Table Entry (PTE) Bits
#ifndef PTE_V
#define PTE_V (1 << 0) // Valid
#endif
#ifndef PTE_R
#define PTE_R (1 << 1) // Read
#endif
#ifndef PTE_W
#define PTE_W (1 << 2) // Write
#endif
#ifndef PTE_X
#define PTE_X (1 << 3) // Execute
#endif
#ifndef PTE_U
#define PTE_U (1 << 4) // User
#endif
#ifndef PTE_G
#define PTE_G (1 << 5) // Global
#endif
#ifndef PTE_A
#define PTE_A (1 << 6) // Accessed
#endif
#ifndef PTE_D
#define PTE_D (1 << 7) // Dirty
#endif

// MSTATUS bits needed for translation
#ifndef MSTATUS_MXR
#define MSTATUS_MXR (1 << 19)
#endif
#ifndef MSTATUS_SUM
#define MSTATUS_SUM (1 << 18)
#endif
#ifndef MSTATUS_MPRV
#define MSTATUS_MPRV (1 << 17)
#endif
#ifndef MSTATUS_MPP_SHIFT
#define MSTATUS_MPP_SHIFT 11
#endif

// ===================================================================
//
// instruction cap (all modes): none by default; REF_MAX_INSTS sets one (a safety stop for runaway runs)
inline const uint64_t g_max_sim_time = std::getenv("REF_MAX_INSTS") ? std::strtoull(std::getenv("REF_MAX_INSTS"), nullptr, 0) : ~0ull;
#define MAX_SIM_TIME g_max_sim_time

// --- 1. 定义四种运行模式 ---
enum class SimMode {
  NORMAL,         // 0. 正常从头运行 (无 BBV，无 Checkpoint)
  GEN_BBV,        // 1. 生成 BBV 文件 (用于 SimPoint 分析)
  GEN_CHECKPOINT, // 2. 根据 SimPoint 结果 (.points) 生成 Checkpoints
  RESTORE,        // 3. 从 Checkpoint 恢复并运行指定长度 (Detailed Run)
  GEN_SPECIAL_CHECKPOINT // 4. 在绝对指令计数处生成 special checkpoints
};

// --- 2. 配置结构体 ---
struct SimConfig {
  SimMode mode = SimMode::NORMAL;
  std::string image_file;
  std::string flash_image_file;
  std::string sdcard_image_file;
  uint32_t ram_size = PHYSICAL_MEMORY_LENGTH;
  uint32_t reset_pc = 0;
  bool difftest = false;

  // Mode: GEN_BBV
  std::string bbv_output_file;

  // Mode: GEN_CHECKPOINT
  std::string points_file;    // 输入: SimPoint 生成的 .points 文件
  std::string checkpoint_dir; // 输出: Checkpoint 保存目录

  // Mode: RESTORE
  std::string restore_file; // 输入: Checkpoint 文件路径
  uint64_t max_insts = 0;   // 运行多少条指令后停止 (0代表一直运行)
  // Front-door restore: load only the memory of the checkpoint; the CPU and the devices start from reset and a restore
  // program (tools/ckpt2stub.py, loaded with --flash-image) sets the registers, CSRs and devices and ends with mret.
  // The instruction counters restart at the first mret.
  bool restore_mem_only = false;
};

typedef struct CPU_state {
  uint32_t gpr[32];
  uint32_t csr[21];
  uint32_t pc;

  uint32_t store_addr;
  uint32_t store_data;
  uint32_t store_strb;
  bool store;
  bool reserve_valid;
  uint32_t reserve_addr;
} CPU_state;

struct ReadOverride {
  bool valid = false;
  uint32_t address = 0;
  uint32_t value = 0;
};

class Ref_cpu {
public:
  ~Ref_cpu();
  uint32_t *memory = nullptr;
  uint32_t *sdram_memory = nullptr;
  std::unordered_map<uint32_t, uint32_t> io_words;
  uint32_t ram_size;
  uint32_t Instruction;
  CPU_state state;
  uint8_t privilege;
  bool asy;
  bool page_fault_inst;
  bool page_fault_load;
  bool page_fault_store;
  bool illegal_exception;
  bool breakpoint_exception; // ebreak inside the semihosting marker sequence: a real breakpoint trap (cause 3)

  bool M_software_interrupt;
  bool M_timer_interrupt;
  bool M_external_interrupt;
  bool S_software_interrupt;
  bool S_timer_interrupt;
  bool S_external_interrupt;

  bool is_br;
  bool br_taken;

  bool is_exception;
  bool is_csr;

  bool sim_end;
  uint64_t sim_time;
  bool is_io;
  bool force_sync;
  bool uart_print = false;
  bool ref_only = false;
  bool device_effects_enable = true;
  bool interrupt_delivery_enable = true;
  ReadOverride external_csr_read;
  std::vector<uint8_t> flash_image;
  std::vector<uint8_t> sdcard_image;
  std::unordered_map<uint32_t, uint32_t> ocsdc_regs;
  bool ocsdc_data_pending = false;
  uint32_t xps_intc_isr = 0;
  uint32_t xps_intc_ier = 0;
  uint32_t xps_intc_mer = 0;

  void init(uint32_t reset_pc, const char *image, uint32_t size);
  void evaluate_interrupts(uint32_t mip_reg);
  bool take_forced_interrupt(uint32_t cause, uint8_t target_privilege,
                             uint32_t pending_snapshot);
  void load_flash_image(const std::string &path);
  void load_sdcard_image(const std::string &path);
  void ocsdc_reset();
  void ocsdc_write_reg(uint32_t word_addr, uint32_t data);
  uint32_t ocsdc_read_reg(uint32_t word_addr) const;
  void ocsdc_execute_command(uint32_t command, uint32_t argument);
  void xps_intc_reset();
  uint32_t xps_intc_read_reg(uint32_t word_addr) const;
  void xps_intc_write_reg(uint32_t word_addr, uint32_t data);
  void xps_intc_set_irq_level(uint32_t irq_id, bool asserted);
  void refresh_external_interrupt();
  void uart_refresh_interrupt();
  uint32_t checkpoint_mmio_read_word(uint32_t word_addr) const;
  void checkpoint_mmio_write_backing(uint32_t word_addr, uint32_t data);
  void checkpoint_mmio_sync_devices();
  void exec(const SimConfig &config);
  bool va2pa(uint32_t &p_addr, uint32_t v_addr, uint32_t type);
  bool va2pa_walk(uint32_t &p_addr, uint32_t v_addr, uint32_t type);
  // Software TLB over va2pa_walk.  The walk is a pure function of (satp, mstatus MXR/SUM/MPRV/MPP, privilege,
  // v_addr, type, page-table memory), so a cached result stays exact as long as no page-table word changed: every
  // physical page a walk read is marked, any store into a marked page (and any bulk write to memory) flushes the TLB.
  struct TlbEntry { uint32_t epoch, vpn, satp, frame, ctx; };
  static constexpr uint32_t kTlbSize = 1024;
  TlbEntry tlb[kTlbSize] = {};
  uint32_t tlb_epoch = 1;
  // Per physical page flags: bit 0 = a page-table page a cached translation depends on, bit 1 = a page decoded
  // instructions were cached from.  A store into a flagged page flushes the matching cache.
  std::vector<uint8_t> pt_page_flag = std::vector<uint8_t>(1u << 20, 0);
  std::vector<uint32_t> pt_marked;
  std::vector<uint32_t> code_marked;
  void tlb_flush();
  void icache_flush();
  void page_flag_hit(uint8_t f) {
    if (f & 1) tlb_flush();  // also flushes the instruction cache (virtual to physical mapping may have changed)
    if (f & 2) icache_flush();
  }
  void mark_pt_page(uint32_t addr) {
    const uint32_t p = addr >> 12;
    if (!(pt_page_flag[p] & 1)) {
      pt_page_flag[p] |= 1;
      pt_marked.push_back(p);
    }
  }
  void mark_code_page(uint32_t addr) {
    const uint32_t p = addr >> 12;
    if (!(pt_page_flag[p] & 2)) {
      pt_page_flag[p] |= 2;
      code_marked.push_back(p);
    }
  }

  // Fast path for plain runs: a basic-block cache with threaded dispatch for the common integer instructions.  Anything
  // unusual (MMIO, faults, misaligned accesses, CSR/AMO/FP, interrupts, the timer deadline) falls back to RISCV() before
  // any state is changed.  Blocks are keyed by virtual pc, (satp, privilege) and an epoch that is bumped when the
  // translation or the code memory may have changed.
  struct FastInsn {
    const void *h = nullptr;  // handler (label address)
    uint32_t imm = 0;
    uint8_t op = 0, rd = 0, rs1 = 0, rs2 = 0;
  };
  static constexpr uint32_t kMaxBlockOps = 32;
  static constexpr uint32_t kFastBlocks = 16384;
  struct FastBlock {
    uint64_t ctx = 0;  // (satp << 32) | (privilege << 30) | epoch; 0 = invalid
    uint32_t pc = 0, n = 0;  // n = instructions executed by a full run (the end pseudo-op is not counted)
    FastInsn ops[kMaxBlockOps + 1];
  };
  std::vector<FastBlock> fblocks;
  uint32_t icache_epoch = 1;  // 30 bits
  // cap: at most this many instructions (the caller keeps the last instruction before an event for RISCV()); stop_iv:
  // stop right after a U-mode branch that completes an interval instead of handling the boundary; bbv: call
  // bbv_commit() at every U-mode branch; trk: keep state.store_addr/data/strb as RISCV() would (checkpoint exactness)
  void fast_run(uint32_t &finished_intervals, uint64_t cap = UINT64_MAX, bool stop_iv = false, bool bbv = false,
                bool trk = false);
  template <bool U, bool DX>
  void fast_run_impl(uint32_t &finished_intervals, uint64_t budget, uint32_t mip_eff, bool stop_iv, bool bbv, bool trk);
  void RISCV();
  void FORCE_INLINE RV32IM();
  __attribute__((noinline)) void RV32A();  // cold: keeps RISCV()'s frame small
  __attribute__((noinline)) void RV32CSR();  // cold: keeps RISCV()'s frame small
  __attribute__((noinline)) void RV32Zfinx();  // cold: keeps RISCV()'s frame small
  void exception(uint32_t trap_val);
  void store_data();
  uint32_t load_word(uint32_t addr) const;
  void store_word(uint32_t addr, uint32_t data);
  uint32_t visible_mip() const;
  uint32_t visible_sip() const;

  uint8_t fcsr_fflags = 0;
  uint8_t fcsr_frm = 0;

  // BBV / checkpoint interval in user-mode instructions; REF_INTERVAL overrides the default 1e8 (bbv and ckpt runs must agree)
  static uint64_t interval_from_env() {
    const char *e = std::getenv("REF_INTERVAL");
    return e ? std::strtoull(e, nullptr, 0) : 100000000ull;
  }
  const uint64_t INTERVAL_SIZE = interval_from_env();
  uint64_t interval_inst_count = 0;

  uint32_t current_bb_head_pc; // 当前基本块的入口 PC
  uint32_t current_bb_len;

  // PC 到 SimPoint ID 的映射 (PC -> ID)
  std::unordered_map<uint32_t, uint32_t> global_pc_to_id;
  uint32_t next_bb_id = 1; // ID 从 1 开始分配
  std::vector<uint64_t> bbv_counts;
  std::ofstream bbv_file;
  void bbv_commit();
  void bbv_init_file(const char *filename);
  void dump_bbv();
  void save_checkpoint(const std::string &filename);
  void restore_checkpoint(const std::string &filename, bool mem_only = false);

#ifdef ENABLE_SPIKE
  std::unique_ptr<SpikeRef> spike_ref;
#endif
  bool difftest_started = false;
};
