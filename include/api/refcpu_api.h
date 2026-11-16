#pragma once

#include <cstddef>
#include <cstdint>

// Register width of the model this header is used with: build with -DXLEN=64 for rv64 (default 64; 32 only on branch rv32-legacy).
#ifndef XLEN
#define XLEN 64
#endif
typedef uint64_t refcpu_xlen_t;

// F/D (default on; rv32 and rv64): the state carries the FP registers and fcsr.  Build with -DFP=0 for a core without floating point; the
// value must be the one librefcpu.a was built with.
#ifndef FP
#define FP 1
#endif

struct RefCpuContext;

struct RefCpuState {
  refcpu_xlen_t gpr[32];
#if FP
  uint64_t fpr[32];   // single precision NaN-boxed
  uint32_t fcsr;      // frm << 5 | fflags
#endif
  refcpu_xlen_t csr[21];
  refcpu_xlen_t pc;

  uint32_t store_addr;
  refcpu_xlen_t store_data;
  uint32_t store_strb;
  bool store;
  uint32_t instruction;
  bool page_fault_inst;
  bool page_fault_load;
  bool page_fault_store;
  uint32_t inst_idx;
  refcpu_xlen_t commit_pc;
  bool reserve_valid;
  uint32_t reserve_addr;
  uint8_t privilege;
};

struct RefCpuStepInfo {
  uint32_t instruction;   // with RVC=1 a compressed instruction is given expanded (the 32-bit equivalent)
  uint32_t inst_len;      // length in bytes of the instruction just executed: 4, or 2 for a compressed one (RVC=1)
  bool page_fault_inst;
  bool page_fault_load;
  bool page_fault_store;
  bool sim_end;
  bool is_exception;
  bool is_csr;
  bool is_br;
  bool br_taken;
  bool is_io;
};

RefCpuContext *refcpu_init(uint32_t reset_pc, uint32_t ram_size_bytes);
void refcpu_destroy(RefCpuContext *ctx);

void refcpu_load_flash_image(RefCpuContext *ctx, const char *path);
void refcpu_load_sdcard_image(RefCpuContext *ctx, const char *path);

void refcpu_get_state(const RefCpuContext *ctx, RefCpuState *state);
void refcpu_set_state(RefCpuContext *ctx, const RefCpuState *state);
void refcpu_get_step_info(const RefCpuContext *ctx, RefCpuStepInfo *info);

// Single fields without a copy of the whole state (a caller's per-instruction path). The arrays are the model's own
// (valid until the model changes them); the values are those refcpu_get_state() would give.
const refcpu_xlen_t *refcpu_gpr(const RefCpuContext *ctx);          // 32 integer registers
#if FP
const uint64_t *refcpu_fpr(const RefCpuContext *ctx);               // 32 FP registers
uint32_t refcpu_fcsr(const RefCpuContext *ctx);                     // frm << 5 | fflags
#endif
refcpu_xlen_t refcpu_csr(const RefCpuContext *ctx, int idx);        // RefCpuState::csr[idx] (mip / sip: the visible values)
refcpu_xlen_t refcpu_pc(const RefCpuContext *ctx);
uint8_t refcpu_privilege(const RefCpuContext *ctx);
void refcpu_reservation(const RefCpuContext *ctx, bool *valid, uint32_t *addr);
void refcpu_store_info(const RefCpuContext *ctx, bool *store, uint32_t *addr);   // the last step's store
// refcpu_set_state() of the current state with pc changed (so the visible mip / sip are latched and the external CSR
// read override is cleared, as that round trip does), without the copies.
void refcpu_set_pc(RefCpuContext *ctx, refcpu_xlen_t pc);

void refcpu_step(RefCpuContext *ctx, uint64_t steps);
void refcpu_sync_ram_from_dut(RefCpuContext *ctx, const uint32_t *ram_src,
                              size_t size_bytes);
void refcpu_sync_sdram_from_dut(RefCpuContext *ctx,
                                const uint32_t *sdram_src,
                                size_t size_bytes);
uint32_t *refcpu_get_ram_ptr(RefCpuContext *ctx);
uint32_t *refcpu_get_sdram_ptr(RefCpuContext *ctx);
uint32_t refcpu_load_word(const RefCpuContext *ctx, uint32_t addr);
void refcpu_store_word(RefCpuContext *ctx, uint32_t addr, uint32_t data);

void refcpu_clear_io_words(RefCpuContext *ctx);
void refcpu_set_io_word(RefCpuContext *ctx, uint32_t addr, uint32_t data);
uint32_t refcpu_get_io_word(const RefCpuContext *ctx, uint32_t addr);
void refcpu_sync_mmio_devices_from_backing(RefCpuContext *ctx);

void refcpu_set_uart_print(RefCpuContext *ctx, bool enable);
void refcpu_set_ref_only(RefCpuContext *ctx, bool enable);
void refcpu_set_device_effects(RefCpuContext *ctx, bool enable);
void refcpu_set_interrupt_delivery(RefCpuContext *ctx, bool enable);
void refcpu_set_next_csr_read(RefCpuContext *ctx, uint32_t csr_addr,
                              refcpu_xlen_t value);
bool refcpu_take_forced_interrupt(RefCpuContext *ctx, refcpu_xlen_t cause,
                                  uint8_t target_privilege,
                                  uint32_t pending_snapshot);
void refcpu_force_next_page_fault(RefCpuContext *ctx, uint32_t type);  // type: 0 fetch, 1 load, 2 store
void refcpu_set_sim_end(RefCpuContext *ctx, bool value);
void refcpu_set_sim_time(RefCpuContext *ctx, uint64_t sim_time);

bool refcpu_probe_pc_translation(RefCpuContext *ctx);
bool refcpu_translate_data(RefCpuContext *ctx, refcpu_xlen_t v_addr, uint32_t type,
                           uint32_t *p_addr);
