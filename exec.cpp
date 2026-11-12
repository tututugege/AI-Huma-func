#ifdef ENABLE_SPIKE
#include "spike_ref.h"
#endif
#include "CSR.h"
#include "RISCV.h"
#include "ref.h"
#include <cstdio>
#include <cstdint>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <map>
#include <vector>
extern "C" {
#ifdef USE_SIMULATOR_SOFTFLOAT
#include "softfloat.h"
#else
#include "softfloat/softfloat.h"
#endif
}

// REF_PC_EVERY / REF_PC_RING debug output is on only when one of them is set (read once at load time)
static const bool g_dbg_hot = std::getenv("REF_PC_EVERY") != nullptr || std::getenv("REF_PC_RING") != nullptr;

namespace {
constexpr uint32_t kSdramBase = SDRAM_BASE;
constexpr uint64_t kSdramUpperBound =
    static_cast<uint64_t>(SDRAM_BASE) + SDRAM_SIZE;
constexpr uint32_t kRamBase = DDR_BASE;
constexpr uint64_t kRamUpperBound = 0x100000000ull;
constexpr uint32_t kRamSizeBytes = kRamUpperBound - kRamBase;
constexpr uint32_t kBootIoBase = 0x00000000u;
constexpr uint32_t kBootIoSize = 0x00100000u;
constexpr uint32_t kSdSectorSize = 512u;
constexpr bool kLogVerboseTimer = false;
constexpr bool kLogVerboseReturns = false;
constexpr bool kLogVerboseCsrMip = false;
constexpr bool kLogVerboseXpsIntc = false;
constexpr bool kLogVerboseOcsdcIrqEnable = false;
constexpr bool kLogVerboseOcsdcData = false;
constexpr bool kLogProgressSimTime = false;
constexpr bool kLogUserTrapSummary = false;
constexpr bool kLogUserEntrySummary = false;
constexpr bool kLogUserSyscallSummary = false;
constexpr bool kMirrorUserWriteSyscalls = true;
uint64_t g_dbg_trap_count[64] = {0};  // index: cause (bit5 set: interrupt) | (taken in S: +32 offset not used)
static uint64_t g_ref_timer_offset = 0;
static uint64_t g_ref_timer_cmp = ~0ull;
static bool g_disable_uart_console_output = false;

constexpr uint32_t kUartIrqId = 2u;
constexpr uint32_t kOcsdcCmdIrqId = 3u;
constexpr uint32_t kOcsdcDatIrqId = 4u;

constexpr uint32_t kUartRegRxTxDll = 0x0u;
constexpr uint32_t kUartRegIerDlm = 0x1u;
constexpr uint32_t kUartRegIirFcr = 0x2u;
constexpr uint32_t kUartRegLcr = 0x3u;
constexpr uint32_t kUartRegMcr = 0x4u;
constexpr uint32_t kUartRegLsr = 0x5u;
constexpr uint32_t kUartRegMsr = 0x6u;
constexpr uint32_t kUartRegScr = 0x7u;

constexpr uint8_t kUartLcrDlab = 0x80u;
constexpr uint8_t kUartLsrThre = 0x20u;
constexpr uint8_t kUartLsrTemt = 0x40u;
constexpr uint8_t kUartMsrCts = 0x10u;
constexpr uint8_t kUartMsrDsr = 0x20u;
constexpr uint8_t kUartMsrDcd = 0x80u;
constexpr uint8_t kUartIerThri = 0x02u;
constexpr uint8_t kUartIirNoInt = 0x01u;
constexpr uint8_t kUartIirThre = 0x02u;
constexpr uint8_t kUartIirFifoEnabled = 0xc0u;

bool ref_uart_trace_enabled() {
  static const bool enabled = [] {
    const char *env = std::getenv("REF_UART_TRACE");
    return env != nullptr && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

bool ref_ocsdc_probe_trace_enabled() {
  static const bool enabled = [] {
    const char *env = std::getenv("REF_OCSDC_PROBE_TRACE");
    return env != nullptr && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

bool ref_stop_at_shell_enabled() {
  static const bool enabled = [] {
    const char *env = std::getenv("REF_STOP_AT_SHELL");
    return env != nullptr && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

bool ref_init_print_trace_enabled() {
  static const bool enabled = [] {
    const char *env = std::getenv("REF_INIT_PRINT_TRACE");
    return env != nullptr && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

bool observe_uart_console_byte(uint64_t time, char ch) {
  static char window[4] = {};
  static bool init_banner_seen = false;
  static unsigned init_banner_pos = 0;
  static uint64_t init_banner_start_time = 0;
  constexpr char kInitBannerPrefix[] = "_     ___ _   _";

  if (ref_init_print_trace_enabled() && !init_banner_seen) {
    if (ch == kInitBannerPrefix[init_banner_pos]) {
      if (init_banner_pos == 0) {
        init_banner_start_time = time;
      }
      ++init_banner_pos;
      if (init_banner_pos == sizeof(kInitBannerPrefix) - 1) {
        init_banner_seen = true;
        std::cerr << "\n[RefCPU][INIT_PRINT_BEGIN] sim_time="
                  << init_banner_start_time
                  << " confirm_time=" << time << std::endl;
      }
    } else {
      init_banner_pos = (ch == kInitBannerPrefix[0]) ? 1u : 0u;
      if (init_banner_pos == 1u) {
        init_banner_start_time = time;
      }
    }
  }

  window[0] = window[1];
  window[1] = window[2];
  window[2] = window[3];
  window[3] = ch;
  if (!ref_stop_at_shell_enabled()) {
    return false;
  }
  if (window[0] == '/' && window[1] == ' ' && window[2] == '#' &&
      window[3] == ' ') {
    std::cerr << "\n[RefCPU][SHELL] sim_time=" << time << std::endl;
    return true;
  }
  return false;
}

const char *uart_reg_name(uint32_t reg) {
  switch (reg) {
  case kUartRegRxTxDll:
    return "RBR/THR/DLL";
  case kUartRegIerDlm:
    return "IER/DLM";
  case kUartRegIirFcr:
    return "IIR/FCR";
  case kUartRegLcr:
    return "LCR";
  case kUartRegMcr:
    return "MCR";
  case kUartRegLsr:
    return "LSR";
  case kUartRegMsr:
    return "MSR";
  case kUartRegScr:
    return "SCR";
  default:
    return "UART";
  }
}

void trace_uart_load(uint64_t time, uint32_t pc, uint32_t paddr, uint8_t func3,
                     uint32_t raw_word, uint32_t result) {
  if (!ref_uart_trace_enabled()) {
    return;
  }
  const uint32_t reg = paddr - UART_BASE;
  std::fprintf(stderr,
               "[REF][UART][LOAD] t=%llu pc=0x%08x addr=0x%08x reg=0x%02x(%s) "
               "func3=0x%x raw_word=0x%08x result=0x%08x\n",
               static_cast<unsigned long long>(time), pc, paddr, reg,
               uart_reg_name(reg), static_cast<unsigned>(func3), raw_word,
               result);
}

void trace_uart_store_effect(uint64_t time, uint32_t pc, uint32_t paddr,
                             uint8_t strb, uint32_t data, uint32_t before0,
                             uint32_t after0, uint32_t after4) {
  if (!ref_uart_trace_enabled()) {
    return;
  }
  const uint32_t reg = paddr - UART_BASE;
  std::fprintf(stderr,
               "[REF][UART][STORE] t=%llu pc=0x%08x addr=0x%08x reg=0x%02x(%s) "
               "strb=0x%x data=0x%08x word0:0x%08x->0x%08x word4=0x%08x\n",
               static_cast<unsigned long long>(time), pc, paddr, reg,
               uart_reg_name(reg), static_cast<unsigned>(strb), data, before0,
               after0, after4);
}

struct SdCardId {
  uint32_t ocr;
  uint32_t cid[4];
  uint32_t csd[4];
};

SdCardId make_sdcard_id(size_t image_size) {
  SdCardId id = {};
  constexpr uint32_t kSdCmdClass =
      (1u << 2) |  // CCC_BLOCK_READ
      (1u << 4) |  // CCC_BLOCK_WRITE
      (1u << 5) |  // CCC_ERASE
      (1u << 6) |  // CCC_WRITE_PROT
      (1u << 8) |  // CCC_APP_SPEC
      (1u << 10);  // CCC_SWITCH
  id.ocr = 0xc0ff8000u; // power up complete + SDHC/SDXC + 2.7V-3.6V
  id.cid[0] = 0x03534453u;
  id.cid[1] = 0x30303030u;
  id.cid[2] = 0x12345678u;
  id.cid[3] = 0x01020304u;

  const uint64_t units = (static_cast<uint64_t>(image_size) + 524287u) / 524288u;
  const uint32_t csize = units == 0 ? 0u : static_cast<uint32_t>(units - 1u);

  id.csd[0] = 0x400e0032u;
  id.csd[1] = kSdCmdClass << 20;
  id.csd[1] |= 9u << 16; // READ_BL_LEN = 512 bytes
  id.csd[1] |= (csize >> 16) & 0x3fu;
  id.csd[2] = (csize & 0xffffu) << 16;
  id.csd[3] = 0u;
  return id;
}

[[noreturn]] void mem_oob_fatal(const char *op, uint32_t addr, uint32_t size) {
  std::cerr << "[RefCPU] illegal " << op << " at paddr=0x" << std::hex << addr
            << " size=0x" << size << " (not in RAM or implemented MMIO)"
            << std::dec << std::endl;
  std::exit(1);
}

std::vector<uint8_t> load_binary_file(const std::string &path,
                                      const char *kind) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    std::cerr << "[RefCPU] Failed to open " << kind << ": " << path
              << std::endl;
    std::exit(1);
  }

  file.seekg(0, std::ios::end);
  const std::streamsize sz = file.tellg();
  file.seekg(0, std::ios::beg);
  if (sz < 0) {
    std::cerr << "[RefCPU] Failed to stat " << kind << ": " << path
              << std::endl;
    std::exit(1);
  }

  std::vector<uint8_t> data(static_cast<size_t>(sz));
  if (sz > 0 && !file.read(reinterpret_cast<char *>(data.data()), sz)) {
    std::cerr << "[RefCPU] Failed to read " << kind << ": " << path
              << std::endl;
    std::exit(1);
  }
  return data;
}

inline bool range_within(uint32_t addr, uint32_t size, uint64_t base,
                         uint64_t upper_bound) {
  if (size == 0 || static_cast<uint64_t>(addr) < base) {
    return false;
  }
  const uint64_t end =
      static_cast<uint64_t>(addr) + static_cast<uint64_t>(size) - 1;
  return end < upper_bound;
}

inline bool is_sdram_range(uint32_t addr, uint32_t size) {
  return range_within(addr, size, kSdramBase, kSdramUpperBound);
}

inline bool is_ddr_range(uint32_t addr, uint32_t size) {
  return range_within(addr, size, kRamBase, kRamUpperBound);
}

inline bool is_ram_range(uint32_t addr, uint32_t size) {
  return is_ddr_range(addr, size) || is_sdram_range(addr, size);  // DDR is by far the common case
}

inline bool is_mmio_range(uint32_t addr, uint32_t size) {
  if (size == 0) {
    return false;
  }

  const uint64_t end =
      static_cast<uint64_t>(addr) + static_cast<uint64_t>(size) - 1;
  const auto in_range = [addr, end](uint32_t base, uint32_t span) {
    const uint64_t range_end =
        static_cast<uint64_t>(base) + static_cast<uint64_t>(span) - 1;
    return addr >= base && end <= range_end;
  };

  return in_range(kBootIoBase, kBootIoSize) ||
         in_range(UART_BASE, UART_MMIO_SIZE) ||
         in_range(XPS_INTC_BASE, XPS_INTC_MMIO_SIZE) ||
         in_range(TIMER_BASE, TIMER_MMIO_SIZE) ||
         in_range(OCSDC_BASE, OCSDC_MMIO_SIZE);
}

inline bool is_legal_phys_range(uint32_t addr, uint32_t size) {
  return is_ram_range(addr, size) || is_mmio_range(addr, size);
}

inline void check_mem_range_or_log(const char *op, uint32_t addr,
                                   uint32_t size) {
  if (size == 0) {
    return;
  }
  if (!is_legal_phys_range(addr, size)) {
    mem_oob_fatal(op, addr, size);
  }
}

static inline float32_t to_f32(uint32_t v) {
  float32_t f;
  f.v = v;
  return f;
}

static inline uint32_t ref_effective_mip(uint32_t mip_reg, uint64_t now,
                                         bool synthesize_timer_interrupt) {
  if (synthesize_timer_interrupt &&
      (now + g_ref_timer_offset) >= g_ref_timer_cmp) {
    return mip_reg | MIP_MTIP;
  }
  return mip_reg;
}

// Interrupt CSRs per the privileged spec (no N extension): mie holds SSIE/MSIE/STIE/MTIE/SEIE/MEIE (0xaaa); the supervisor
// views sie / sip show only the bits delegated by mideleg (mideleg itself holds SSI/STI/SEI = 0x222).
constexpr uint32_t kMieBits = 0x00000aaau, kSBits = 0x00000222u;
static inline uint32_t delegated_s_bits(const CPU_state &state) { return state.csr[csr_mideleg] & kSBits; }
static inline void refresh_sie(CPU_state &state) { state.csr[csr_sie] = state.csr[csr_mie] & delegated_s_bits(state); }

static inline void sync_timer_csrs(CPU_state &state, uint64_t now,
                                   bool synthesize_timer_interrupt) {
  state.csr[csr_sip] =
      ref_effective_mip(state.csr[csr_mip], now, synthesize_timer_interrupt) &
      delegated_s_bits(state);
}

static inline uint64_t ref_timer_now(uint64_t now) {
  return now + g_ref_timer_offset;
}

static inline void ref_write_timer_value(uint64_t value, uint64_t now) {
  g_ref_timer_offset = value - now;
}

// Physical address of an untranslated (bare / M-mode) access: this SoC's physical space is below 4 GiB.
static inline uint32_t bare_pa(xlen_t a) {
  if (XLEN == 64 && (a >> 32) != 0) {
    std::cerr << "[RefCPU] untranslated address outside the 32-bit physical space: 0x" << std::hex << a << std::dec << std::endl;
    exit(-1);
  }
  return static_cast<uint32_t>(a);
}

// The timer registers read the live counters instead of the stored word.
static inline uint32_t timer_word_override(uint32_t p_addr, uint32_t data, uint64_t now) {
  if (p_addr == TIMER_BASE) return static_cast<uint32_t>(ref_timer_now(now));
  if (p_addr == (TIMER_BASE + 4u)) return static_cast<uint32_t>(ref_timer_now(now) >> 32);
  if (p_addr == (TIMER_BASE + 8u)) return static_cast<uint32_t>(g_ref_timer_cmp);
  if (p_addr == (TIMER_BASE + 12u)) return static_cast<uint32_t>(g_ref_timer_cmp >> 32);
  return data;
}

static inline uint32_t from_f32(float32_t f) { return f.v; }

static inline bool is_nan32(uint32_t v) {
  return ((v & 0x7F800000u) == 0x7F800000u) && ((v & 0x007FFFFFu) != 0);
}

static inline bool is_snan32(uint32_t v) {
  return is_nan32(v) && (((v >> 22) & 1u) == 0);
}

static inline uint32_t f32_min_riscv(uint32_t a, uint32_t b) {
  if (is_snan32(a) || is_snan32(b)) {
    softfloat_exceptionFlags |= softfloat_flag_invalid;
  }
  const bool a_nan = is_nan32(a);
  const bool b_nan = is_nan32(b);
  if (a_nan && b_nan)
    return 0x7fc00000u;
  if (a_nan)
    return b;
  if (b_nan)
    return a;

  const float32_t fa = to_f32(a);
  const float32_t fb = to_f32(b);
  if (f32_lt(fa, fb))
    return a;
  if (f32_lt(fb, fa))
    return b;
  return a | b; // -0.0 wins on ties
}

static inline uint32_t f32_max_riscv(uint32_t a, uint32_t b) {
  if (is_snan32(a) || is_snan32(b)) {
    softfloat_exceptionFlags |= softfloat_flag_invalid;
  }
  const bool a_nan = is_nan32(a);
  const bool b_nan = is_nan32(b);
  if (a_nan && b_nan)
    return 0x7fc00000u;
  if (a_nan)
    return b;
  if (b_nan)
    return a;

  const float32_t fa = to_f32(a);
  const float32_t fb = to_f32(b);
  if (f32_lt(fa, fb))
    return b;
  if (f32_lt(fb, fa))
    return a;
  return a & b; // +0.0 wins on ties
}

static inline uint32_t f32_classify_riscv(float32_t f) {
  const uint32_t bits = from_f32(f);
  const uint32_t sign = (bits >> 31) & 1u;
  const uint32_t exp = (bits >> 23) & 0xFFu;
  const uint32_t mant = bits & 0x7FFFFFu;

  const bool is_subnormal = (exp == 0) && (mant != 0);
  const bool is_zero = (exp == 0) && (mant == 0);
  const bool is_inf = (exp == 0xFFu) && (mant == 0);
  const bool is_nan = (exp == 0xFFu) && (mant != 0);
  const bool is_snan = is_nan && (((mant >> 22) & 1u) == 0);
  const bool is_qnan = is_nan && (((mant >> 22) & 1u) == 1);

  uint32_t res = 0;
  if (is_inf && sign)
    res |= (1u << 0);
  else if (!is_inf && !is_zero && !is_nan && !is_subnormal && sign)
    res |= (1u << 1);
  else if (is_subnormal && sign)
    res |= (1u << 2);
  else if (is_zero && sign)
    res |= (1u << 3);
  else if (is_zero && !sign)
    res |= (1u << 4);
  else if (is_subnormal && !sign)
    res |= (1u << 5);
  else if (!is_inf && !is_zero && !is_nan && !is_subnormal && !sign)
    res |= (1u << 6);
  else if (is_inf && !sign)
    res |= (1u << 7);

  if (is_snan)
    res |= (1u << 8);
  if (is_qnan)
    res |= (1u << 9);
  return res;
}
} // namespace

uint32_t Ref_cpu::visible_mip() const {
  return ref_effective_mip(state.csr[csr_mip], sim_time,
                           device_effects_enable && interrupt_delivery_enable);
}

uint32_t Ref_cpu::visible_sip() const {
  return visible_mip() & delegated_s_bits(state);
}

std::map<uint32_t, uint32_t> load_simpoints(const std::string &filename);
std::map<uint64_t, std::string>
load_special_checkpoint_targets(const std::string &filename);

namespace {
std::string checkpoint_label(std::string label) {
  for (char &ch : label) {
    const bool safe = (ch >= 'a' && ch <= 'z') ||
                      (ch >= 'A' && ch <= 'Z') ||
                      (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
    if (!safe)
      ch = '_';
  }
  return label.empty() ? "event" : label;
}
} // namespace

Ref_cpu::~Ref_cpu() {
  if (memory) {
    free(memory);
  }
  if (sdram_memory) {
    free(sdram_memory);
  }
}

void Ref_cpu::init(uint32_t reset_pc, const char *image, uint32_t size) {
  state.pc = reset_pc;
  g_ref_timer_offset = 0;
  g_ref_timer_cmp = ~0ull;
  ram_size = size;
  if (ram_size != kRamSizeBytes) {
    std::cerr << "[RefCPU] Unsupported RAM size: 0x" << std::hex << ram_size
              << ", expected 0x" << kRamSizeBytes << std::dec << std::endl;
    exit(1);
  }

  const uint32_t ram_words = ram_size / sizeof(uint32_t);
  memory = (uint32_t *)calloc(ram_words, sizeof(uint32_t));
  if (!memory) {
    std::cerr << "Error: Could not allocate " << ram_size
              << " bytes of memory" << std::endl;
    exit(1);
  }
  const uint32_t sdram_words = SDRAM_SIZE / sizeof(uint32_t);
  sdram_memory =
      (uint32_t *)calloc(sdram_words, sizeof(uint32_t));
  if (!sdram_memory) {
    std::cerr << "Error: Could not allocate " << SDRAM_SIZE
              << " bytes of SDRAM" << std::endl;
    exit(1);
  }
  io_words.clear();
  io_words[UART_BASE + 0x00] =
      static_cast<uint32_t>(kUartIirNoInt | kUartIirFifoEnabled) << 16;
  io_words[UART_BASE + 0x04] =
      0x00000003u |
      (static_cast<uint32_t>(kUartLsrThre | kUartLsrTemt) << 8) |
      (static_cast<uint32_t>(kUartMsrCts | kUartMsrDsr | kUartMsrDcd) << 16);
  ocsdc_regs.clear();
  ocsdc_data_pending = false;

  if (image != nullptr) {
    std::ifstream inst_data(image, std::ios::in | std::ios::binary);
    if (!inst_data.is_open()) {
      std::cerr << "Error: Image " << image << " does not exist" << std::endl;
      exit(1);
    }

    inst_data.seekg(0, std::ios::end);
    std::streamsize img_size = inst_data.tellg();
    inst_data.seekg(0, std::ios::beg);

    if (img_size < 0 || static_cast<uint64_t>(img_size) > kRamSizeBytes) {
      std::cerr << "[RefCPU] Image too large for DDR window: " << img_size
                << " bytes" << std::endl;
      exit(1);
    }
    const uint32_t img_bytes = static_cast<uint32_t>(img_size);
    check_mem_range_or_log("image load", kRamBase, img_bytes);

    std::cout << "[RefCPU] Loading image at 0x80000000, size: " << img_size
              << " bytes" << std::endl;
    if (!inst_data.read(reinterpret_cast<char *>(memory), img_bytes)) {
      std::cerr << "读取文件失败！" << std::endl;
      exit(1);
    }

    inst_data.close();
  }

  store_word(0x10000004, 0x00006000); // 和进入 OpenSBI 相关
  if (image != nullptr) {
#if XLEN == 64
    // boot ROM: a0 = mhartid, a1 = 0x83e00000, jump to 0x80000000 (lui would sign-extend into the upper 32 bits)
    store_word(0x0, 0xf1402573);   // csrr a0, mhartid
    store_word(0x4, 0x41f00593);   // li   a1, 0x41f
    store_word(0x8, 0x01559593);   // slli a1, a1, 21
    store_word(0xc, 0x00100293);   // li   t0, 1
    store_word(0x10, 0x01f29293);  // slli t0, t0, 31
    store_word(0x14, 0x00028067);  // jr   t0
#else
    store_word(0x0, 0xf1402573);
    store_word(0x4, 0x83e005b7);
    store_word(0x8, 0x800002b7);
    store_word(0xc, 0x00028067);
#endif
  }

  for (int i = 0; i < 32; i++) {
    state.gpr[i] = 0;
  }
  for (int i = 0; i < 21; i++) {
    state.csr[i] = 0;
  }
#if XLEN == 64
#if FP
  state.csr[csr_misa] = (xlen_t(2) << 62) | 0x14112B;   // MXL = 2, A B D F I M S U
#else
  state.csr[csr_misa] = (xlen_t(2) << 62) | 0x141103;   // MXL = 2, A B I M S U
#endif
#else
#if FP
  state.csr[csr_misa] = 0x4014112B;   // MXL = 1, A B D F I M S U
#else
  state.csr[csr_misa] = 0x40141103;
#endif
#endif
  state.csr[csr_mstatus] = kMstatusReset;
  state.csr[csr_sstatus] = kMstatusReset & kSstatusMask;
  privilege = 0b11;

  state.store = false;
  asy = false;
  page_fault_inst = false;
  page_fault_load = false;
  page_fault_store = false;
  state.reserve_valid = false;
  state.reserve_addr = 0;
  fcsr_fflags = 0;
  fcsr_frm = 0;
#if FP
  for (int i = 0; i < 32; i++) state.fpr[i] = 0;
#endif
  sim_time = 0;
  g_disable_uart_console_output = false;
  difftest_started = false;
  sim_end = false;
  uart_print = false;
  ref_only = false;
  ocsdc_reset();
  xps_intc_reset();
}

void Ref_cpu::load_flash_image(const std::string &path) {
  flash_image = load_binary_file(path, "flash image");
  if (flash_image.size() > kBootIoSize) {
    std::cerr << "[RefCPU] Flash image too large for boot window: "
              << flash_image.size() << " > " << kBootIoSize << std::endl;
    std::exit(1);
  }

  std::cout << "[RefCPU] Loading flash image at 0x0, size: "
            << flash_image.size() << " bytes" << std::endl;
  for (size_t i = 0; i < flash_image.size(); ++i) {
    const uint32_t addr = static_cast<uint32_t>(i);
    const uint32_t word_addr = addr & ~0x3u;
    const uint32_t shift = (addr & 0x3u) * 8;
    uint32_t old_word = load_word(word_addr);
    old_word &= ~(0xffu << shift);
    old_word |= static_cast<uint32_t>(flash_image[i]) << shift;
    store_word(word_addr, old_word);
  }
}

void Ref_cpu::load_sdcard_image(const std::string &path) {
  sdcard_image = load_binary_file(path, "sdcard image");
  const uint64_t rounded_capacity =
      ((static_cast<uint64_t>(sdcard_image.size()) + 524287u) / 524288u) *
      524288u;
  if (rounded_capacity > sdcard_image.size()) {
    sdcard_image.resize(static_cast<size_t>(rounded_capacity), 0);
  }
  std::cout << "[RefCPU] Loaded sdcard backend, size: " << sdcard_image.size()
            << " bytes" << std::endl;
}

void Ref_cpu::ocsdc_reset() {
  ocsdc_regs.clear();
  ocsdc_regs[OCSDC_BASE + 0x00] = 0;
  ocsdc_regs[OCSDC_BASE + 0x04] = 0;
  ocsdc_regs[OCSDC_BASE + 0x2c] = 3300; // POWER_CONTROL
  ocsdc_regs[OCSDC_BASE + 0x30] = 0;
  ocsdc_regs[OCSDC_BASE + 0x34] = 0;
  ocsdc_regs[OCSDC_BASE + 0x38] = 0;
  ocsdc_regs[OCSDC_BASE + 0x3c] = 0;
  ocsdc_regs[OCSDC_BASE + 0x40] = 0;
  ocsdc_data_pending = false;
}

void Ref_cpu::xps_intc_reset() {
  xps_intc_isr = 0;
  xps_intc_ier = 0;
  xps_intc_mer = 0;
  io_words[XPS_INTC_BASE + 0x00] = 0; // ISR
  io_words[XPS_INTC_BASE + 0x04] = 0; // IPR
  io_words[XPS_INTC_BASE + 0x08] = 0; // IER
  io_words[XPS_INTC_BASE + 0x18] = 0xffffffffu; // IVR
  io_words[XPS_INTC_BASE + 0x1c] = 0; // MER
  refresh_external_interrupt();
}

static uint32_t uart_reg_word_addr(uint32_t addr) {
  return UART_BASE + ((addr - UART_BASE) & ~0x3u);
}

static uint8_t uart_extract_byte(uint32_t word, uint32_t byte_index) {
  return static_cast<uint8_t>((word >> (byte_index * 8)) & 0xffu);
}

static uint32_t uart_update_byte(uint32_t word, uint32_t byte_index,
                                 uint8_t value) {
  const uint32_t shift = byte_index * 8;
  word &= ~(0xffu << shift);
  word |= static_cast<uint32_t>(value) << shift;
  return word;
}

static uint8_t uart_read_reg8(
    const std::unordered_map<uint32_t, uint32_t> &io_words, uint32_t addr) {
  const uint32_t word_addr = uart_reg_word_addr(addr);
  const uint32_t byte_index = addr & 0x3u;
  auto it = io_words.find(word_addr);
  const uint32_t word = (it == io_words.end()) ? 0u : it->second;
  return uart_extract_byte(word, byte_index);
}

static void uart_write_reg8(std::unordered_map<uint32_t, uint32_t> &io_words,
                            uint32_t addr, uint8_t value) {
  const uint32_t word_addr = uart_reg_word_addr(addr);
  const uint32_t byte_index = addr & 0x3u;
  auto it = io_words.find(word_addr);
  const uint32_t old_word = (it == io_words.end()) ? 0u : it->second;
  io_words[word_addr] = uart_update_byte(old_word, byte_index, value);
}

void Ref_cpu::uart_refresh_interrupt() {
  const uint8_t ier = uart_read_reg8(io_words, UART_BASE + kUartRegIerDlm);
  const uint8_t lsr = uart_read_reg8(io_words, UART_BASE + kUartRegLsr);
  const bool thre_pending = ((ier & kUartIerThri) != 0) &&
                            ((lsr & kUartLsrThre) != 0);
  uart_write_reg8(io_words, UART_BASE + kUartRegIirFcr,
                  static_cast<uint8_t>(kUartIirFifoEnabled |
                                       (thre_pending ? kUartIirThre
                                                     : kUartIirNoInt)));
  xps_intc_set_irq_level(kUartIrqId, thre_pending);
}

uint32_t Ref_cpu::xps_intc_read_reg(uint32_t word_addr) const {
  switch (word_addr - XPS_INTC_BASE) {
  case 0x00:
    return xps_intc_isr;
  case 0x04:
    return xps_intc_isr & xps_intc_ier;
  case 0x08:
    return xps_intc_ier;
  case 0x18: {
    const uint32_t pending = xps_intc_isr & xps_intc_ier;
    if (pending == 0) {
      return 0xffffffffu;
    }
    for (uint32_t i = 0; i < 32; ++i) {
      if (pending & (1u << i)) {
        return i;
      }
    }
    return 0xffffffffu;
  }
  case 0x1c:
    return xps_intc_mer;
  default: {
    auto it = io_words.find(word_addr);
    return it == io_words.end() ? 0u : it->second;
  }
  }
}

void Ref_cpu::refresh_external_interrupt() {
  const bool global_enable = (xps_intc_mer & 0x3u) == 0x3u;
  const bool pending = global_enable && ((xps_intc_isr & xps_intc_ier) != 0);
  if (pending) {
    state.csr[csr_mip] |= MIP_SEIP;
  } else {
    state.csr[csr_mip] &= ~MIP_SEIP;
  }
  state.csr[csr_sip] = visible_sip();
  if (kLogVerboseXpsIntc && (xps_intc_isr != 0 || xps_intc_ier != 0 ||
                             xps_intc_mer != 0 || pending)) {
    std::cerr << "[RefCPU][XPS_INTC] refresh"
              << " isr=0x" << std::hex << xps_intc_isr
              << " ier=0x" << xps_intc_ier
              << " mer=0x" << xps_intc_mer
              << " pending=" << std::dec << pending
              << " mip=0x" << std::hex << state.csr[csr_mip]
              << " sip=0x" << state.csr[csr_sip] << std::dec << std::endl;
  }
}

void Ref_cpu::xps_intc_set_irq_level(uint32_t irq_id, bool asserted) {
  if (irq_id >= 32) {
    return;
  }
  const uint32_t mask = 1u << irq_id;
  if (asserted) {
    xps_intc_isr |= mask;
  } else {
    xps_intc_isr &= ~mask;
  }
  io_words[XPS_INTC_BASE + 0x00] = xps_intc_isr;
  io_words[XPS_INTC_BASE + 0x04] = xps_intc_isr & xps_intc_ier;
  if (kLogVerboseXpsIntc &&
      (irq_id == kUartIrqId || irq_id == kOcsdcCmdIrqId ||
       irq_id == kOcsdcDatIrqId)) {
    std::cerr << "[RefCPU][XPS_INTC] irq" << irq_id
              << (asserted ? " assert" : " clear") << " isr=0x" << std::hex
              << xps_intc_isr << " ier=0x" << xps_intc_ier
              << " mer=0x" << xps_intc_mer
              << " ipr=0x" << (xps_intc_isr & xps_intc_ier) << std::dec
              << std::endl;
  }
  refresh_external_interrupt();
}

void Ref_cpu::xps_intc_write_reg(uint32_t word_addr, uint32_t data) {
  const uint32_t offset = word_addr - XPS_INTC_BASE;
  if (kLogVerboseXpsIntc &&
      (offset == 0x08 || offset == 0x10 || offset == 0x14 ||
       offset == 0x18 || offset == 0x1c)) {
    std::cerr << "[RefCPU][XPS_INTC] write off=0x" << std::hex << offset
              << " data=0x" << data << std::dec << std::endl;
  }
  switch (word_addr - XPS_INTC_BASE) {
  case 0x00: // ISR clear-on-write
    xps_intc_isr &= ~data;
    break;
  case 0x08: // IER
    xps_intc_ier = data;
    break;
  case 0x0c: // IAR
    xps_intc_isr &= ~data;
    break;
  case 0x10: // SIE
    xps_intc_ier |= data;
    break;
  case 0x14: // CIE
    xps_intc_ier &= ~data;
    break;
  case 0x1c: // MER
    xps_intc_mer = data;
    break;
  default:
    io_words[word_addr] = data;
    break;
  }
  io_words[XPS_INTC_BASE + 0x00] = xps_intc_isr;
  io_words[XPS_INTC_BASE + 0x04] = xps_intc_isr & xps_intc_ier;
  io_words[XPS_INTC_BASE + 0x08] = xps_intc_ier;
  io_words[XPS_INTC_BASE + 0x18] = xps_intc_read_reg(XPS_INTC_BASE + 0x18);
  io_words[XPS_INTC_BASE + 0x1c] = xps_intc_mer;
  refresh_external_interrupt();
}

uint32_t Ref_cpu::ocsdc_read_reg(uint32_t word_addr) const {
  auto it = ocsdc_regs.find(word_addr);
  return it == ocsdc_regs.end() ? 0u : it->second;
}

void Ref_cpu::ocsdc_execute_command(uint32_t command, uint32_t argument) {
  constexpr uint32_t kCmdResp48 = 0x1;
  constexpr uint32_t kCmdResp136 = 0x2;
  constexpr uint32_t kCmdDataRead = 0x20;
  constexpr uint32_t kCmdDataWrite = 0x40;
  constexpr uint32_t kCmdIdxShift = 8;
  constexpr uint32_t kCmdIntCc = 0x0001;
  constexpr uint32_t kDatIntCc = 0x01;
  constexpr uint32_t kDatIntTrs = 0x01;

  const uint32_t opcode = command >> kCmdIdxShift;
  const bool wants_resp = command & (kCmdResp48 | kCmdResp136);
  const bool data_read = command & kCmdDataRead;
  const bool data_write = command & kCmdDataWrite;
  const SdCardId card_id = make_sdcard_id(sdcard_image.size());

  ocsdc_regs[OCSDC_BASE + 0x34] = 0;
  ocsdc_regs[OCSDC_BASE + 0x3c] = 0;
  ocsdc_data_pending = false;

  auto set_r1 = [&](uint32_t resp) {
    ocsdc_regs[OCSDC_BASE + 0x08] = resp;
    ocsdc_regs[OCSDC_BASE + 0x0c] = 0;
    ocsdc_regs[OCSDC_BASE + 0x10] = 0;
    ocsdc_regs[OCSDC_BASE + 0x14] = 0;
  };

  auto write_data_buffer = [&](const uint8_t *src, uint32_t len) {
    const uint32_t dst = ocsdc_read_reg(OCSDC_BASE + 0x60);
    check_mem_range_or_log("ocsdc dma read", dst, len);
    for (uint32_t i = 0; i < len; ++i) {
      const uint32_t addr = dst + i;
      const uint32_t word_addr = addr & ~0x3u;
      const uint32_t shift = (addr & 0x3u) * 8;
      uint32_t old_word = load_word(word_addr);
      old_word &= ~(0xffu << shift);
      old_word |= static_cast<uint32_t>(src[i]) << shift;
      store_word(word_addr, old_word);
    }
#ifdef ENABLE_SPIKE
    if (difftest_started && spike_ref) {
      spike_ref->sync_memory_range_from_dut(dst, len, memory, sdram_memory);
    }
#endif
  };

  switch (opcode) {
  case 0:
    set_r1(0);
    break;
  case 8:
    set_r1(argument);
    break;
  case 55:
    set_r1(0x20);
    break;
  case 41:
    set_r1(card_id.ocr);
    break;
  case 2:
    ocsdc_regs[OCSDC_BASE + 0x08] = card_id.cid[0];
    ocsdc_regs[OCSDC_BASE + 0x0c] = card_id.cid[1];
    ocsdc_regs[OCSDC_BASE + 0x10] = card_id.cid[2];
    ocsdc_regs[OCSDC_BASE + 0x14] = card_id.cid[3];
    break;
  case 3:
    set_r1(1u << 16);
    break;
  case 7:
    set_r1(0x00000900u);
    break;
  case 9:
    ocsdc_regs[OCSDC_BASE + 0x08] = card_id.csd[0];
    ocsdc_regs[OCSDC_BASE + 0x0c] = card_id.csd[1];
    ocsdc_regs[OCSDC_BASE + 0x10] = card_id.csd[2];
    ocsdc_regs[OCSDC_BASE + 0x14] = card_id.csd[3];
    break;
  case 13:
    set_r1(0x00000900u | (4u << 9) | (1u << 8));
    break;
  case 16:
    set_r1(0);
    break;
  case 17:
    set_r1(0);
    break;
  case 18:
    set_r1(0x00000900u);
    break;
  case 6:
    set_r1(0);
    break;
  default:
    set_r1(0);
    break;
  }

  if (kLogVerboseOcsdcData &&
      (opcode == 41 || opcode == 9 || opcode == 17 || opcode == 18)) {
    std::cerr << "[RefCPU][OCSDC] opcode=" << opcode << " arg=0x" << std::hex
              << argument << " resp0=0x" << ocsdc_regs[OCSDC_BASE + 0x08]
              << " resp1=0x" << ocsdc_regs[OCSDC_BASE + 0x0c]
              << " resp2=0x" << ocsdc_regs[OCSDC_BASE + 0x10]
              << " resp3=0x" << ocsdc_regs[OCSDC_BASE + 0x14] << std::dec
              << std::endl;
  }

  if (data_read || data_write) {
    const uint32_t dst = ocsdc_read_reg(OCSDC_BASE + 0x60);
    const uint32_t blk_size = (ocsdc_read_reg(OCSDC_BASE + 0x44) & 0xffffu) + 1u;
    const uint32_t blk_count = (ocsdc_read_reg(OCSDC_BASE + 0x48) & 0xffffu) + 1u;
    const uint64_t total_bytes =
        static_cast<uint64_t>(blk_size) * static_cast<uint64_t>(blk_count);
    const uint64_t card_offset =
        static_cast<uint64_t>(argument) * static_cast<uint64_t>(kSdSectorSize);

    if (kLogVerboseOcsdcData &&
        (opcode == 17 || opcode == 18 || opcode == 41 || opcode == 9)) {
      std::cerr << "[RefCPU][OCSDC] cmd=" << opcode << " arg=0x" << std::hex
                << argument << " blk_size=0x" << blk_size
                << " blk_count=0x" << blk_count << " card_offset=0x"
                << card_offset << std::dec << std::endl;
    }

    if (data_read) {
      if (opcode == 51 && blk_size == 8 && blk_count == 1) {
        // Linux mmc_app_send_scr() DMA-loads 8 raw bytes, then converts each
        // 32-bit lane with be32_to_cpu(). Encode SCR as:
        //   raw_scr[0] = 0x02050000 -> spec v2, 1-bit + 4-bit support
        //   raw_scr[1] = 0x00000000
        uint8_t scr[8] = {0x02, 0x05, 0x00, 0x00,
                          0x00, 0x00, 0x00, 0x00};
        write_data_buffer(scr, sizeof(scr));
      } else if (opcode == 6 && blk_size == 64 && blk_count == 1) {
        uint32_t switch_status[16] = {};
        const bool do_switch = (argument >> 31) & 1u;
        switch_status[3] = 0x00020000u;
        switch_status[4] = do_switch ? 0x01000000u : 0x00000000u;
        switch_status[7] = 0x00000000u;
        write_data_buffer(reinterpret_cast<const uint8_t *>(switch_status),
                          sizeof(switch_status));
      } else {
        if (sdcard_image.empty()) {
          std::cerr << "[RefCPU] OCSDC read requested but no sdcard image loaded"
                    << std::endl;
          std::exit(1);
        }
        if (card_offset + total_bytes > sdcard_image.size()) {
          std::cerr << "[RefCPU] OCSDC read beyond sdcard image: offset=0x"
                    << std::hex << card_offset << " bytes=0x" << total_bytes
                    << " image_size=0x" << sdcard_image.size() << std::dec
                    << std::endl;
          std::exit(1);
        }
        write_data_buffer(sdcard_image.data() + card_offset,
                          static_cast<uint32_t>(total_bytes));
      }
      if (ref_ocsdc_probe_trace_enabled()) {
        std::cerr << "[REF][t=" << sim_time << "][OCSDC] dma-read opcode="
                  << std::dec << opcode << " arg=0x" << std::hex << argument
                  << " blk_size=0x" << blk_size << " blk_count=0x"
                  << blk_count << " dst=0x" << dst << " first=0x"
                  << load_word(dst & ~0x3u) << " bytes=0x" << total_bytes
                  << std::dec << std::endl;
      }
    }
    ocsdc_regs[OCSDC_BASE + 0x3c] = kDatIntCc | kDatIntTrs;
    ocsdc_data_pending = true;
    if (ocsdc_read_reg(OCSDC_BASE + 0x40) != 0) {
      xps_intc_set_irq_level(kOcsdcDatIrqId, true);
    }
  }

  if (wants_resp || opcode == 0) {
    ocsdc_regs[OCSDC_BASE + 0x34] = kCmdIntCc;
    if (ocsdc_read_reg(OCSDC_BASE + 0x38) != 0) {
      xps_intc_set_irq_level(kOcsdcCmdIrqId, true);
    }
  }
}

void Ref_cpu::ocsdc_write_reg(uint32_t word_addr, uint32_t data) {
  switch (word_addr - OCSDC_BASE) {
  case 0x28:
    ocsdc_regs[word_addr] = data;
    if (data & 1u)
      ocsdc_reset();
    break;
  case 0x34:
    ocsdc_regs[word_addr] = 0;
    xps_intc_set_irq_level(kOcsdcCmdIrqId, false);
    break;
  case 0x3c:
    ocsdc_regs[word_addr] = 0;
    xps_intc_set_irq_level(kOcsdcDatIrqId, false);
    break;
  case 0x38:
  case 0x40:
    ocsdc_regs[word_addr] = data;
    if (kLogVerboseOcsdcIrqEnable) {
      std::cerr << "[RefCPU][OCSDC] int_enable off=0x" << std::hex
                << (word_addr - OCSDC_BASE) << " data=0x" << data
                << std::dec << std::endl;
    }
    if ((word_addr - OCSDC_BASE) == 0x38) {
      if ((data != 0) && (ocsdc_read_reg(OCSDC_BASE + 0x34) != 0)) {
        xps_intc_set_irq_level(kOcsdcCmdIrqId, true);
      } else if (data == 0) {
        xps_intc_set_irq_level(kOcsdcCmdIrqId, false);
      }
    } else {
      if ((data != 0) && (ocsdc_read_reg(OCSDC_BASE + 0x3c) != 0)) {
        xps_intc_set_irq_level(kOcsdcDatIrqId, true);
      } else if (data == 0) {
        xps_intc_set_irq_level(kOcsdcDatIrqId, false);
      }
    }
    break;
  case 0x00:
    ocsdc_regs[word_addr] = data;
    ocsdc_execute_command(ocsdc_read_reg(OCSDC_BASE + 0x04), data);
    break;
  case 0x04:
    ocsdc_regs[word_addr] = data;
    break;
  default:
    ocsdc_regs[word_addr] = data;
    break;
  }
}

uint32_t Ref_cpu::checkpoint_mmio_read_word(uint32_t word_addr) const {
  word_addr &= ~0x3u;
  if (word_addr >= TIMER_BASE &&
      word_addr < (TIMER_BASE + TIMER_MMIO_SIZE)) {
    switch (word_addr - TIMER_BASE) {
    case 0x00:
      return static_cast<uint32_t>(ref_timer_now(sim_time));
    case 0x04:
      return static_cast<uint32_t>(ref_timer_now(sim_time) >> 32);
    case 0x08:
      return static_cast<uint32_t>(g_ref_timer_cmp);
    case 0x0c:
      return static_cast<uint32_t>(g_ref_timer_cmp >> 32);
    default:
      return 0;
    }
  }
  if (word_addr >= XPS_INTC_BASE &&
      word_addr < (XPS_INTC_BASE + XPS_INTC_MMIO_SIZE)) {
    return xps_intc_read_reg(word_addr);
  }
  if (word_addr >= OCSDC_BASE &&
      word_addr < (OCSDC_BASE + OCSDC_MMIO_SIZE)) {
    return ocsdc_read_reg(word_addr);
  }
  auto it = io_words.find(word_addr);
  return it == io_words.end() ? 0u : it->second;
}

void Ref_cpu::checkpoint_mmio_write_backing(uint32_t word_addr,
                                             uint32_t data) {
  word_addr &= ~0x3u;
  if (data == 0) {
    io_words.erase(word_addr);
  } else {
    io_words[word_addr] = data;
  }
}

void Ref_cpu::checkpoint_mmio_sync_devices() {
  const auto backing_word = [&](uint32_t addr) {
    auto it = io_words.find(addr & ~0x3u);
    return it == io_words.end() ? 0u : it->second;
  };

  const uint64_t timer_value =
      static_cast<uint64_t>(backing_word(TIMER_BASE)) |
      (static_cast<uint64_t>(backing_word(TIMER_BASE + 4u)) << 32);
  g_ref_timer_offset = timer_value - sim_time;
  g_ref_timer_cmp =
      static_cast<uint64_t>(backing_word(TIMER_BASE + 8u)) |
      (static_cast<uint64_t>(backing_word(TIMER_BASE + 12u)) << 32);

  xps_intc_isr = backing_word(XPS_INTC_BASE + 0x00u);
  xps_intc_ier = backing_word(XPS_INTC_BASE + 0x08u);
  xps_intc_mer = backing_word(XPS_INTC_BASE + 0x1cu);

  ocsdc_regs.clear();
  for (uint32_t off = 0; off + 4u <= OCSDC_MMIO_SIZE; off += 4u) {
    ocsdc_regs[OCSDC_BASE + off] = backing_word(OCSDC_BASE + off);
  }

  refresh_external_interrupt();
}

// ---- XLEN-generic integer helpers (rv32: 32-bit values, rv64: 64-bit values) ----
static inline xlen_t sext32(uint32_t v) { return static_cast<xlen_t>(static_cast<sxlen_t>(static_cast<int32_t>(v))); }
static inline xlen_t xlen_rol(xlen_t v, unsigned s) { s &= kShamtMask; return s ? (v << s) | (v >> (XLEN - s)) : v; }
static inline xlen_t xlen_ror(xlen_t v, unsigned s) { s &= kShamtMask; return s ? (v >> s) | (v << (XLEN - s)) : v; }
static inline xlen_t xlen_clz(xlen_t v) { return v == 0 ? XLEN : (XLEN == 64 ? __builtin_clzll(v) : __builtin_clz(static_cast<uint32_t>(v))); }
static inline xlen_t xlen_ctz(xlen_t v) { return v == 0 ? XLEN : (XLEN == 64 ? __builtin_ctzll(v) : __builtin_ctz(static_cast<uint32_t>(v))); }
static inline xlen_t xlen_cpop(xlen_t v) { return XLEN == 64 ? __builtin_popcountll(v) : __builtin_popcount(static_cast<uint32_t>(v)); }
static inline xlen_t xlen_rev8(xlen_t v) { return XLEN == 64 ? __builtin_bswap64(v) : __builtin_bswap32(static_cast<uint32_t>(v)); }
static inline xlen_t xlen_orcb(xlen_t v) {
  xlen_t r = 0;
  for (unsigned i = 0; i < XLEN; i += 8) if ((v >> i) & 0xFF) r |= xlen_t(0xFF) << i;
  return r;
}
static inline xlen_t mul_hi_ss(xlen_t a, xlen_t b) {
#if XLEN == 64
  return static_cast<xlen_t>((static_cast<__int128>(static_cast<int64_t>(a)) * static_cast<int64_t>(b)) >> 64);
#else
  return static_cast<uint32_t>((static_cast<int64_t>(static_cast<int32_t>(a)) * static_cast<int64_t>(static_cast<int32_t>(b))) >> 32);
#endif
}
static inline xlen_t mul_hi_su(xlen_t a, xlen_t b) {
#if XLEN == 64
  return static_cast<xlen_t>((static_cast<__int128>(static_cast<int64_t>(a)) * static_cast<__int128>(static_cast<unsigned __int128>(b))) >> 64);
#else
  return static_cast<uint32_t>((static_cast<int64_t>(static_cast<int32_t>(a)) * static_cast<int64_t>(static_cast<uint64_t>(b))) >> 32);
#endif
}
static inline xlen_t mul_hi_uu(xlen_t a, xlen_t b) {
#if XLEN == 64
  return static_cast<xlen_t>((static_cast<unsigned __int128>(a) * b) >> 64);
#else
  return static_cast<uint32_t>((static_cast<uint64_t>(a) * static_cast<uint64_t>(b)) >> 32);
#endif
}
static inline xlen_t xlen_div(sxlen_t a, sxlen_t b) {
  if (b == 0) return static_cast<xlen_t>(-1);
  if (a == static_cast<sxlen_t>(kXlenMsb) && b == -1) return static_cast<xlen_t>(a);
  return static_cast<xlen_t>(a / b);
}
static inline xlen_t xlen_rem(sxlen_t a, sxlen_t b) {
  if (b == 0) return static_cast<xlen_t>(a);
  if (a == static_cast<sxlen_t>(kXlenMsb) && b == -1) return 0;
  return static_cast<xlen_t>(a % b);
}
static inline xlen_t xlen_divu(xlen_t a, xlen_t b) { return b == 0 ? ~xlen_t(0) : a / b; }
static inline xlen_t xlen_remu(xlen_t a, xlen_t b) { return b == 0 ? a : a % b; }
// 32-bit (*W) flavours on rv64: operate on the low 32 bits, results are sign-extended.
static inline xlen_t w_div(int32_t a, int32_t b) {
  if (b == 0) return sext32(~0u);
  if (a == INT32_MIN && b == -1) return sext32(static_cast<uint32_t>(a));
  return sext32(static_cast<uint32_t>(a / b));
}
static inline xlen_t w_rem(int32_t a, int32_t b) {
  if (b == 0) return sext32(static_cast<uint32_t>(a));
  if (a == INT32_MIN && b == -1) return 0;
  return sext32(static_cast<uint32_t>(a % b));
}
static inline xlen_t w_divu(uint32_t a, uint32_t b) { return sext32(b == 0 ? ~0u : a / b); }
static inline xlen_t w_remu(uint32_t a, uint32_t b) { return sext32(b == 0 ? a : a % b); }
static inline uint32_t w_rol(uint32_t v, unsigned s) { s &= 31; return s ? (v << s) | (v >> (32 - s)) : v; }
static inline uint32_t w_ror(uint32_t v, unsigned s) { s &= 31; return s ? (v >> s) | (v << (32 - s)) : v; }
static inline xlen_t xlen_clmul(xlen_t a, xlen_t b) {
  xlen_t out = 0;
  for (unsigned i = 0; i < XLEN; i++) if ((b >> i) & 1) out ^= (a << i);
  return out;
}
static inline xlen_t xlen_clmulr(xlen_t a, xlen_t b) {
  xlen_t out = 0;
  for (unsigned i = 0; i < XLEN; i++) if ((b >> i) & 1) out ^= (a >> (XLEN - 1 - i));
  return out;
}
static inline xlen_t xlen_clmulh(xlen_t a, xlen_t b) {
  xlen_t out = 0;
  for (unsigned i = 1; i < XLEN; i++) if ((b >> i) & 1) out ^= (a >> (XLEN - i));
  return out;
}

void Ref_cpu::RV32A() {
  // pc + 4
  xlen_t next_pc = state.pc + 4;
  uint32_t funct5 = BITS(Instruction, 31, 27);
  uint32_t funct3 = BITS(Instruction, 14, 12);
  uint32_t reg_d_index = BITS(Instruction, 11, 7);
  uint32_t reg_a_index = BITS(Instruction, 19, 15);
  uint32_t reg_b_index = BITS(Instruction, 24, 20);

  const xlen_t reg_rdata1 = state.gpr[reg_a_index];
  const xlen_t reg_rdata2 = state.gpr[reg_b_index];

#if XLEN == 64
  const bool dword = funct3 == 3;
  if (funct3 != 2 && funct3 != 3) {
    illegal_exception = true;
    return;
  }
#else
  const bool dword = false;
  (void)funct3;
#endif
  const uint32_t amo_size = dword ? 8 : 4;

  xlen_t v_addr = reg_rdata1;
  uint32_t p_addr = static_cast<uint32_t>(v_addr);

  if (v_addr % amo_size != 0) {
    std::cerr << "Misaligned AMO Access! addr: 0x" << std::hex << v_addr << std::endl;
    exit(-1);
  }

  const xlen_t amo_mstatus = state.csr[csr_mstatus];
  const uint32_t amo_eff_priv =
      (amo_mstatus & MSTATUS_MPRV) ? GET_MPP(amo_mstatus) : privilege;
  if (satp_enabled(state.csr[csr_satp]) && amo_eff_priv != RISCV_MODE_M) {
    bool page_fault_1 = !va2pa(p_addr, v_addr, 1);
    bool page_fault_2 = !va2pa(p_addr, v_addr, 2);

    if (page_fault_1 || page_fault_2) {
      if (funct5 == 2) {
        if (page_fault_1) {
          page_fault_load = true;
        }
      } else if (funct5 == 3) {
        if (page_fault_2) {
          page_fault_store = true;
        }
      } else {
        page_fault_store = true;
      }
    }

    if (page_fault_load || page_fault_store) {
      exception(v_addr);
      return;
    }
  } else {
    p_addr = bare_pa(v_addr);
  }
  if (!is_ram_range(p_addr, amo_size)) {
    is_io = true;
  }
  check_mem_range_or_log("amo", p_addr, amo_size);

  if (funct5 != 2) {
    state.store = true;
    state.store_addr = p_addr;
    state.store_strb = dword ? 0xFF : 0b1111;
  }
  // old memory value, as the register image (sign-extended for .w) and as raw bits
  xlen_t old_val;
  if (dword) {
    old_val = (static_cast<xlen_t>(load_word(p_addr + 4)) << 32) | load_word(p_addr);
  } else {
    old_val = sext32(load_word(p_addr));
  }
  const xlen_t operand = dword ? reg_rdata2 : sext32(static_cast<uint32_t>(reg_rdata2));
  // signed / unsigned comparison on the memory width
  const sxlen_t s_old = static_cast<sxlen_t>(old_val), s_op = static_cast<sxlen_t>(operand);
  const xlen_t u_old = dword ? old_val : (old_val & 0xFFFFFFFFu), u_op = dword ? operand : (operand & 0xFFFFFFFFu);
  auto set_store = [&](xlen_t v) { state.store_data = dword ? v : (v & 0xFFFFFFFFu); };

  switch (funct5) {
  case 0: state.gpr[reg_d_index] = old_val; set_store(old_val + operand); break;   // amoadd
  case 1: state.gpr[reg_d_index] = old_val; set_store(operand); break;             // amoswap
  case 2: {                                                                        // lr
    state.gpr[reg_d_index] = old_val;
    state.reserve_valid = true;
    state.reserve_addr = p_addr;
    break;
  }
  case 3: { // sc
    if (state.reserve_valid && state.reserve_addr == p_addr) {
      set_store(operand);
      state.gpr[reg_d_index] = 0; // Success
    } else {
      state.gpr[reg_d_index] = 1; // Fail
      state.store = false;        // Don't perform the write
    }
    state.reserve_valid = false; // Regardless of success, invalidate reservation
    break;
  }
  case 4: state.gpr[reg_d_index] = old_val; set_store(old_val ^ operand); break;   // amoxor
  case 8: state.gpr[reg_d_index] = old_val; set_store(old_val | operand); break;   // amoor
  case 12: state.gpr[reg_d_index] = old_val; set_store(old_val & operand); break;  // amoand
  case 16: state.gpr[reg_d_index] = old_val; set_store(s_old > s_op ? operand : old_val); break;  // amomin
  case 20: state.gpr[reg_d_index] = old_val; set_store(s_old > s_op ? old_val : operand); break;  // amomax
  case 24: state.gpr[reg_d_index] = old_val; set_store(u_old < u_op ? old_val : operand); break;  // amominu
  case 28: state.gpr[reg_d_index] = old_val; set_store(u_old > u_op ? old_val : operand); break;  // amomaxu
  default: {
    illegal_exception = true;
    break;
  }
  }

  if (illegal_exception) {
    state.store = false;
    return;
  }
  if (state.store) {
    store_data();
  }
  state.pc = next_pc;
}

// ---------------------------------------------------------------------------------------------------------------
// Fast path for plain runs (see ref.h): a basic-block cache with threaded dispatch for the common integer
// instructions.  A block is a run of consecutive fast instructions inside one page, ending at a jump/branch, before an
// instruction the fast path does not handle, at the page end or at kMaxBlockOps.  It is decoded once and validated once
// on entry (virtual pc, (satp, privilege) and an epoch); inside a block each instruction is a computed jump to its
// handler.  A handler either executes the instruction exactly as RV32IM() would, or leaves all state untouched and
// returns to the caller, which then runs RISCV() for that instruction.
namespace {
// Adjacent instruction pairs executed by one handler (chosen from the dynamic pair frequencies of Linux/SPEC runs).  The
// handler is the first instruction's body, 'op' moved to the second slot, then the second body, so a fallback in the
// second instruction (fault, MMIO, misaligned) leaves the first one executed and the block position exact.
// NC: the second instruction does not end the block; C: it is a jump or branch (ends the block).
#define FUSE_LIST_NC(X)                                                                                               \
  X(LW, LW) X(SW, SW) X(ADDI, ADDI) X(LW, ADDI) X(ADDI, SW) X(ADDI, LW) X(SW, LW) X(SW, ADDI) X(LW, SLLI)            \
  X(LW, ANDI) X(ADD, LW) X(SLLI, ADD) X(LUI, ADDI) X(AUIPC, LW) X(AUIPC, ADDI) X(SRLI, ANDI) X(ADD, ADD) X(AND, AND)
#define FUSE_LIST_C(X)                                                                                                \
  X(ANDI, BEQ) X(ANDI, BNE) X(LW, BEQ) X(LW, BNE) X(ADDI, BEQ) X(ADDI, BNE) X(ADDI, BGE) X(AND, BNE) X(ADDI, JALR)   \
  X(ADDI, JAL) X(AUIPC, JALR)

enum FastOp : uint8_t {
  F_SLOW = 0, F_NOP, F_CONT, F_EXIT,
  F_LUI, F_AUIPC, F_JAL, F_JALR,
  F_BEQ, F_BNE, F_BLT, F_BGE, F_BLTU, F_BGEU,
  F_LB, F_LH, F_LW, F_LBU, F_LHU, F_LWU, F_LD,
  F_SB, F_SH, F_SW, F_SD,
  F_CSR,
  F_ADDI, F_SLTI, F_SLTIU, F_XORI, F_ORI, F_ANDI, F_SLLI, F_SRLI, F_SRAI,
  F_ADD, F_SUB, F_SLL, F_SLT, F_SLTU, F_XOR, F_SRL, F_SRA, F_OR, F_AND,
  F_MUL, F_MULH, F_MULHSU, F_MULHU, F_DIV, F_DIVU, F_REM, F_REMU,
  // rv64 only (never produced by the rv32 decode)
  F_ADDIW, F_SLLIW, F_SRLIW, F_SRAIW, F_ADDW, F_SUBW, F_SLLW, F_SRLW, F_SRAW, F_MULW, F_DIVW, F_DIVUW, F_REMW, F_REMUW,
  // fused pairs (produced only by the fusion pass, never by decode): the first slot runs both instructions
#define X(a, b) F_##a##_##b,
  FUSE_LIST_NC(X) FUSE_LIST_C(X)
#undef X
  F_NUM
};

// REF_NO_FAST=1 runs everything through RISCV(); REF_STATE_DIGEST=N prints a hash of (pc, x1..x31) every N
// instructions (the fast path then uses one-instruction blocks).  The two together compare the fast path against the
// original path instruction by instruction.
const bool g_no_fast = std::getenv("REF_NO_FAST") != nullptr;
const uint64_t g_digest_every =
    std::getenv("REF_STATE_DIGEST") ? std::strtoull(std::getenv("REF_STATE_DIGEST"), nullptr, 0) : 0;

// REF_FAST_STATS=1: report, at exit, how many instructions the fast path ran and why it handed over to RISCV()
const bool g_fast_stats = std::getenv("REF_FAST_STATS") != nullptr;
struct FastStats {
  uint64_t fast_insts = 0, runs = 0, blocks = 0, stop[8] = {0}, slow_op[128 * 8] = {0};
  uint64_t pair[128][128] = {};  // dynamic count of adjacent op pairs inside executed blocks
  ~FastStats() {
    if (!g_fast_stats) return;
    fprintf(stderr,
            "[fast] instructions=%llu runs=%llu blocks_built=%llu stops: interrupt/timer=%llu budget=%llu "
            "fill_fail=%llu slow_op=%llu mem_fallback=%llu\n",
            (unsigned long long)fast_insts, (unsigned long long)runs, (unsigned long long)blocks,
            (unsigned long long)stop[0], (unsigned long long)stop[1], (unsigned long long)stop[3],
            (unsigned long long)stop[4], (unsigned long long)stop[5]);
    for (int pass = 0; pass < 24; pass++) {
      int bi = -1, bj = -1;
      uint64_t bc = 0;
      for (int i = 0; i < 128; i++)
        for (int j = 0; j < 128; j++)
          if (pair[i][j] > bc) { bc = pair[i][j]; bi = i; bj = j; }
      if (bi < 0) break;
      fprintf(stderr, "[fast]   pair op%d,op%d : %llu\n", bi, bj, (unsigned long long)bc);
      pair[bi][bj] = 0;
    }
    for (int pass = 0; pass < 12; pass++) {
      int best = -1;
      uint64_t bc = 0;
      for (int i = 0; i < 128 * 8; i++)
        if (slow_op[i] > bc) { bc = slow_op[i]; best = i; }
      if (best < 0) break;
      fprintf(stderr, "[fast]   slow opcode=0x%02x funct3=%d : %llu\n", best / 8, best % 8, (unsigned long long)bc);
      slow_op[best] = 0;
    }
  }
} g_fs;

// REF_STATE_DIGEST_BLOCK=N: the fast path keeps its normal blocks and prints a digest at the first block boundary at or
// after every N instructions (with the exact instruction count); REF_DIGEST_LIST=file then makes a REF_NO_FAST run
// print digests at exactly those counts, so both can be compared.
const uint64_t g_digest_block =
    std::getenv("REF_STATE_DIGEST_BLOCK") ? std::strtoull(std::getenv("REF_STATE_DIGEST_BLOCK"), nullptr, 0) : 0;
uint64_t g_next_digest = g_digest_block;
std::vector<uint64_t> g_digest_list = [] {
  std::vector<uint64_t> v;
  if (const char *f = std::getenv("REF_DIGEST_LIST")) {
    FILE *fp = std::fopen(f, "r");
    unsigned long long t;
    while (fp && std::fscanf(fp, "%llu", &t) == 1) v.push_back(t);
    if (fp) std::fclose(fp);
  }
  return v;
}();
size_t g_digest_list_pos = 0;

// REF_STATE_DIGEST_FULL=1: the digests also cover the privilege and the CSRs, except the bits a front-door restore cannot
// reproduce (mstatus.MPP/MPIE, mepc and their sstatus shadow): mret sets MPP to U and MPIE to 1, and mepc to the target.
const bool g_digest_full = std::getenv("REF_STATE_DIGEST_FULL") != nullptr;

void print_state_digest(uint64_t t, const CPU_state &st, uint8_t priv = 0) {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&h](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
  mix(st.pc);
  for (int i = 1; i < 32; i++) mix(st.gpr[i]);
#if FP
  for (int i = 0; i < 32; i++) mix(st.fpr[i]);
#endif
  if (g_digest_full) {
    mix(priv);
    for (int i = 0; i < 21; i++) {
      if (i == csr_mepc || i == csr_sip) continue;  // sip is a shadow that depends on the timer at the moment of the last sync
      uint32_t v = st.csr[i];
      if (i == csr_mstatus || i == csr_sstatus) v &= ~0x1880u;
      mix(v);
    }
  }
  fprintf(stderr, "[digest] t=%llu h=%016llx\n", (unsigned long long)t, (unsigned long long)h);
}

// Classify an instruction word.  Only exact base-ISA / M encodings are fast; everything else (Zba/Zbb/Zbc/Zbs,
// CSR, AMO, FP, system, unknown funct3/funct7 combinations) is F_SLOW and goes through RISCV().
void fast_decode(uint32_t in, Ref_cpu::FastInsn &e) {
  e.rd = BITS(in, 11, 7);
  e.rs1 = BITS(in, 19, 15);
  e.rs2 = BITS(in, 24, 20);
  e.imm = 0;
  const uint32_t opc = BITS(in, 6, 0), f3 = BITS(in, 14, 12), f7 = BITS(in, 31, 25);
  const uint32_t f7s = (XLEN == 64) ? (f7 & ~1u) : f7;   // shift immediates: rv64 has a 6-bit shamt
  uint8_t op = F_SLOW;
  switch (opc) {
  case number_0_opcode_lui: op = F_LUI; e.imm = immU(in); break;
  case number_1_opcode_auipc: op = F_AUIPC; e.imm = immU(in); break;
  case number_2_opcode_jal: op = F_JAL; e.imm = immJ(in); break;
  case number_3_opcode_jalr: op = F_JALR; e.imm = immI(in); break;
  case number_4_opcode_beq:
    e.imm = immB(in);
    switch (f3) {
    case 0: op = F_BEQ; break;
    case 1: op = F_BNE; break;
    case 4: op = F_BLT; break;
    case 5: op = F_BGE; break;
    case 6: op = F_BLTU; break;
    case 7: op = F_BGEU; break;
    }
    break;
  case number_5_opcode_lb:
    e.imm = immI(in);
    switch (f3) {
    case 0: op = F_LB; break;
    case 1: op = F_LH; break;
    case 2: op = F_LW; break;
    case 4: op = F_LBU; break;
    case 5: op = F_LHU; break;
#if XLEN == 64
    case 6: op = F_LWU; break;
    case 3: op = F_LD; break;
#endif
    }
    break;
  case number_6_opcode_sb:
    e.imm = immS(in);
    switch (f3) {
    case 0: op = F_SB; break;
    case 1: op = F_SH; break;
    case 2: op = F_SW; break;
#if XLEN == 64
    case 3: op = F_SD; break;
#endif
    }
    break;
  case number_7_opcode_addi:
    e.imm = immI(in);
    switch (f3) {
    case 0: op = F_ADDI; break;
    case 2: op = F_SLTI; break;
    case 3: op = F_SLTIU; break;
    case 4: op = F_XORI; break;
    case 6: op = F_ORI; break;
    case 7: op = F_ANDI; break;
    case 1: if (f7s == 0) { op = F_SLLI; e.imm &= kShamtMask; } break;
    case 5:
      if (f7s == 0) { op = F_SRLI; e.imm &= kShamtMask; }
      else if (f7s == 0x20) { op = F_SRAI; e.imm &= kShamtMask; }
      break;
    }
    break;
  case number_8_opcode_add:
    if (f7 == 1) {
      static const uint8_t m[8] = {F_MUL, F_MULH, F_MULHSU, F_MULHU, F_DIV, F_DIVU, F_REM, F_REMU};
      op = m[f3];
    } else if (f7 == 0) {
      static const uint8_t b[8] = {F_ADD, F_SLL, F_SLT, F_SLTU, F_XOR, F_SRL, F_OR, F_AND};
      op = b[f3];
    } else if (f7 == 0x20) {
      if (f3 == 0) op = F_SUB;
      else if (f3 == 5) op = F_SRA;
    }
    break;
#if XLEN == 64
  case 0b0011011:  // OP-IMM-32
    e.imm = immI(in);
    switch (f3) {
    case 0: op = F_ADDIW; break;
    case 1: if (f7 == 0) { op = F_SLLIW; e.imm &= 0x1F; } break;
    case 5:
      if (f7 == 0) { op = F_SRLIW; e.imm &= 0x1F; }
      else if (f7 == 0x20) { op = F_SRAIW; e.imm &= 0x1F; }
      break;
    }
    break;
  case 0b0111011:  // OP-32
    if (f7 == 1) {
      switch (f3) {
      case 0: op = F_MULW; break;
      case 4: op = F_DIVW; break;
      case 5: op = F_DIVUW; break;
      case 6: op = F_REMW; break;
      case 7: op = F_REMUW; break;
      }
    } else if (f7 == 0) {
      if (f3 == 0) op = F_ADDW;
      else if (f3 == 1) op = F_SLLW;
      else if (f3 == 5) op = F_SRLW;
    } else if (f7 == 0x20) {
      if (f3 == 0) op = F_SUBW;
      else if (f3 == 5) op = F_SRAW;
    }
    break;
#endif
  case number_9_opcode_fence: op = F_NOP; break;
  case number_10_opcode_ecall:
    if (f3 != 0 && f3 != 4) { op = F_CSR; e.imm = in; }  // executed by RV32CSR(); funct3 0 (ecall, mret, wfi, ...) is slow
    break;
  }
  // an instruction without side effects that writes x0 does nothing
  if (e.rd == 0 && (op == F_LUI || op == F_AUIPC || (op >= F_ADDI && op != F_SD))) {
    op = F_NOP;
  }
  e.op = op;
}

inline bool fast_is_control(uint8_t op) { return op >= F_JAL && op <= F_BGEU; }
// F_CSR also ends a block: afterwards the fast-path assumptions are re-checked
inline bool fast_ends_block(uint8_t op) { return fast_is_control(op) || op == F_CSR; }
}  // namespace

// Run blocks until an instruction needs the general path, an interrupt could be taken, or the timer deadline is near.
// U: the privilege is U (basic-block and interval bookkeeping); DX: loads and stores are translated.
template <bool U, bool DX>
void Ref_cpu::fast_run_impl(uint32_t &finished_intervals, uint64_t budget, uint32_t mip_eff, bool stop_iv, bool bbv,
                            bool trk) {
  static const void *const tbl[F_NUM] = {
      &&L_SLOW, &&L_NOP, &&L_CONT, &&L_EXIT,
      &&L_LUI, &&L_AUIPC, &&L_JAL, &&L_JALR,
      &&L_BEQ, &&L_BNE, &&L_BLT, &&L_BGE, &&L_BLTU, &&L_BGEU,
      &&L_LB, &&L_LH, &&L_LW, &&L_LBU, &&L_LHU, &&L_LWU, &&L_LD,
      &&L_SB, &&L_SH, &&L_SW, &&L_SD,
      &&L_CSR,
      &&L_ADDI, &&L_SLTI, &&L_SLTIU, &&L_XORI, &&L_ORI, &&L_ANDI, &&L_SLLI, &&L_SRLI, &&L_SRAI,
      &&L_ADD, &&L_SUB, &&L_SLL, &&L_SLT, &&L_SLTU, &&L_XOR, &&L_SRL, &&L_SRA, &&L_OR, &&L_AND,
      &&L_MUL, &&L_MULH, &&L_MULHSU, &&L_MULHU, &&L_DIV, &&L_DIVU, &&L_REM, &&L_REMU,
      &&L_ADDIW, &&L_SLLIW, &&L_SRLIW, &&L_SRAIW, &&L_ADDW, &&L_SUBW, &&L_SLLW, &&L_SRLW, &&L_SRAW, &&L_MULW, &&L_DIVW,
      &&L_DIVUW, &&L_REMW, &&L_REMUW,
#define X(a, b) &&L_F_##a##_##b,
      FUSE_LIST_NC(X) FUSE_LIST_C(X)
#undef X
  };

  xlen_t *const x = state.gpr;
  uint32_t *const mem = memory;
  uint8_t *const pgf = pt_page_flag.data();
  const xlen_t satp = state.csr[csr_satp];
  const uint32_t priv = privilege;
  const bool fetch_translate = satp_enabled(satp) && priv != RISCV_MODE_M;
  // rv32: satp fits in the context word; rv64: FastBlock::satp is compared separately
  const uint64_t ctx_base = (XLEN == 32 ? (static_cast<uint64_t>(satp) << 32) : 0) | (static_cast<uint64_t>(priv) << 30);
  const uint32_t maxlen = g_digest_every ? 1u : kMaxBlockOps;
  FastBlock *const blocks = fblocks.data();

  uint64_t left = budget;             // instructions still allowed
  const uint64_t budget0 = budget;
  const uint64_t st0 = sim_time;      // sim_time at entry; the member is brought up to date at exit and before RV32CSR()
  const uint32_t mie0 = state.csr[csr_mie];
  const uint32_t mip0 = state.csr[csr_mip];
  const uint32_t mst0 = state.csr[csr_mstatus];
  constexpr uint32_t kMstatusDataMask = MSTATUS_MPRV | MSTATUS_MPP | MSTATUS_MXR | MSTATUS_SUM;  // what data translation depends on
  const uint32_t data_priv0 = (mst0 & MSTATUS_MPRV) ? GET_MPP(mst0) : priv;
  const bool irq_possible = (mip_eff & mie0 & 0x00000AAAu) != 0;  // something is pending and enabled (but not deliverable now)
  // TLB context of a load / store, exactly as va2pa() forms it (access type, effective privilege, MXR, SUM); the TLB
  // is probed inline below and va2pa() is only called on a miss
  const uint32_t tctx_common = (data_priv0 << 2) | ((mst0 & MSTATUS_MXR) ? 16u : 0u) | ((mst0 & MSTATUS_SUM) ? 32u : 0u);
  const uint32_t tctx_ld = 1u | tctx_common, tctx_st = 2u | tctx_common;
  constexpr uint32_t kTlbMask = Ref_cpu::kTlbSize - 1;
  xlen_t pc = state.pc;
  uint32_t ep = icache_epoch;         // epoch the current ctx was built with
  uint64_t ctx = ctx_base | ep;
  uint64_t bbl = current_bb_len;      // U mode: instructions in the current basic block (written back at exit)
  uint64_t last_digest = 0;

  FastBlock *b = nullptr;
  const FastInsn *op = nullptr;
  xlen_t bpc = 0, ipc = 0, v = 0;
  uint32_t p = 0, n = 0, ex = 0;
  const FastInsn *ip = nullptr;
  uint32_t q = 0;
  xlen_t cpc = 0;
  uint32_t cnt = 0;

dispatch_block:
  if (__builtin_expect(g_digest_block != 0, 0)) {
    const uint64_t c = budget0 - left;
    if (c > 0 && st0 + c >= g_next_digest) {
      state.pc = pc;
      print_state_digest(st0 + c, state, priv);
      g_next_digest = st0 + c + g_digest_block;
    }
  }
  if (__builtin_expect(g_digest_every != 0, 0)) {
    const uint64_t c = budget0 - left;
    if (c > 0 && c != last_digest && (st0 + c) % g_digest_every == 0) {
      state.pc = pc;
      print_state_digest(st0 + c, state, priv);
      last_digest = c;
    }
  }
  if (pc & 3u) {
    goto out;  // misaligned pc: the general path fetches the aligned word
  }
  b = &blocks[(pc >> 2) & (kFastBlocks - 1)];
  if (__builtin_expect(b->pc != pc || b->ctx != ctx || (XLEN == 64 && b->satp != satp), 0)) {
    goto build;
  }
run_block:
  if (left < b->n) {
    if (g_fast_stats) g_fs.stop[1]++;
    goto out;  // not enough budget left for the whole block: the general path steps the last instructions
  }
  left -= b->n;
  if (__builtin_expect(g_fast_stats, 0)) {
    for (uint32_t k = 0; k + 1 < b->n; k++) g_fs.pair[b->ops[k].op][b->ops[k + 1].op]++;
  }
  bpc = pc;
  if (U && bbl == 0) {
    current_bb_head_pc = pc;
  }
  op = b->ops;
  goto *op->h;

build : {
  // decode a new block starting at pc
  p = static_cast<uint32_t>(pc);
  if (fetch_translate) {
    if (!va2pa(p, pc, 0)) {
      if (g_fast_stats) g_fs.stop[3]++;
      goto out;  // instruction page fault: RISCV() raises it
    }
  } else if (XLEN == 64 && (pc >> 32) != 0) {
    goto out;  // outside the 32-bit physical space: RISCV() reports it
  }
  if ((int32_t)p >= 0) {
    if (g_fast_stats) g_fs.stop[3]++;
    goto out;  // not DDR (SDRAM, MMIO, boot ROM): RISCV() fetches it
  }
  n = 0;
  q = p;
  cpc = pc;
  uint8_t end_op = F_CONT;
  for (;;) {
    if (n == maxlen) { end_op = F_CONT; break; }
    FastInsn &fi = b->ops[n];
    fast_decode(mem[(q & 0x7FFFFFFFu) >> 2], fi);
    if (fi.op == F_SLOW) {
      if (g_fast_stats) g_fs.slow_op[(mem[(q & 0x7FFFFFFFu) >> 2] & 0x7F) * 8 + ((mem[(q & 0x7FFFFFFFu) >> 2] >> 12) & 7)]++;
      end_op = F_EXIT;
      break;
    }
    fi.h = tbl[fi.op];
    n++;
    q += 4;
    cpc += 4;
    if (fast_ends_block(fi.op)) { end_op = F_NOP; break; }  // a jump, branch or CSR instruction ends the block
    if ((cpc & 0xFFFu) == 0) { end_op = F_CONT; break; }    // the next instruction is on another page
  }
  if (end_op != F_NOP) {
    FastInsn &fi = b->ops[n];
    fi.op = end_op;
    fi.h = tbl[end_op];
  }
  {
    // fuse adjacent pairs (greedy, left to right); slot i runs instructions i and i+1, slot i+1 stays intact
    static const auto fuse_tbl = [] {
      std::array<std::array<uint8_t, F_NUM>, F_NUM> t{};
#define X(a, b) t[F_##a][F_##b] = F_##a##_##b;
      FUSE_LIST_NC(X) FUSE_LIST_C(X)
#undef X
      return t;
    }();
    for (uint32_t i = 0; i + 1 < n;) {
      const uint8_t f = fuse_tbl[b->ops[i].op][b->ops[i + 1].op];
      if (f) {
        b->ops[i].op = f;
        b->ops[i].h = tbl[f];
        i += 2;
      } else {
        i++;
      }
    }
  }
  b->pc = pc;
  b->ctx = ctx;
  b->satp = satp;
  b->n = n;
  mark_code_page(p);
  if (g_fast_stats) g_fs.blocks++;
  if (n == 0) {
    // the first instruction is not a fast one: run the EXIT end op to hand over
  }
  goto run_block;
}

#define NEXT() do { ++op; goto *op->h; } while (0)
#define PC_OF(o) (bpc + (xlen_t)(((o) - b->ops) << 2))
// leaving the block before executing 'op': give back the budget of the instructions not executed
#define BAIL()                                                                                                       \
  do {                                                                                                               \
    ex = (uint32_t)(op - b->ops);                                                                                    \
    left += b->n - ex;                                                                                               \
    if (U) bbl += ex;                                                                                                \
    pc = bpc + (ex << 2);                                                                                            \
    if (g_fast_stats) g_fs.stop[5]++;                                                                                \
    goto out;                                                                                                        \
  } while (0)
// a control instruction ends the block: U-mode bookkeeping, then the next block
#define END_CONTROL()                                                                                                \
  do {                                                                                                               \
    if (U) {                                                                                                         \
      bbl += b->n;                                                                                                   \
      if (__builtin_expect(bbv, 0)) {                                                                                \
        current_bb_len = static_cast<uint32_t>(bbl);                                                                 \
        bbv_commit();                                                                                                \
      }                                                                                                              \
      interval_inst_count += bbl;                                                                                    \
      bbl = 0;                                                                                                       \
      if (interval_inst_count >= INTERVAL_SIZE) {                                                                    \
        if (stop_iv) {                                                                                               \
          goto out; /* the caller handles the interval boundary (pc is already the next pc) */                      \
        }                                                                                                            \
        finished_intervals++;                                                                                        \
        interval_inst_count = 0;                                                                                     \
      }                                                                                                              \
    }                                                                                                                \
    goto dispatch_block;                                                                                             \
  } while (0)

L_SLOW:
  goto out;  // never in a block (F_SLOW ends a block with F_EXIT)
L_NOP:
  NEXT();
L_CONT:
  if (U) bbl += b->n;
  pc = bpc + (b->n << 2);
  goto dispatch_block;
L_EXIT:
  if (U) bbl += b->n;
  pc = bpc + (b->n << 2);
  if (g_fast_stats) g_fs.stop[4]++;
  goto out;

// ---- instruction bodies: each assumes 'op' is its own slot and falls through (or leaves via a macro) without dispatching ----
#define B_LUI x[op->rd] = op->imm;
#define B_AUIPC x[op->rd] = op->imm + PC_OF(op);
#define B_JAL                                                                                                        \
  ipc = PC_OF(op);                                                                                                   \
  x[op->rd] = ipc + 4;                                                                                               \
  x[0] = 0;                                                                                                          \
  pc = ipc + op->imm;                                                                                                \
  END_CONTROL();
#define B_JALR                                                                                                       \
  ipc = PC_OF(op);                                                                                                   \
  v = (x[op->rs1] + op->imm) & ~xlen_t(3);                                                                           \
  x[op->rd] = ipc + 4;                                                                                               \
  x[0] = 0;                                                                                                          \
  pc = v;                                                                                                            \
  END_CONTROL();
#define B_BRANCH(COND)                                                                                               \
  ipc = PC_OF(op);                                                                                                   \
  pc = (COND) ? ipc + op->imm : ipc + 4;                                                                             \
  END_CONTROL();
#define B_BEQ B_BRANCH(x[op->rs1] == x[op->rs2])
#define B_BNE B_BRANCH(x[op->rs1] != x[op->rs2])
#define B_BLT B_BRANCH((sxlen_t)x[op->rs1] < (sxlen_t)x[op->rs2])
#define B_BGE B_BRANCH((sxlen_t)x[op->rs1] >= (sxlen_t)x[op->rs2])
#define B_BLTU B_BRANCH(x[op->rs1] < x[op->rs2])
#define B_BGEU B_BRANCH(x[op->rs1] >= x[op->rs2])

// data address translation: inline TLB probe, va2pa() (page walk) on a miss; a fault leaves the instruction to RISCV()
#define FAST_XLATE(TYPE, TCTX)                                                                                       \
  if (DX) {                                                                                                          \
    const TlbEntry &te = tlb[(static_cast<uint32_t>(v >> 12) ^ ((TYPE) << 7)) & kTlbMask];                           \
    if (__builtin_expect(te.epoch == tlb_epoch && te.vpn == (v >> 12) && te.satp == satp && te.ctx == (TCTX), 1)) { \
      p = (te.frame << 12) | (v & 0xFFFu);                                                                           \
    } else if (!va2pa(p, v, (TYPE))) {                                                                               \
      BAIL();                                                                                                        \
    }                                                                                                                \
  }

// loads: misaligned, page-fault and non-DDR cases leave the instruction to RISCV()
#define B_LOAD(ALIGN_MASK, EXPR)                                                                                     \
  v = x[op->rs1] + op->imm;                                                                                          \
  if (v & (ALIGN_MASK)) BAIL();                                                                                      \
  if (!DX && XLEN == 64 && (v >> 32) != 0) BAIL();                                                                   \
  p = static_cast<uint32_t>(v);                                                                                      \
  FAST_XLATE(1u, tctx_ld)                                                                                            \
  if ((int32_t)p >= 0) BAIL();                                                                                       \
  {                                                                                                                  \
    const uint32_t w = mem[(p & 0x7FFFFFFFu) >> 2] >> ((p & 3u) * 8);                                                \
    x[op->rd] = (EXPR);                                                                                              \
  }                                                                                                                  \
  x[0] = 0;
#define B_LB B_LOAD(0u, (xlen_t)(sxlen_t)(int8_t)(w & 0xFF))
#define B_LBU B_LOAD(0u, w & 0xFF)
#define B_LH B_LOAD(1u, (xlen_t)(sxlen_t)(int16_t)(w & 0xFFFF))
#define B_LHU B_LOAD(1u, w & 0xFFFF)
#define B_LW B_LOAD(3u, (xlen_t)(sxlen_t)(int32_t)w)
#define B_LWU B_LOAD(3u, (xlen_t)w)
#define B_LD                                                                                                         \
  v = x[op->rs1] + op->imm;                                                                                          \
  if (v & 7u) BAIL();                                                                                                \
  if (!DX && (v >> 32) != 0) BAIL();                                                                                 \
  p = static_cast<uint32_t>(v);                                                                                      \
  FAST_XLATE(1u, tctx_ld)                                                                                            \
  if ((int32_t)p >= 0) BAIL();                                                                                       \
  x[op->rd] = static_cast<xlen_t>(mem[(p & 0x7FFFFFFFu) >> 2]) | (static_cast<xlen_t>(mem[((p & 0x7FFFFFFFu) >> 2) + 1]) << 32); \
  x[0] = 0;

// stores: same fallbacks; a store into a page that holds page tables or decoded code flushes the caches, after which
// the rest of the block is abandoned (the next instruction is fetched again)
#define B_STORE(ALIGN_MASK, SIZE_MASK)                                                                               \
  v = x[op->rs1] + op->imm;                                                                                          \
  if (v & (ALIGN_MASK)) BAIL();                                                                                      \
  if (!DX && XLEN == 64 && (v >> 32) != 0) BAIL();                                                                   \
  p = static_cast<uint32_t>(v);                                                                                      \
  FAST_XLATE(2u, tctx_st)                                                                                            \
  if ((int32_t)p >= 0) BAIL();                                                                                       \
  {                                                                                                                  \
    if (state.reserve_valid && state.reserve_addr == p) state.reserve_valid = false;                                 \
    const uint32_t off = p & 3u;                                                                                     \
    const uint32_t m = (SIZE_MASK) << (off * 8);                                                                     \
    const uint32_t wd = (x[op->rs2] & (SIZE_MASK)) << (off * 8);                                                     \
    uint32_t &word = mem[(p & 0x7FFFFFFFu) >> 2];                                                                    \
    if (__builtin_expect(trk, 0)) {                                                                                  \
      state.store_addr = p;                                                                                          \
      state.store_data = wd;                                                                                         \
      state.store_strb = ((SIZE_MASK) == 0xFFu ? 1u : ((SIZE_MASK) == 0xFFFFu ? 3u : 15u)) << (off * 8);              \
    }                                                                                                                \
    const uint8_t f = pgf[p >> 12];                                                                                  \
    if (__builtin_expect(f != 0, 0)) {                                                                               \
      page_flag_hit(f);                                                                                              \
      word = (m & wd) | (~m & word);                                                                                 \
      ex = (uint32_t)(op - b->ops) + 1;                                                                              \
      left += b->n - ex;                                                                                             \
      if (U) bbl += ex;                                                                                              \
      pc = bpc + (ex << 2);                                                                                          \
      ep = icache_epoch;                                                                                             \
      ctx = ctx_base | ep;                                                                                           \
      goto dispatch_block;                                                                                           \
    }                                                                                                                \
    word = (m & wd) | (~m & word);                                                                                   \
  }
#define B_SB B_STORE(0u, 0xFFu)
#define B_SH B_STORE(1u, 0xFFFFu)
#define B_SW B_STORE(3u, 0xFFFFFFFFu)
// sd: aligned, so both words are in the same page
#define B_SD                                                                                                         \
  v = x[op->rs1] + op->imm;                                                                                          \
  if (v & 7u) BAIL();                                                                                                \
  if (!DX && (v >> 32) != 0) BAIL();                                                                                 \
  p = static_cast<uint32_t>(v);                                                                                      \
  FAST_XLATE(2u, tctx_st)                                                                                            \
  if ((int32_t)p >= 0) BAIL();                                                                                       \
  {                                                                                                                  \
    if (state.reserve_valid && state.reserve_addr == p) state.reserve_valid = false;                                 \
    uint32_t &w0 = mem[(p & 0x7FFFFFFFu) >> 2];                                                                      \
    uint32_t &w1 = mem[((p & 0x7FFFFFFFu) >> 2) + 1];                                                                \
    const xlen_t sv = x[op->rs2];                                                                                    \
    if (__builtin_expect(trk, 0)) {                                                                                  \
      state.store_addr = p;                                                                                          \
      state.store_data = sv;                                                                                         \
      state.store_strb = 0xFFu;                                                                                      \
    }                                                                                                                \
    const uint8_t f = pgf[p >> 12];                                                                                  \
    w0 = static_cast<uint32_t>(sv);                                                                                  \
    w1 = static_cast<uint32_t>(sv >> 32);                                                                            \
    if (__builtin_expect(f != 0, 0)) {                                                                               \
      page_flag_hit(f);                                                                                              \
      ex = (uint32_t)(op - b->ops) + 1;                                                                              \
      left += b->n - ex;                                                                                             \
      if (U) bbl += ex;                                                                                              \
      pc = bpc + (ex << 2);                                                                                          \
      ep = icache_epoch;                                                                                             \
      ctx = ctx_base | ep;                                                                                           \
      goto dispatch_block;                                                                                           \
    }                                                                                                                \
  }

#define B_ADDI x[op->rd] = x[op->rs1] + op->imm;
#define B_SLTI x[op->rd] = (sxlen_t)x[op->rs1] < (sxlen_t)op->imm ? 1 : 0;
#define B_SLTIU x[op->rd] = x[op->rs1] < op->imm ? 1 : 0;
#define B_XORI x[op->rd] = x[op->rs1] ^ op->imm;
#define B_ORI x[op->rd] = x[op->rs1] | op->imm;
#define B_ANDI x[op->rd] = x[op->rs1] & op->imm;
#define B_SLLI x[op->rd] = x[op->rs1] << op->imm;
#define B_SRLI x[op->rd] = x[op->rs1] >> op->imm;
#define B_SRAI x[op->rd] = (xlen_t)((sxlen_t)x[op->rs1] >> op->imm);
#define B_ADD x[op->rd] = x[op->rs1] + x[op->rs2];
#define B_SUB x[op->rd] = x[op->rs1] - x[op->rs2];
#define B_SLL x[op->rd] = x[op->rs1] << (x[op->rs2] & kShamtMask);
#define B_SLT x[op->rd] = (sxlen_t)x[op->rs1] < (sxlen_t)x[op->rs2] ? 1 : 0;
#define B_SLTU x[op->rd] = x[op->rs1] < x[op->rs2] ? 1 : 0;
#define B_XOR x[op->rd] = x[op->rs1] ^ x[op->rs2];
#define B_SRL x[op->rd] = x[op->rs1] >> (x[op->rs2] & kShamtMask);
#define B_SRA x[op->rd] = (xlen_t)((sxlen_t)x[op->rs1] >> (x[op->rs2] & kShamtMask));
#define B_OR x[op->rd] = x[op->rs1] | x[op->rs2];
#define B_AND x[op->rd] = x[op->rs1] & x[op->rs2];
#define B_MUL x[op->rd] = x[op->rs1] * x[op->rs2];
#define B_MULH x[op->rd] = mul_hi_ss(x[op->rs1], x[op->rs2]);
#define B_MULHSU x[op->rd] = mul_hi_su(x[op->rs1], x[op->rs2]);
#define B_MULHU x[op->rd] = mul_hi_uu(x[op->rs1], x[op->rs2]);
#define B_DIV x[op->rd] = xlen_div((sxlen_t)x[op->rs1], (sxlen_t)x[op->rs2]);
#define B_DIVU x[op->rd] = xlen_divu(x[op->rs1], x[op->rs2]);
#define B_REM x[op->rd] = xlen_rem((sxlen_t)x[op->rs1], (sxlen_t)x[op->rs2]);
#define B_REMU x[op->rd] = xlen_remu(x[op->rs1], x[op->rs2]);
#if XLEN == 64
#define B_ADDIW x[op->rd] = sext32((uint32_t)(x[op->rs1] + op->imm));
#define B_SLLIW x[op->rd] = sext32((uint32_t)x[op->rs1] << op->imm);
#define B_SRLIW x[op->rd] = sext32((uint32_t)x[op->rs1] >> op->imm);
#define B_SRAIW x[op->rd] = sext32((uint32_t)((int32_t)(uint32_t)x[op->rs1] >> op->imm));
#define B_ADDW x[op->rd] = sext32((uint32_t)(x[op->rs1] + x[op->rs2]));
#define B_SUBW x[op->rd] = sext32((uint32_t)(x[op->rs1] - x[op->rs2]));
#define B_SLLW x[op->rd] = sext32((uint32_t)x[op->rs1] << (x[op->rs2] & 31));
#define B_SRLW x[op->rd] = sext32((uint32_t)x[op->rs1] >> (x[op->rs2] & 31));
#define B_SRAW x[op->rd] = sext32((uint32_t)((int32_t)(uint32_t)x[op->rs1] >> (x[op->rs2] & 31)));
#define B_MULW x[op->rd] = sext32((uint32_t)x[op->rs1] * (uint32_t)x[op->rs2]);
#define B_DIVW x[op->rd] = w_div((int32_t)(uint32_t)x[op->rs1], (int32_t)(uint32_t)x[op->rs2]);
#define B_DIVUW x[op->rd] = w_divu((uint32_t)x[op->rs1], (uint32_t)x[op->rs2]);
#define B_REMW x[op->rd] = w_rem((int32_t)(uint32_t)x[op->rs1], (int32_t)(uint32_t)x[op->rs2]);
#define B_REMUW x[op->rd] = w_remu((uint32_t)x[op->rs1], (uint32_t)x[op->rs2]);
#endif

// CSR instruction: run the general implementation for it (same code RISCV() would call), then continue in the fast path
// only if nothing the fast path assumes has changed.
L_CSR: {
  ipc = PC_OF(op);
  state.pc = ipc;
  Instruction = op->imm;
  sim_time = st0 + (budget0 - left) - 1;  // the instructions completed before this one
  is_csr = true;
  is_exception = is_br = br_taken = false;
  illegal_exception = page_fault_load = page_fault_inst = page_fault_store = false;
  breakpoint_exception = false;
  asy = false;
  state.store = false;
  is_io = false;
  force_sync = false;
  sync_timer_csrs(state, sim_time, device_effects_enable && interrupt_delivery_enable);
  RV32CSR();
  state.gpr[0] = 0;
  sim_time = st0 + (budget0 - left);
  if (U) bbl += b->n;
  {
    const xlen_t mst = state.csr[csr_mstatus];
    bool ok = !is_exception && !illegal_exception && state.pc == ipc + 4 && privilege == priv &&
              state.csr[csr_satp] == satp && state.csr[csr_mie] == mie0 && state.csr[csr_mip] == mip0 &&
              ((mst & kMstatusDataMask) == (mst0 & kMstatusDataMask));
    if (ok && irq_possible) {
      // an interrupt that is pending and enabled may have become deliverable (mstatus.SIE/MIE, ...)
      evaluate_interrupts(mip_eff);
      ok = !(M_software_interrupt || M_timer_interrupt || M_external_interrupt || S_software_interrupt ||
             S_timer_interrupt || S_external_interrupt);
    }
    if (!ok) {
      pc = state.pc;
      goto out;
    }
  }
  pc = state.pc;
  goto dispatch_block;
}


// ---- one handler per instruction ----
#define H(NAME) L_##NAME: B_##NAME NEXT();
#define HC(NAME) L_##NAME: B_##NAME
H(LUI) H(AUIPC) HC(JAL) HC(JALR) HC(BEQ) HC(BNE) HC(BLT) HC(BGE) HC(BLTU) HC(BGEU)
H(LB) H(LH) H(LW) H(LBU) H(LHU) H(SB) H(SH) H(SW)
#if XLEN == 64
H(LWU) H(LD) H(SD)
H(ADDIW) H(SLLIW) H(SRLIW) H(SRAIW) H(ADDW) H(SUBW) H(SLLW) H(SRLW) H(SRAW) H(MULW) H(DIVW) H(DIVUW) H(REMW) H(REMUW)
#else
L_LWU: L_LD: L_SD: L_ADDIW: L_SLLIW: L_SRLIW: L_SRAIW: L_ADDW: L_SUBW: L_SLLW: L_SRLW: L_SRAW: L_MULW: L_DIVW: L_DIVUW:
L_REMW: L_REMUW: goto out;   // never produced by the rv32 decode
#endif
H(ADDI) H(SLTI) H(SLTIU) H(XORI) H(ORI) H(ANDI) H(SLLI) H(SRLI) H(SRAI)
H(ADD) H(SUB) H(SLL) H(SLT) H(SLTU) H(XOR) H(SRL) H(SRA) H(OR) H(AND)
H(MUL) H(MULH) H(MULHSU) H(MULHU) H(DIV) H(DIVU) H(REM) H(REMU)
#undef H
#undef HC

// ---- fused pairs: first body, move to the second slot, second body ----
#define FH_NC(a, b) L_F_##a##_##b: B_##a ++op; B_##b NEXT();
#define FH_C(a, b) L_F_##a##_##b: B_##a ++op; B_##b
FUSE_LIST_NC(FH_NC)
FUSE_LIST_C(FH_C)
#undef FH_NC
#undef FH_C

#undef NEXT
#undef PC_OF
#undef BAIL
#undef END_CONTROL

out:
  state.pc = pc;
  {
    const uint64_t done = budget0 - left;
    sim_time = st0 + done;
    if (U) {
      current_bb_len = static_cast<uint32_t>(bbl);
    }
    if (g_fast_stats) {
      g_fs.fast_insts += done;
    }
    if (__builtin_expect(g_digest_every != 0, 0) && done > 0 && done != last_digest &&
        sim_time % g_digest_every == 0) {
      print_state_digest(sim_time, state, privilege);
    }
    if (__builtin_expect(g_digest_block != 0, 0) && done > 0 && sim_time >= g_next_digest) {
      print_state_digest(sim_time, state, privilege);
      g_next_digest = sim_time + g_digest_block;
    }
    if (done > 0) {
      // leave the per-instruction flags as RISCV() would for a plain instruction
      is_csr = is_exception = false;
      is_br = br_taken = false;
      is_io = false;
      force_sync = false;
      asy = false;
      illegal_exception = page_fault_load = page_fault_inst = page_fault_store = false;
      breakpoint_exception = false;
      state.store = false;
      sync_timer_csrs(state, sim_time, device_effects_enable && interrupt_delivery_enable);
    }
  }
}

void Ref_cpu::fast_run(uint32_t &finished_intervals, uint64_t cap, bool stop_iv, bool bbv, bool trk) {
  if (sim_time >= MAX_SIM_TIME) {
    return;
  }
  const uint32_t mie = state.csr[csr_mie];
  if (g_fast_stats) g_fs.runs++;
  // RISCV() takes no interrupt and no trap before executing an ordinary instruction exactly when none of the six
  // interrupt flags of evaluate_interrupts() is set.  Pending-and-enabled interrupts that are not deliverable (for
  // example the supervisor timer while sstatus.SIE is 0) do not stop the fast path.  The machine timer is the only
  // source that changes by itself: when it is enabled and not yet due, run at most until the deadline.
  uint32_t mip_eff = 0;
  if (interrupt_delivery_enable) {
    mip_eff = ref_effective_mip(state.csr[csr_mip], sim_time, device_effects_enable);
  }
  evaluate_interrupts(mip_eff);
  if (M_software_interrupt || M_timer_interrupt || M_external_interrupt || S_software_interrupt || S_timer_interrupt ||
      S_external_interrupt) {
    if (g_fast_stats) g_fs.stop[0]++;
    return;
  }
  uint64_t budget = MAX_SIM_TIME - sim_time;
  if (device_effects_enable && interrupt_delivery_enable && (mie & MIP_MTIP)) {
    const uint64_t now = sim_time + g_ref_timer_offset;
    if (now < g_ref_timer_cmp) {
      budget = std::min<uint64_t>(budget, g_ref_timer_cmp - now);
    }
  }
  budget = std::min<uint64_t>(budget, cap);
  if (budget == 0) {
    return;
  }
  if (fblocks.empty()) {
    fblocks.resize(kFastBlocks);
  }
  const uint32_t mstatus = state.csr[csr_mstatus];
  const uint32_t data_priv = (mstatus & MSTATUS_MPRV) ? GET_MPP(mstatus) : privilege;
  const bool dx = satp_enabled(state.csr[csr_satp]) && data_priv != RISCV_MODE_M;
  const bool u = privilege == RISCV_MODE_U;
  if (u) {
    if (dx) fast_run_impl<true, true>(finished_intervals, budget, mip_eff, stop_iv, bbv, trk);
    else fast_run_impl<true, false>(finished_intervals, budget, mip_eff, stop_iv, bbv, trk);
  } else {
    if (dx) fast_run_impl<false, true>(finished_intervals, budget, mip_eff, stop_iv, bbv, trk);
    else fast_run_impl<false, false>(finished_intervals, budget, mip_eff, stop_iv, bbv, trk);
  }
}

void Ref_cpu::exec(const SimConfig &config) {
  // Initialize Spike Reference Simulator if enabled
#ifdef ENABLE_SPIKE
  if (config.difftest) {
    spike_ref = std::make_unique<SpikeRef>(XLEN == 64 ? (FP ? "rv64imafdb_zbc_zbkb_zbkx" : "rv64imab_zbc_zbkb_zbkx") : (FP ? "rv32imafdb_zbc_zbkb_zbkx" : "rv32imab_zbc_zbkb_zbkx"), 0x80000000, 0x80000000,
                                           config.image_file.c_str());
  }
#else
  if (config.difftest) {
    std::cerr << "[RefCPU] Difftest requested, but this build is compiled "
                 "without Spike support."
              << std::endl;
    exit(1);
  }
#endif

  // 准备 GEN_CHECKPOINT 需要的 target_intervals
  std::map<uint32_t, uint32_t> target_intervals;
  std::map<uint64_t, std::string> special_targets;
  uint32_t finished_intervals = 0;

  auto save_special_checkpoint = [&](const std::string &ckpt_name) {
    // The legacy interval field is a SimPoint warmup length.  It is unrelated
    // to an absolute special-checkpoint boundary; serializing the live U-mode
    // BBV counter here would make simulator CKPT mode prewarm past the saved
    // CPU state before starting the DUT.
    const uint64_t live_interval_inst_count = interval_inst_count;
    interval_inst_count = 0;
    save_checkpoint(ckpt_name);
    interval_inst_count = live_interval_inst_count;
  };

  if (config.mode == SimMode::GEN_CHECKPOINT) {
    target_intervals = load_simpoints(config.points_file);

    // [特殊处理] Interval 0 无法向前预热，只能直接保存
    // 如果 SimPoint 选中了 0，我们只能保存刚启动的状态
    if (target_intervals.count(0)) {
      uint32_t sp_id = target_intervals[0];
      std::string ckpt_name = config.checkpoint_dir + "/ckpt_sp" +
                              std::to_string(sp_id) +
                              "_target0_nowarmup.gz"; // 建议加上.gz后缀
      save_checkpoint(ckpt_name);
    }
  } else if (config.mode == SimMode::GEN_SPECIAL_CHECKPOINT) {
    special_targets = load_special_checkpoint_targets(config.points_file);
    if (special_targets.empty()) {
      std::cerr << "Error: no special checkpoint targets were loaded."
                << std::endl;
      exit(1);
    }
    auto zero = special_targets.find(0);
    if (zero != special_targets.end()) {
      const std::string ckpt_name =
          config.checkpoint_dir + "/ckpt_special_" +
          checkpoint_label(zero->second) + "_inst0.gz";
      save_special_checkpoint(ckpt_name);
      special_targets.erase(zero);
      if (special_targets.empty())
        sim_end = true;
    }
  } else if (config.mode == SimMode::GEN_BBV) {
    bbv_init_file(config.bbv_output_file.c_str());
  } else if (config.mode == SimMode::RESTORE) {
    restore_checkpoint(config.restore_file, config.restore_mem_only);
  }

  uint64_t restored_inst_count = 0;
  // Front-door restore: the restore program (flash image) runs from reset; its first mret hands over to the checkpoint's
  // code. The instruction limit and all counters start counting there.
  bool front_door_pending = config.mode == SimMode::RESTORE && config.restore_mem_only;
  uint64_t front_door_insts = 0;

  // Interval boundary (an interval of U-mode instructions just ended): mode-specific dump / checkpoint, then restart the count
  auto interval_boundary = [&]() {
    // 当前 Interval (finished_intervals) 刚刚结束
    finished_intervals++;

    // 即将开始的 Interval 编号
    // 例如：finished_intervals 变成 1823，说明我们要开始跑 Interval 1823 了
    uint32_t upcoming_interval_id = finished_intervals;

    // --- 模式分支处理 ---

    if (config.mode == SimMode::GEN_BBV) {
      std::cout << "Dump BBV at interval boundary: " << upcoming_interval_id
                << std::endl;
      dump_bbv();
    } else if (config.mode == SimMode::GEN_CHECKPOINT) {
      // [Warmup 逻辑核心修改]
      // 我们不检查 upcoming_interval_id 是否是目标，
      // 而是检查 (upcoming_interval_id + 1) 是否是目标。
      // 如果是，说明当前这个 Interval (upcoming_interval_id) 就是那个 Warmup
      // 区间。 我们需要在进入 Warmup 区间之前保存 Checkpoint。

      uint32_t target_id = upcoming_interval_id + 1; // Lookahead (向前看一个)

      auto it = target_intervals.find(target_id);
      if (it != target_intervals.end()) {
        uint32_t sp_id = it->second;

        // 文件名命名建议：明确标出这是 target 谁的 warmup
        std::string ckpt_name = config.checkpoint_dir + "/ckpt_sp" +
                                std::to_string(sp_id) + "_target" +
                                std::to_string(target_id) +
                                "_warmup1.gz"; // warmup1 代表预热长度为1

        std::cout << "Creating Warmup Checkpoint for Target " << target_id
                  << std::endl;
        save_checkpoint(ckpt_name);

        // 优化：如果最大的目标都已经生成过预热快照了，就可以退出了
        // 注意：target_intervals 是有序 map，rbegin() 是最大的 key
        if (!target_intervals.empty() &&
            target_id >= target_intervals.rbegin()->first) {
          std::cout << "All checkpoints generated. Simulation finished."
                    << std::endl;
          sim_end = true;
        }
      }
    }

    interval_inst_count = 0;
  };

  // --- 主循环 ---
  // Plain run (no difftest, no instruction limit, no checkpoint/BBV mode): the generic loop below reduces to exactly
  // this, so skip its per-instruction mode and limit checks.
  if (config.mode == SimMode::NORMAL && !config.difftest && config.max_insts == 0 && !kLogProgressSimTime) {
    while (!sim_end && sim_time < MAX_SIM_TIME) {
      if (!g_no_fast) {
        fast_run(finished_intervals);
        if (sim_end || sim_time >= MAX_SIM_TIME)
          break;
      }
      RISCV();
      if (sim_end)
        break;
      sim_time++;
      if (privilege == RISCV_MODE_U && is_br) {
        interval_inst_count += current_bb_len;
        current_bb_len = 0;
      }
      if (interval_inst_count >= INTERVAL_SIZE) {
        finished_intervals++;
        interval_inst_count = 0;
      }
      if (__builtin_expect(g_digest_every != 0, 0) && sim_time % g_digest_every == 0)
        print_state_digest(sim_time, state, privilege);
      if (__builtin_expect(g_digest_block != 0, 0) && sim_time >= g_next_digest && !g_no_fast) {
        print_state_digest(sim_time, state, privilege);
        g_next_digest = sim_time + g_digest_block;
      }
      if (__builtin_expect(g_digest_list_pos < g_digest_list.size(), 0) && sim_time == g_digest_list[g_digest_list_pos]) {
        print_state_digest(sim_time, state, privilege);
        g_digest_list_pos++;
      }
    }
  }
  while (!(config.mode == SimMode::NORMAL && !config.difftest && config.max_insts == 0 && !kLogProgressSimTime) &&
         !sim_end && sim_time < MAX_SIM_TIME) {

    if (kLogProgressSimTime && sim_time % 100000000 == 0) {
      std::cout << "SimTime: " << sim_time
                << " | Intervals: " << finished_intervals << std::endl;
    }

    // 0. Fast path (also for BBV / checkpoint / limited runs): run as many instructions as possible without events.
    // The caps keep the last instruction before a special-checkpoint target or the instruction limit for RISCV(), so
    // the machine state at those points is exactly what the general path leaves.
    if (!g_no_fast && !config.difftest) {
      uint64_t cap = UINT64_MAX;
      if (config.mode == SimMode::GEN_SPECIAL_CHECKPOINT) {
        const auto it = special_targets.upper_bound(sim_time);
        if (it != special_targets.end()) {
          cap = it->first - sim_time - 1;
        }
      }
      if (config.max_insts > 0 && !front_door_pending) {
        const uint64_t cur = (config.mode == SimMode::RESTORE) ? restored_inst_count : sim_time;
        cap = std::min<uint64_t>(cap, cur + 1 >= config.max_insts ? 0 : config.max_insts - cur - 1);
      }
      const uint64_t t0 = sim_time;
      fast_run(finished_intervals, cap, /*stop_iv=*/true, config.mode == SimMode::GEN_BBV,
               config.mode == SimMode::GEN_CHECKPOINT || config.mode == SimMode::GEN_SPECIAL_CHECKPOINT);
      const uint64_t done = sim_time - t0;
      if (config.mode == SimMode::RESTORE) {
        restored_inst_count += done;
      }
      if (sim_end || sim_time >= MAX_SIM_TIME) {
        break;
      }
      if (done > 0 && interval_inst_count >= INTERVAL_SIZE) {
        interval_boundary();  // a U-mode branch completed the interval; no instruction left to run before it
        continue;
      }
    }

    // 1. 执行指令
    RISCV();

    // If the simulation ended (e.g., ebreak), skip Difftest for this last step
    // because DUT and Spike handle termination differently.
    if (sim_end)
      break;

    // Difftest Step
    if (config.difftest
#ifdef ENABLE_SPIKE
        && spike_ref
#endif
    ) {
#ifdef ENABLE_SPIKE
      if (!difftest_started) {
        // DUT starts at 0x0 with some boot code.
        // Spike is configured to start at 0x80000000.
        // We wait until DUT reaches 0x80000000 to sync state and start
        // difftest.
        if (state.pc == 0x80000000) {
          spike_ref->sync_all_memory_from_dut(memory, ram_size, sdram_memory,
                                              SDRAM_SIZE);
          spike_ref->sync_state(state, privilege);
          difftest_started = true;
        }
      } else {
        processor_t *ref_core = spike_ref->sim->get_core(0);
        if (is_io || force_sync) {
          // Device/MMIO instructions and interrupt/CSR transitions are owned
          // by the DUT model.  Do not let Spike execute them independently;
          // synchronize the post-commit architectural state instead.
          spike_ref->sync_state(state, privilege);
        } else {
          spike_ref->suppress_async_interrupts();
          spike_ref->step(1);
          if (Instruction == INST_WFI) {
            // The DUT and RefCPU implement WFI as a retiring NOP.  Spike may
            // enter its internal WFI wait state after retiring the instruction;
            // clear that non-architectural state without synchronizing CPU
            // state so the next instruction can be checked normally.
            ref_core->clear_waiting_for_interrupt();
          }
          // Strict check: No automatic recovery. Mismatch = Failure.
          spike_ref->reg_check(state, privilege);
        }
      }
#endif
    }

    sim_time++;
    if (__builtin_expect(g_digest_every != 0, 0) && sim_time % g_digest_every == 0)
      print_state_digest(sim_time, state, privilege);
    if (__builtin_expect(g_digest_block != 0, 0) && sim_time >= g_next_digest && !g_no_fast) {
      print_state_digest(sim_time, state, privilege);
      g_next_digest = sim_time + g_digest_block;
    }
    if (__builtin_expect(g_digest_list_pos < g_digest_list.size(), 0) && sim_time == g_digest_list[g_digest_list_pos]) {
      print_state_digest(sim_time, state, privilege);
      g_digest_list_pos++;
    }

    // Special checkpoints use the absolute functional-model instruction count,
    // independent of U-mode SimPoint interval accounting.  At sim_time == N,
    // exactly N instructions have completed, so the next instruction is a
    // deterministic boundary shared by generation and restore.
    if (config.mode == SimMode::GEN_SPECIAL_CHECKPOINT) {
      auto target = special_targets.find(sim_time);
      if (target != special_targets.end()) {
        const std::string ckpt_name =
            config.checkpoint_dir + "/ckpt_special_" +
            checkpoint_label(target->second) + "_inst" +
            std::to_string(target->first) + ".gz";
        std::cout << "Creating special checkpoint at absolute instruction "
                  << target->first << " (" << target->second << ")"
                  << std::endl;
        save_special_checkpoint(ckpt_name);
        special_targets.erase(target);
        if (special_targets.empty()) {
          std::cout << "All special checkpoints generated. Simulation "
                       "finished."
                    << std::endl;
          sim_end = true;
        }
      }
    }

    // 2. 计数逻辑 (保持和你生成 BBV 时一致，这对 SimPoint 对齐至关重要)
    if (privilege == RISCV_MODE_U && is_br) {
      if (config.mode == SimMode::GEN_BBV) {
        bbv_commit(); // 更新内存中的 bbv_counts
      }
      // 这里的 interval_inst_count 决定了 Interval 的边界
      interval_inst_count += current_bb_len;
      current_bb_len = 0;
    }

    // 3. 指令数限制检查
    if (config.max_insts > 0 && !front_door_pending) {
      if (config.mode == SimMode::RESTORE) {
        restored_inst_count++;
        if (restored_inst_count >= config.max_insts) {
          std::cout << "Restore run finished (max_insts reached)." << std::endl;
          break;
        }
      } else if (sim_time >= config.max_insts) {
        std::cout << "Run finished (max_insts reached)." << std::endl;
        break;
      }
    }

    // 4. Interval 边界处理
    if (interval_inst_count >= INTERVAL_SIZE) {
      interval_boundary();
    }

    // 5. Front-door restore: the restore program ended with its mret. Start counting from the checkpoint's code; the
    // timer offset absorbs the reset of sim_time so mtime stays continuous.
    if (__builtin_expect(front_door_pending, 0) && Instruction == INST_MRET) {
      front_door_pending = false;
      front_door_insts = sim_time;
      std::cout << "[front-door] restore program done after " << std::dec << front_door_insts
                << " instructions; pc=0x" << std::hex << state.pc << " priv=" << std::dec << (unsigned)privilege
                << " mtime=" << ref_timer_now(sim_time) << " mtimecmp=" << g_ref_timer_cmp << std::endl;
      g_ref_timer_offset += sim_time;
      sim_time = 0;
      restored_inst_count = 0;
      interval_inst_count = 0;
      current_bb_len = 0;
      finished_intervals = 0;
    }
  }

  if (sim_time >= MAX_SIM_TIME) {
    std::cerr << "Error: Simulation Timeout! (MAX_SIM_TIME = " << MAX_SIM_TIME
              << " reached)" << std::endl;
    exit(1);
  }

  // --- 结束清理 ---
  if (config.mode == SimMode::GEN_BBV) {
    if (bbv_file.is_open())
      bbv_file.close();
  }
}

void Ref_cpu::exception(xlen_t trap_val) {
  is_exception = true;
  xlen_t next_pc = state.pc + 4;

  // 重新获取当前状态（因为exec可能没传进来最新的）
  bool ecall = (Instruction == INST_ECALL);
  bool mret = (Instruction == INST_MRET);
  bool sret = (Instruction == INST_SRET);

  xlen_t mstatus = state.csr[csr_mstatus];
  xlen_t sstatus = state.csr[csr_sstatus];
  xlen_t medeleg = state.csr[csr_medeleg];
  xlen_t mtvec = state.csr[csr_mtvec];
  xlen_t stvec = state.csr[csr_stvec];

  // 再次计算 Trap 原因 (与 RISCV() 中逻辑一致，但这里是为了确定是用 MTrap 还是
  // STrap 处理)
  // 注意：为了代码复用，这里其实可以简化，但为了保持你原有逻辑结构：

  bool medeleg_U_ecall = (medeleg >> 8) & 1;
  bool medeleg_S_ecall = (medeleg >> 9) & 1;
  bool medeleg_page_fault_inst = (medeleg >> 12) & 1;
  bool medeleg_page_fault_load = (medeleg >> 13) & 1;
  bool medeleg_page_fault_store = (medeleg >> 15) & 1;
  bool medeleg_illegal_inst = (medeleg >> 2) & 1;
  bool medeleg_breakpoint = ((medeleg >> 3) & 1) && privilege != 3;  // a trap taken in M mode is never delegated

  // 这里直接复用成员变量里的中断状态 (假设RISCV函数刚跑完，状态是新的)
  // 如果不是，需要重新计算 M_software_interrupt 等

  bool MTrap =
      (M_software_interrupt) || (M_timer_interrupt) || (M_external_interrupt) ||
      ((privilege == 0) && !medeleg_U_ecall && ecall) ||
      (ecall && (privilege == 1) && !medeleg_S_ecall) ||
      (ecall && (privilege == 3)) ||
      (page_fault_inst && !medeleg_page_fault_inst) ||
      (page_fault_load && !medeleg_page_fault_load) ||
      (page_fault_store && !medeleg_page_fault_store) ||
      (illegal_exception && !medeleg_illegal_inst) ||
      (breakpoint_exception && !medeleg_breakpoint);

  bool STrap = S_software_interrupt || S_timer_interrupt ||
               S_external_interrupt ||
               (ecall && (privilege == 0) && medeleg_U_ecall) ||
               (ecall && (privilege == 1) && medeleg_S_ecall) ||
               (page_fault_inst && medeleg_page_fault_inst) ||
               (page_fault_load && medeleg_page_fault_load) ||
               (page_fault_store && medeleg_page_fault_store) ||
               (illegal_exception && medeleg_illegal_inst) ||
               (breakpoint_exception && medeleg_breakpoint);

  if (M_software_interrupt || M_timer_interrupt || M_external_interrupt ||
      S_software_interrupt || S_timer_interrupt || S_external_interrupt) {
    force_sync = true;
  }

  if (kLogUserSyscallSummary && privilege == RISCV_MODE_U && ecall) {
    const uint32_t sysno = state.gpr[17];
    if (sysno == 29 || sysno == 56 || sysno == 57 || sysno == 63 ||
        sysno == 64 || sysno == 66 || sysno == 221 || sysno == 222) {
      std::cerr << "[RefCPU][U-SYSCALL] nr=" << sysno << " pc=0x" << std::hex
                << state.pc << " a0=0x" << state.gpr[10] << " a1=0x"
                << state.gpr[11] << " a2=0x" << state.gpr[12] << " a3=0x"
                << state.gpr[13] << std::dec << std::endl;
    }
  }
  if (std::getenv("REF_TRAP_TRACE")) {
    static long seen = 0; static int shown = 0;
    static const long every = std::getenv("REF_TRAP_EVERY") ? std::atol(std::getenv("REF_TRAP_EVERY")) : 1;
    if (seen++ % every == 0 && shown++ < 60)
      fprintf(stderr, "[trap] t=%llu pc=0x%08x priv=%u M=%d S=%d mip=0x%x mie=0x%x mideleg=0x%x cmp=0x%llx now=0x%llx\n",
              (unsigned long long)sim_time, state.pc, (unsigned)privilege, (int)MTrap, (int)STrap, visible_mip(),
              state.csr[csr_mie], state.csr[csr_mideleg], (unsigned long long)g_ref_timer_cmp,
              (unsigned long long)ref_timer_now(sim_time));
  }
  if (MTrap) {
    state.csr[csr_mepc] = state.pc;
    xlen_t cause = 0;

    // 计算 MCause
    bool is_interrupt =
        M_software_interrupt || M_timer_interrupt || M_external_interrupt;
    if (is_interrupt)
      cause |= kXlenMsb;

    uint32_t exception_code = 0;
    if (M_software_interrupt)
      exception_code = 3;
    else if (M_timer_interrupt)
      exception_code = 7;
    else if (M_external_interrupt ||
             (ecall && privilege == 3 && !medeleg_U_ecall))
      exception_code = 11;
    else if (ecall && privilege == 0 && !medeleg_U_ecall)
      exception_code = 8;
    else if (ecall && privilege == 1 && !medeleg_S_ecall)
      exception_code = 9;
    else if (page_fault_inst && !medeleg_page_fault_inst)
      exception_code = 12;
    else if (page_fault_load && !medeleg_page_fault_load)
      exception_code = 13;
    else if (page_fault_store && !medeleg_page_fault_store)
      exception_code = 15;
    else if (breakpoint_exception)
      exception_code = 3;
    else if (illegal_exception)
      exception_code = 2;

    cause |= exception_code;
    g_dbg_trap_count[(exception_code & 15) | (is_interrupt ? 16 : 0)]++;
    state.csr[csr_mcause] = cause;
    if (kLogVerboseTimer && exception_code == 7) {
      std::cerr << "[RefCPU][TRAP] M-timer pc=0x" << std::hex << state.pc
                << " mtvec=0x" << mtvec << " mip=0x" << visible_mip()
                << " mie=0x" << state.csr[csr_mie]
                << " cmp=0x" << g_ref_timer_cmp << " now=0x"
                << ref_timer_now(sim_time) << std::dec
                << std::endl;
    }

    // 向量中断跳转
    if ((mtvec & 1) && (cause & kXlenMsb)) {
      next_pc = (mtvec & ~xlen_t(3)) + 4 * (cause & ~kXlenMsb);
    } else {
      next_pc =
          mtvec & ~xlen_t(3); // 这里的MASK可能需要根据Spec确认，通常是清除低2位
    }

    // 更新 mstatus
    // MPP = privilege
    mstatus = (mstatus & ~MSTATUS_MPP) | ((privilege & 0x3) << 11);
    // MPIE = MIE
    if (mstatus & MSTATUS_MIE)
      mstatus |= MSTATUS_MPIE;
    else
      mstatus &= ~MSTATUS_MPIE;
    // MIE = 0
    mstatus &= ~MSTATUS_MIE;

    // 同步 sstatus (sstatus 是 mstatus 的影子)
    state.csr[csr_mstatus] = mstatus;
    state.csr[csr_sstatus] =
        mstatus & kSstatusMask; // sstatus is the supervisor-visible view of mstatus

    privilege = 3; // Machine Mode
    state.csr[csr_mtval] = trap_val;

  } else if (STrap) {
    state.csr[csr_sepc] = state.pc;
    xlen_t cause = 0;

    bool is_interrupt =
        S_software_interrupt || S_timer_interrupt || S_external_interrupt;
    if (is_interrupt)
      cause |= kXlenMsb;

    uint32_t exception_code = 0;
    if (S_external_interrupt || (ecall && privilege == 1 && medeleg_S_ecall))
      exception_code = 9;
    else if (S_timer_interrupt)
      exception_code = 5;
    else if (ecall && privilege == 0 && medeleg_U_ecall)
      exception_code = 8;
    else if (S_software_interrupt)
      exception_code = 1;
    else if (page_fault_inst && medeleg_page_fault_inst)
      exception_code = 12;
    else if (page_fault_load && medeleg_page_fault_load)
      exception_code = 13;
    else if (page_fault_store && medeleg_page_fault_store)
      exception_code = 15;
    else if (breakpoint_exception && medeleg_breakpoint)
      exception_code = 3;
    else if (illegal_exception && medeleg_illegal_inst)
      exception_code = 2;

    cause |= exception_code;
    state.csr[csr_scause] = cause;
    if (kLogUserTrapSummary && privilege == RISCV_MODE_U &&
        !S_software_interrupt && !S_timer_interrupt && !S_external_interrupt) {
      std::cerr << "[RefCPU][U-TRAP->S] pc=0x" << std::hex << state.pc
                << " cause=0x" << cause << " tval=0x" << trap_val
                << " sepc=0x" << state.csr[csr_sepc]
                << " satp=0x" << state.csr[csr_satp] << std::dec << std::endl;
    }
    if (kLogVerboseTimer && exception_code == 5) {
      std::cerr << "[RefCPU][TRAP] S-timer pc=0x" << std::hex << state.pc
                << " stvec=0x" << stvec << " mip=0x" << visible_mip()
                << " mie=0x" << state.csr[csr_mie]
                << " cmp=0x" << g_ref_timer_cmp << " now=0x"
                << ref_timer_now(sim_time) << std::dec
                << std::endl;
    }

    if ((stvec & 1) && (cause & kXlenMsb)) {
      next_pc = (stvec & ~xlen_t(3)) + 4 * (cause & ~kXlenMsb);
    } else {
      next_pc = stvec & ~xlen_t(3);
    }

    // 更新 sstatus
    // SPP = privilege
    if (privilege == 1)
      sstatus |= MSTATUS_SPP;
    else
      sstatus &= ~MSTATUS_SPP;

    // SPIE = SIE
    if (sstatus & MSTATUS_SIE)
      sstatus |= MSTATUS_SPIE;
    else
      sstatus &= ~MSTATUS_SPIE;

    // SIE = 0
    sstatus &= ~MSTATUS_SIE;

    // 写回
    state.csr[csr_sstatus] = sstatus;
    // 更新 mstatus 中对应的位 (SPIE, SIE, SPP 在 mstatus 中也有对应位置)
    // 简单做法：读出改完的 sstatus，把对应位刷回 mstatus
    const xlen_t mask = kSstatusMask;
    state.csr[csr_mstatus] =
        (state.csr[csr_mstatus] & ~mask) | (sstatus & mask);

    privilege = 1; // Supervisor Mode
    state.csr[csr_stval] = trap_val;

  } else if (mret) {
    // MIE = MPIE
    if (mstatus & MSTATUS_MPIE)
      mstatus |= MSTATUS_MIE;
    else
      mstatus &= ~MSTATUS_MIE;

    // Privilege = MPP
    privilege = GET_MPP(mstatus);

    // MPIE = 1
    mstatus |= MSTATUS_MPIE;
    // MPP = U (0)
    mstatus &= ~MSTATUS_MPP;

    state.csr[csr_mstatus] = mstatus;
    // 同步 sstatus
    state.csr[csr_sstatus] = mstatus & kSstatusMask;

    next_pc = state.csr[csr_mepc];
    if (kLogVerboseReturns &&
        (next_pc == 0xc033fc6c || next_pc == 0xc033fc70 ||
         next_pc == 0xc033fc74 || next_pc == 0xc0004f5c)) {
      std::cerr << "[RefCPU][RET] mret -> pc=0x" << std::hex << next_pc
                << " priv=" << std::dec << privilege << " mip=0x" << std::hex
                << visible_mip() << " mie=0x" << state.csr[csr_mie]
                << " mstatus=0x" << state.csr[csr_mstatus] << std::dec
                << std::endl;
    }

  } else if (sret) {
    // SIE = SPIE
    if (sstatus & MSTATUS_SPIE)
      sstatus |= MSTATUS_SIE;
    else
      sstatus &= ~MSTATUS_SIE;

    // Privilege = SPP
    privilege = GET_SPP(sstatus);

    // SPIE = 1
    sstatus |= MSTATUS_SPIE;
    // SPP = U (0)
    sstatus &= ~MSTATUS_SPP;

    state.csr[csr_sstatus] = sstatus;
    // 同步回 mstatus
    const xlen_t mask = kSstatusMask;
    state.csr[csr_mstatus] =
        (state.csr[csr_mstatus] & ~mask) | (sstatus & mask);

    next_pc = state.csr[csr_sepc];
    if (kLogVerboseReturns) {
      std::cerr << "[RefCPU][RET] sret -> pc=0x" << std::hex << next_pc
                << " priv=" << std::dec << privilege << " mip=0x" << std::hex
                << visible_mip() << " mie=0x" << state.csr[csr_mie]
                << " sstatus=0x" << state.csr[csr_sstatus] << std::dec
                << std::endl;
    }
  }

  state.pc = next_pc;
}

void Ref_cpu::evaluate_interrupts(uint32_t mip_reg) {
  const uint32_t mstatus = state.csr[csr_mstatus];
  const uint32_t mie_reg = state.csr[csr_mie];
  const uint32_t mideleg = state.csr[csr_mideleg];
  const bool mstatus_mie = (mstatus & MSTATUS_MIE) != 0;
  const bool mstatus_sie = (mstatus & MSTATUS_SIE) != 0;

  // Every term below needs (mip & mie) of its own bit (MSIP/SSIP/MTIP/STIP/MEIP/SEIP = 0xAAA), so without any such
  // bit all six flags are false: the common case, evaluated once per instruction.
  if ((mip_reg & mie_reg & 0x00000AAAu) == 0) {
    M_software_interrupt = M_timer_interrupt = M_external_interrupt = false;
    S_software_interrupt = S_timer_interrupt = S_external_interrupt = false;
    return;
  }

  M_software_interrupt = (mip_reg & MIP_MSIP) && (mie_reg & MIP_MSIP) &&
                         !(mideleg & MIP_MSIP) &&
                         (privilege < RISCV_MODE_M || mstatus_mie);
  M_timer_interrupt = (mip_reg & MIP_MTIP) && (mie_reg & MIP_MTIP) &&
                      !(mideleg & MIP_MTIP) &&
                      (privilege < RISCV_MODE_M || mstatus_mie);
  M_external_interrupt = (mip_reg & MIP_MEIP) && (mie_reg & MIP_MEIP) &&
                         !(mideleg & MIP_MEIP) &&
                         (privilege < RISCV_MODE_M || mstatus_mie);

  const bool s_irq_enable =
      privilege < RISCV_MODE_S ||
      (privilege == RISCV_MODE_S && mstatus_sie);
  S_software_interrupt =
      (((mip_reg & MIP_MSIP) && (mie_reg & MIP_MSIP) &&
        (mideleg & MIP_MSIP)) ||
       ((mip_reg & MIP_SSIP) && (mie_reg & MIP_SSIP))) &&
      privilege < RISCV_MODE_M && s_irq_enable;
  S_timer_interrupt =
      (((mip_reg & MIP_MTIP) && (mie_reg & MIP_MTIP) &&
        (mideleg & MIP_MTIP)) ||
       ((mip_reg & MIP_STIP) && (mie_reg & MIP_STIP))) &&
      privilege < RISCV_MODE_M && s_irq_enable;
  S_external_interrupt =
      (((mip_reg & MIP_MEIP) && (mie_reg & MIP_MEIP) &&
        (mideleg & MIP_MEIP)) ||
       ((mip_reg & MIP_SEIP) && (mie_reg & MIP_SEIP))) &&
      privilege < RISCV_MODE_M && s_irq_enable;
}

bool Ref_cpu::take_forced_interrupt(xlen_t cause, uint8_t target_privilege,
                                    uint32_t pending_snapshot) {
  constexpr xlen_t kInterruptBit = kXlenMsb;   // the interrupt flag of mcause / scause (bit XLEN-1)
  constexpr uint32_t kPendingMask = kMieBits;
  if ((cause & kInterruptBit) == 0) {
    return false;
  }

  state.csr[csr_mip] = (state.csr[csr_mip] & ~kPendingMask) |
                       (pending_snapshot & kPendingMask);
  state.csr[csr_sip] = state.csr[csr_mip] & delegated_s_bits(state);
  evaluate_interrupts(state.csr[csr_mip]);

  struct Candidate {
    bool active;
    uint8_t privilege;
    uint32_t code;
  };
  const Candidate candidates[] = {
      {M_software_interrupt, RISCV_MODE_M, 3u},
      {M_timer_interrupt, RISCV_MODE_M, 7u},
      {M_external_interrupt, RISCV_MODE_M, 11u},
      {S_external_interrupt, RISCV_MODE_S, 9u},
      {S_timer_interrupt, RISCV_MODE_S, 5u},
      {S_software_interrupt, RISCV_MODE_S, 1u},
  };

  const xlen_t requested_code = cause & ~kInterruptBit;
  const Candidate *selected = nullptr;
  for (const auto &candidate : candidates) {
    if (candidate.active) {
      selected = &candidate;
      break;
    }
  }
  if (selected == nullptr || selected->privilege != target_privilege ||
      selected->code != requested_code) {
    return false;
  }

  Instruction = 0;
  state.store = false;
  state.store_addr = state.store_data = state.store_strb = 0;
  page_fault_inst = page_fault_load = page_fault_store = false;
  illegal_exception = false;
  breakpoint_exception = false;
  is_io = false;
  force_sync = false;
  exception(0);
  evaluate_interrupts(0);
  return true;
}

void Ref_cpu::RISCV() {
  sync_timer_csrs(state, sim_time,
                  device_effects_enable && interrupt_delivery_enable);
  if (privilege == RISCV_MODE_U) {
    if (current_bb_len == 0) {
      current_bb_head_pc = state.pc;
    }
    current_bb_len++;
  }

  is_csr = is_exception = is_br = br_taken = false;
  illegal_exception = page_fault_load = page_fault_inst = page_fault_store =
      asy = false;
  breakpoint_exception = false;
  state.store = false;
  is_io = false;
  force_sync = false;

  uint32_t p_addr = static_cast<uint32_t>(state.pc);

  if (satp_enabled(state.csr[csr_satp]) && privilege != RISCV_MODE_M) {
    page_fault_inst = !va2pa(p_addr, state.pc, 0);

    if (page_fault_inst) {
      exception(state.pc);
      return;
    }
  } else {
    p_addr = bare_pa(state.pc);
  }
  if (__builtin_expect(is_ddr_range(p_addr, 4), 1)) {
    Instruction = memory[(p_addr - kRamBase) >> 2];  // same word load_word() returns for DDR
  } else {
    check_mem_range_or_log("instruction fetch", p_addr, 4);
    Instruction = load_word(p_addr);
  }
  if (__builtin_expect(g_dbg_hot, 0)) {  // debug: ring of the last executed PCs, printed at ebreak when REF_PC_RING is set
    static uint32_t ring[64];
    static uint64_t n = 0;
    ring[n++ & 63] = state.pc;
    static const uint64_t every = std::getenv("REF_PC_EVERY") ? std::strtoull(std::getenv("REF_PC_EVERY"), nullptr, 0) : 0;
    if (every && n % every == 0) { fprintf(stderr, "[pc] n=%llu pc=0x%08x priv=%u time=%llu | Mtraps: illegal=%llu ecallS=%llu Mtimer=%llu\n", (unsigned long long)n, state.pc, (unsigned)privilege, (unsigned long long)sim_time, (unsigned long long)g_dbg_trap_count[2], (unsigned long long)g_dbg_trap_count[9], (unsigned long long)g_dbg_trap_count[16 + 7]); }
    if (Instruction == INST_EBREAK && std::getenv("REF_PC_RING"))
      for (int k = 0; k < 64; k++) fprintf(stderr, "pc=0x%08x\n", ring[(n + k) & 63]);
  }

  if (kLogUserEntrySummary && privilege == RISCV_MODE_U &&
      (state.pc == 0x10000u || state.pc == 0x10428u)) {
    std::cerr << "[RefCPU][U-ENTRY] pc=0x" << std::hex << state.pc
              << " sp=0x" << state.gpr[2] << " ra=0x" << state.gpr[1]
              << " satp=0x" << state.csr[csr_satp] << std::dec << std::endl;
  }

  // RISC-V semihosting marker: slli x0,x0,0x1f ; ebreak ; srai x0,x0,7. OpenSBI's generic platform probes it and
  // expects a breakpoint trap; any other ebreak keeps ending the simulation (the bare-metal tests' exit).
  if (Instruction == INST_EBREAK && load_word(p_addr - 4) == 0x01f01013u && load_word(p_addr + 4) == 0x40705013u) {
    if (std::getenv("REF_PC_RING")) fprintf(stderr, "[bp] pc=0x%08x mtvec=0x%08x a3=0x%08x\n", state.pc, state.csr[csr_mtvec], state.gpr[13]);
    breakpoint_exception = true;
    exception(state.pc);
    return;
  }

  if (Instruction == INST_EBREAK) {
    xlen_t exit_code = state.gpr[10]; // a0
    if (kLogUserTrapSummary && privilege == RISCV_MODE_U) {
      std::cerr << "[RefCPU][U-EBREAK] pc=0x" << std::hex << state.pc
                << " a0=0x" << exit_code << " sp=0x" << state.gpr[2]
                << " ra=0x" << state.gpr[1] << std::dec << std::endl;
    }
    std::cout << "ebreak signal received. pc = 0x" << std::hex << state.pc << " priv=" << privilege << " ra=0x" << state.gpr[1] << " code = 0x" << exit_code
              << std::dec << std::endl;
    std::cout << "total_sim_time: " << sim_time << std::endl;
    if (exit_code == 0) {
      std::cout << "\033[1;32mSuccess!\033[0m" << std::endl;
    } else {
      std::cout << "\033[1;31mTest Failed with code: " << exit_code << "\033[0m"
                << std::endl;
    }
    state.pc += 4;
    sim_end = true;
    return;
  }

  // SBI system shutdown/reset requested by the supervisor (legacy SBI_SHUTDOWN, a7=8, or the SRST extension,
  // a7=0x53525354 / a6=0): end the simulation here instead of letting OpenSBI spin in its wfi hang loop.
  if (Instruction == INST_ECALL && privilege == RISCV_MODE_S &&
      (state.gpr[17] == 8u || (state.gpr[17] == 0x53525354u && state.gpr[16] == 0u))) {
    std::cout << "SBI " << (state.gpr[17] == 8u ? "shutdown" : "system reset") << " requested (a7=0x" << std::hex
              << state.gpr[17] << " a0=0x" << state.gpr[10] << std::dec << "), exiting" << std::endl;
    std::cout << "total_sim_time: " << sim_time << std::endl;
    state.pc += 4;
    sim_end = true;
    return;
  }

  // === 优化 1: 极速解码 ===
  // 使用 BITS 宏直接提取字段，完全替代 bool 数组操作
  uint32_t opcode = BITS(Instruction, 6, 0);

  bool ecall = (Instruction == INST_ECALL);
  bool mret = (Instruction == INST_MRET);
  bool sret = (Instruction == INST_SRET);

  // === 优化 2: 快速读取 CSR 状态 ===
  uint32_t mie_reg = state.csr[csr_mie];
  uint32_t mip_reg = ref_effective_mip(state.csr[csr_mip], sim_time,
                                       device_effects_enable &&
                                           interrupt_delivery_enable);
  if (!interrupt_delivery_enable) {
    mip_reg = 0;
  }

  // Fast path: no interrupt bit is both pending and enabled (0xAAA = MSIP/SSIP/MTIP/STIP/MEIP/SEIP) and the
  // instruction is not ecall/mret/sret.  Then every interrupt flag is false and no trap is taken here (page_fault_inst
  // returned above, illegal_exception is still false), exactly what the general path below computes.
  if ((mip_reg & mie_reg & 0x00000AAAu) == 0 && !ecall && !mret && !sret) {
    M_software_interrupt = M_timer_interrupt = M_external_interrupt = false;
    S_software_interrupt = S_timer_interrupt = S_external_interrupt = false;
    asy = false;
  } else {
    uint32_t medeleg = state.csr[csr_medeleg];

    // 异常委托位 (Exceptions)
    bool medeleg_U_ecall = (medeleg >> 8) & 1;
    bool medeleg_S_ecall = (medeleg >> 9) & 1;
    // bool medeleg_M_ecall = (medeleg >> 11) & 1; // 通常M-ecall不委托

    bool medeleg_page_fault_inst = (medeleg >> 12) & 1;

    evaluate_interrupts(mip_reg);

    // Trap 判断
    bool MTrap =
        M_software_interrupt || M_timer_interrupt || M_external_interrupt ||
        ((privilege == 0) && !medeleg_U_ecall && ecall) || // ecall from U
        ((privilege == 1) && !medeleg_S_ecall && ecall) || // ecall from S
        ((privilege == 3) && ecall) ||                     // ecall from M
        (page_fault_inst && !medeleg_page_fault_inst) || illegal_exception;

    bool STrap = S_software_interrupt || S_timer_interrupt ||
                 S_external_interrupt ||
                 ((privilege == 0) && medeleg_U_ecall && ecall) ||
                 ((privilege == 1) && medeleg_S_ecall && ecall) ||
                 (page_fault_inst && medeleg_page_fault_inst);

    asy = MTrap || STrap || mret || sret;
  }

  if (page_fault_inst) {
    exception(state.pc);
    return;
  } else if (illegal_exception) {
    exception(Instruction);
    return;
  } else if (asy || Instruction == INST_ECALL) {
    exception(0);
    return;
  } else if (opcode == number_10_opcode_ecall) {
    // SYSTEM 指令 (CSR, WFI, MRET等)
    if (Instruction == INST_WFI) {
      is_csr = false;
    } else {
      is_csr = true;
    }
    RV32CSR();
  } else if (opcode == number_11_opcode_lrw) {
    RV32A();
    if (illegal_exception) {
      exception(Instruction);
      return;
    }
#if FP
  } else if (opcode == number_12_opcode_float || opcode == number_13_opcode_fmadd ||
             opcode == number_14_opcode_fmsub || opcode == number_15_opcode_fnmsub ||
             opcode == number_16_opcode_fnmadd) {
    RVFD();
    if (illegal_exception) {
      exception(Instruction);
      return;
    }
#endif
  } else {
    RV32IM();
    if (illegal_exception) {
      exception(Instruction);
      return;
    }
  }
  state.gpr[0] = 0;
}

void Ref_cpu::RV32CSR() {
  // pc + 4
  xlen_t next_pc = state.pc + 4;

  // 使用宏直接提取，无需 copy_indice
  uint32_t rd = BITS(Instruction, 11, 7);
  uint32_t rs1 = BITS(Instruction, 19, 15);
  uint32_t uimm = rs1; // 对于立即数CSR指令，rs1字段就是立即数
  uint32_t csr_addr = BITS(Instruction, 31, 20);
  uint32_t funct3 = BITS(Instruction, 14, 12);

  xlen_t reg_rdata1 = state.gpr[rs1];

  uint32_t wcmd = funct3 & 0b11;
  bool we = wcmd == CSR_W || rs1 != 0;   // csrrw and csrrwi always write (also with a zero operand); csrrs/csrrc[i] only when rs1/uimm != 0
  bool re = wcmd != CSR_W || rd != 0;
  xlen_t csr_wdata = 0, wdata;

  if (funct3 & 0b100) {
    wdata = rs1;
  } else {
    wdata = reg_rdata1;
  }

  auto apply_csr_op = [&](xlen_t old_val, xlen_t operand) -> xlen_t {
    if (wcmd == CSR_W) {
      return operand;
    }
    if (wcmd == CSR_S) {
      return old_val | operand;
    }
    return old_val & ~operand;
  };

  if (funct3 == 0) {
    const bool is_sfence_vma =
        BITS(Instruction, 31, 25) == 0b0001001 && rd == 0;
    if (Instruction == INST_WFI || is_sfence_vma) {
      state.pc = next_pc;
      return;
    }
    illegal_exception = true;
    exception(Instruction);
    return;
  }

#if FP
  if (csr_addr == number_fflags || csr_addr == number_frm ||
      csr_addr == number_fcsr) {
#if FP
    if (!fp_enabled()) {   // mstatus.FS = Off: the FP CSRs are illegal
      illegal_exception = true;
      exception(Instruction);
      return;
    }
#endif
    uint32_t old_val = 0;
    if (csr_addr == number_fflags) {
      old_val = fcsr_fflags & 0x1Fu;
    } else if (csr_addr == number_frm) {
      old_val = fcsr_frm & 0x7u;
    } else {
      old_val = ((fcsr_frm & 0x7u) << 5) | (fcsr_fflags & 0x1Fu);
    }

    if (re) {
      state.gpr[rd] = old_val;
    }

    if (we) {
      uint32_t new_val = apply_csr_op(old_val, wdata);
      if (csr_addr == number_fflags) {
        fcsr_fflags = static_cast<uint8_t>(new_val & 0x1Fu);
      } else if (csr_addr == number_frm) {
        fcsr_frm = static_cast<uint8_t>(new_val & 0x7u);
      } else {
        fcsr_fflags = static_cast<uint8_t>(new_val & 0x1Fu);
        fcsr_frm = static_cast<uint8_t>((new_val >> 5) & 0x7u);
      }
#if FP
      fs_dirty();
#endif
    }
    state.pc = next_pc;
    return;
  }

#endif

  if (csr_addr != number_mtvec && csr_addr != number_mepc &&
      csr_addr != number_mcause && csr_addr != number_mie &&
      csr_addr != number_mip && csr_addr != number_mtval &&
      csr_addr != number_mscratch && csr_addr != number_mstatus &&
      csr_addr != number_mideleg && csr_addr != number_medeleg &&
      csr_addr != number_sepc && csr_addr != number_stvec &&
      csr_addr != number_scause && csr_addr != number_sscratch &&
      csr_addr != number_stval && csr_addr != number_sstatus &&
      csr_addr != number_sie && csr_addr != number_sip &&
      csr_addr != number_satp && csr_addr != number_mhartid &&
      csr_addr != number_misa) {
    illegal_exception = true;
#ifdef ENABLE_SPIKE
    force_sync = true;   // Spike implements CSRs this model does not (mcycle, mhpmcounter*, menvcfg, ...): it must not execute the access; take our trap state instead
#endif
    exception(Instruction);
    return;
  } else {

    int csr_idx = cvt_number_to_csr(csr_addr);
    const bool use_external_csr_read =
        external_csr_read.valid && external_csr_read.address == csr_addr;
    xlen_t observed_csr_value = 0;
    if (use_external_csr_read) {
      observed_csr_value = external_csr_read.value;
    } else if (csr_addr == number_mip) {
      observed_csr_value =
          ref_effective_mip(state.csr[csr_mip], sim_time,
                            device_effects_enable && interrupt_delivery_enable);
    } else if (csr_addr == number_sip) {
      observed_csr_value =
          ref_effective_mip(state.csr[csr_mip], sim_time,
                            device_effects_enable && interrupt_delivery_enable) &
          delegated_s_bits(state);
    } else {
      observed_csr_value = state.csr[csr_idx];
    }

    if (re) {
      state.gpr[rd] = observed_csr_value;
    }

    if (we) {
      xlen_t old_val = observed_csr_value;
      if (wcmd == CSR_W) {
        csr_wdata = wdata;
      } else if (wcmd == CSR_S) {
        csr_wdata = old_val | wdata;
      } else if (wcmd == CSR_C) {
        csr_wdata = old_val & ~wdata;
      }

      if (csr_idx == csr_mie || csr_idx == csr_sie) {
        if (csr_idx == csr_sie) {
          // sie: only the bits delegated by mideleg are visible and writable
          const xlen_t m = delegated_s_bits(state);
          state.csr[csr_mie] = (state.csr[csr_mie] & ~m) | (csr_wdata & m);
        } else {
          state.csr[csr_mie] = csr_wdata & kMieBits;
        }
        refresh_sie(state);

      } else if (csr_idx == csr_mip || csr_idx == csr_sip) {
        xlen_t mip_mask = 0x00000a2au; // DUT write mask 0xbbb minus MTIP: MTIP is derived from mtimecmp; a sticky copy (written back by csrs mip) caused a timer interrupt storm under OpenSBI
        const xlen_t sip_mask = delegated_s_bits(state);   // supervisor view: delegated SEIP / STIP / SSIP

        if (csr_idx == csr_sip) {
          const xlen_t writable = static_cast<uint32_t>(MIP_SSIP) & sip_mask;   // sip: only SSIP, and only if delegated
          state.csr[csr_mip] =
              (state.csr[csr_mip] & ~writable) | (csr_wdata & writable);
        } else {
          state.csr[csr_mip] =
              (state.csr[csr_mip] & ~mip_mask) | (csr_wdata & mip_mask);
        }
        force_sync = true;
        state.csr[csr_sip] =
            ref_effective_mip(state.csr[csr_mip], sim_time,
                              device_effects_enable &&
                                  interrupt_delivery_enable) &
            sip_mask;
        if (kLogVerboseCsrMip &&
            ((csr_wdata & (MIP_STIP | MIP_SSIP)) != 0 ||
             ((old_val ^ state.csr[csr_mip]) & (MIP_STIP | MIP_SSIP)) != 0)) {
          std::cerr << "[RefCPU][CSR] " << (csr_idx == csr_sip ? "sip" : "mip")
                    << " <- 0x" << std::hex << csr_wdata
                    << " old=0x" << old_val << " new_mip=0x"
                    << state.csr[csr_mip] << " new_sip=0x"
                    << state.csr[csr_sip] << " pc=0x" << state.pc << std::dec
                    << std::endl;
        }

      } else if (csr_idx == csr_mstatus || csr_idx == csr_sstatus) {
        const xlen_t mstatus_mask = kMstatusWmask;
        const xlen_t sstatus_mask = kSstatusMask;

        if (csr_idx == csr_sstatus) {
          // sstatus 写入：仅修改 mstatus 中属于 sstatus 掩码范围内的位
          state.csr[csr_mstatus] = (state.csr[csr_mstatus] & ~kSstatusWmask) |
                                   (csr_wdata & kSstatusWmask);
        } else {
          // mstatus 写入：应用 mstatus 写掩码
          state.csr[csr_mstatus] = (state.csr[csr_mstatus] & ~mstatus_mask) |
                                   (csr_wdata & mstatus_mask);
        }
#if FP
        sync_sd();
#endif
        // 同步更新 sstatus 影子值
        state.csr[csr_sstatus] = state.csr[csr_mstatus] & sstatus_mask;

      } else if (csr_idx == csr_mideleg) {
        state.csr[csr_mideleg] = csr_wdata & kSBits;
        refresh_sie(state);
        state.csr[csr_sip] = visible_mip() & delegated_s_bits(state);
      } else if (csr_idx == csr_satp && XLEN == 64 && (csr_wdata >> 60) != 0 && (csr_wdata >> 60) != 8) {
        // unsupported translation mode: the write has no effect (Sv48/Sv57 are not implemented)
      } else {
        state.csr[csr_idx] = csr_wdata;
      }
    }
    if (use_external_csr_read) {
      external_csr_read = {};
    }
  }

  state.pc = next_pc;
}


#if FP
// ---------------------------------------------------------------------------------------------------------------------
// F / D (rv64).  The FP registers are 64 bits; a single-precision value is NaN-boxed (upper word all ones), and a source
// that is not properly boxed reads as the canonical NaN.  Flags accumulate in fcsr.fflags, and every FP-state write marks
// mstatus.FS Dirty.  Behaviour follows Spike (the reference of --diff).
// ---------------------------------------------------------------------------------------------------------------------
bool Ref_cpu::fp_enabled() const { return ((state.csr[csr_mstatus] >> 13) & 3) != 0; }

void Ref_cpu::sync_sd() {
  const xlen_t sd = xlen_t(1) << (XLEN - 1);
  if (((state.csr[csr_mstatus] >> 13) & 3) == 3) state.csr[csr_mstatus] |= sd;
  else state.csr[csr_mstatus] &= ~sd;
}

void Ref_cpu::fs_dirty() {
  state.csr[csr_mstatus] |= xlen_t(3) << 13;
  sync_sd();
  state.csr[csr_sstatus] = state.csr[csr_mstatus] & kSstatusMask;
}

namespace {
constexpr uint32_t kCanonNaN32 = 0x7fc00000u;
constexpr uint64_t kCanonNaN64 = 0x7ff8000000000000ull;
inline uint64_t nanbox32(uint32_t v) { return 0xFFFFFFFF00000000ull | v; }
inline uint32_t unbox32(uint64_t v) { return (v >> 32) == 0xFFFFFFFFu ? static_cast<uint32_t>(v) : kCanonNaN32; }
inline float64_t to_f64(uint64_t v) { float64_t f; f.v = v; return f; }

inline bool is_nan64(uint64_t v) { return ((v >> 52) & 0x7FFu) == 0x7FFu && (v & 0x000FFFFFFFFFFFFFull) != 0; }
inline bool is_snan64(uint64_t v) { return is_nan64(v) && ((v >> 51) & 1u) == 0; }

template <typename UInt, int EXPW, int MANTW>
uint32_t fclass_generic(UInt bits) {
  const UInt sign = (bits >> (EXPW + MANTW)) & 1u;
  const UInt exp = (bits >> MANTW) & ((UInt(1) << EXPW) - 1);
  const UInt mant = bits & ((UInt(1) << MANTW) - 1);
  const bool expmax = exp == ((UInt(1) << EXPW) - 1);
  const bool zero = exp == 0 && mant == 0, sub = exp == 0 && mant != 0, inf = expmax && mant == 0, nan = expmax && mant != 0;
  if (nan) return ((mant >> (MANTW - 1)) & 1u) ? (1u << 9) : (1u << 8);
  if (inf) return sign ? (1u << 0) : (1u << 7);
  if (zero) return sign ? (1u << 3) : (1u << 4);
  if (sub) return sign ? (1u << 2) : (1u << 5);
  return sign ? (1u << 1) : (1u << 6);
}

inline uint64_t f64_min_riscv(uint64_t a, uint64_t b) {
  if (is_snan64(a) || is_snan64(b)) softfloat_exceptionFlags |= softfloat_flag_invalid;
  const bool an = is_nan64(a), bn = is_nan64(b);
  if (an && bn) return kCanonNaN64;
  if (an) return b;
  if (bn) return a;
  if (f64_lt(to_f64(a), to_f64(b))) return a;
  if (f64_lt(to_f64(b), to_f64(a))) return b;
  return a | b;   // -0.0 wins on ties
}
inline uint64_t f64_max_riscv(uint64_t a, uint64_t b) {
  if (is_snan64(a) || is_snan64(b)) softfloat_exceptionFlags |= softfloat_flag_invalid;
  const bool an = is_nan64(a), bn = is_nan64(b);
  if (an && bn) return kCanonNaN64;
  if (an) return b;
  if (bn) return a;
  if (f64_lt(to_f64(a), to_f64(b))) return b;
  if (f64_lt(to_f64(b), to_f64(a))) return a;
  return a & b;   // +0.0 wins on ties
}
}  // namespace

void Ref_cpu::RVFD() {
  const xlen_t next_pc = state.pc + 4;
  const uint32_t opcode = BITS(Instruction, 6, 0);
  const uint32_t rd = BITS(Instruction, 11, 7), rs1 = BITS(Instruction, 19, 15), rs2 = BITS(Instruction, 24, 20);
  const uint32_t rs3 = BITS(Instruction, 31, 27), funct3 = BITS(Instruction, 14, 12);
  const uint32_t fmt = BITS(Instruction, 26, 25), funct5 = BITS(Instruction, 31, 27);
  if (!fp_enabled() || fmt > 1) { illegal_exception = true; return; }
  const bool D = fmt == 1;

  // rounding mode: the rm field, or fcsr.frm when it is 7 (dynamic); 5, 6 and a dynamic 5..7 are reserved
  auto set_rm = [&]() -> bool {
    const uint32_t rm = funct3 == 7 ? (fcsr_frm & 7u) : funct3;
    switch (rm) {
    case 0: softfloat_roundingMode = softfloat_round_near_even; return true;
    case 1: softfloat_roundingMode = softfloat_round_minMag; return true;
    case 2: softfloat_roundingMode = softfloat_round_min; return true;
    case 3: softfloat_roundingMode = softfloat_round_max; return true;
    case 4: softfloat_roundingMode = softfloat_round_near_maxMag; return true;
    default: return false;
    }
  };
  softfloat_exceptionFlags = 0;
  auto bad = [&]() { illegal_exception = true; };
  auto wrfS = [&](uint32_t v) { state.fpr[rd] = nanbox32(v); fs_dirty(); };
  auto wrfD = [&](uint64_t v) { state.fpr[rd] = v; fs_dirty(); };
  auto wrx = [&](xlen_t v) { if (rd != 0) state.gpr[rd] = v; };
  const uint32_t a32 = unbox32(state.fpr[rs1]), b32 = unbox32(state.fpr[rs2]), c32 = unbox32(state.fpr[rs3]);
  const uint64_t a64 = state.fpr[rs1], b64 = state.fpr[rs2], c64 = state.fpr[rs3];
  constexpr uint32_t S32 = 0x80000000u;
  constexpr uint64_t S64 = 0x8000000000000000ull;

  if (opcode != number_12_opcode_float) {   // FMADD / FMSUB / FNMSUB / FNMADD
    if (!set_rm()) { bad(); return; }
    if (D) {
      uint64_t x = a64, z = c64;
      if (opcode == number_15_opcode_fnmsub || opcode == number_16_opcode_fnmadd) x ^= S64;
      if (opcode == number_14_opcode_fmsub || opcode == number_16_opcode_fnmadd) z ^= S64;
      wrfD(f64_mulAdd(to_f64(x), to_f64(b64), to_f64(z)).v);
    } else {
      uint32_t x = a32, z = c32;
      if (opcode == number_15_opcode_fnmsub || opcode == number_16_opcode_fnmadd) x ^= S32;
      if (opcode == number_14_opcode_fmsub || opcode == number_16_opcode_fnmadd) z ^= S32;
      wrfS(f32_mulAdd(to_f32(x), to_f32(b32), to_f32(z)).v);
    }
  } else {
    switch (funct5) {
    case 0x00: case 0x01: case 0x02: case 0x03: {   // fadd fsub fmul fdiv
      if (!set_rm()) { bad(); return; }
      if (D) {
        const float64_t x = to_f64(a64), y = to_f64(b64);
        wrfD((funct5 == 0 ? f64_add(x, y) : funct5 == 1 ? f64_sub(x, y) : funct5 == 2 ? f64_mul(x, y) : f64_div(x, y)).v);
      } else {
        const float32_t x = to_f32(a32), y = to_f32(b32);
        wrfS((funct5 == 0 ? f32_add(x, y) : funct5 == 1 ? f32_sub(x, y) : funct5 == 2 ? f32_mul(x, y) : f32_div(x, y)).v);
      }
      break;
    }
    case 0x0B:   // fsqrt
      if (rs2 != 0 || !set_rm()) { bad(); return; }
      if (D) wrfD(f64_sqrt(to_f64(a64)).v); else wrfS(f32_sqrt(to_f32(a32)).v);
      break;
    case 0x04:   // fsgnj fsgnjn fsgnjx
      if (funct3 > 2) { bad(); return; }
      if (D) {
        const uint64_t sgn = funct3 == 0 ? (b64 & S64) : funct3 == 1 ? (~b64 & S64) : ((a64 ^ b64) & S64);
        wrfD((a64 & ~S64) | sgn);
      } else {
        const uint32_t sgn = funct3 == 0 ? (b32 & S32) : funct3 == 1 ? (~b32 & S32) : ((a32 ^ b32) & S32);
        wrfS((a32 & ~S32) | sgn);
      }
      break;
    case 0x05:   // fmin fmax
      if (funct3 > 1) { bad(); return; }
      if (D) wrfD(funct3 == 0 ? f64_min_riscv(a64, b64) : f64_max_riscv(a64, b64));
      else wrfS(funct3 == 0 ? f32_min_riscv(a32, b32) : f32_max_riscv(a32, b32));
      break;
    case 0x08:   // fcvt.s.d (fmt S, rs2 = 1) / fcvt.d.s (fmt D, rs2 = 0)
      if (D) {
        if (rs2 != 0 || !set_rm()) { bad(); return; }   // (Spike checks rm here too, though the conversion is exact)
        wrfD(f32_to_f64(to_f32(a32)).v);
      } else {
        if (rs2 != 1 || !set_rm()) { bad(); return; }
        wrfS(f64_to_f32(to_f64(a64)).v);
      }
      break;
    case 0x14: {   // feq flt fle
      if (funct3 > 2) { bad(); return; }
      bool r;
      if (D) r = funct3 == 2 ? f64_eq(to_f64(a64), to_f64(b64)) : funct3 == 1 ? f64_lt(to_f64(a64), to_f64(b64)) : f64_le(to_f64(a64), to_f64(b64));
      else r = funct3 == 2 ? f32_eq(to_f32(a32), to_f32(b32)) : funct3 == 1 ? f32_lt(to_f32(a32), to_f32(b32)) : f32_le(to_f32(a32), to_f32(b32));
      wrx(r ? 1 : 0);
      break;
    }
    case 0x18: {   // fcvt.w / wu / l / lu . fmt
      if (rs2 > (XLEN == 64 ? 3 : 1) || !set_rm()) { bad(); return; }   // fcvt.l / lu exist on rv64 only
      const uint_fast8_t rm = softfloat_roundingMode;
      xlen_t r;
      if (D) {
        const float64_t x = to_f64(a64);
        r = rs2 == 0 ? static_cast<xlen_t>(static_cast<int64_t>(f64_to_i32(x, rm, true)))
          : rs2 == 1 ? static_cast<xlen_t>(static_cast<int64_t>(static_cast<int32_t>(f64_to_ui32(x, rm, true))))
          : rs2 == 2 ? static_cast<xlen_t>(f64_to_i64(x, rm, true)) : static_cast<xlen_t>(f64_to_ui64(x, rm, true));
      } else {
        const float32_t x = to_f32(a32);
        r = rs2 == 0 ? static_cast<xlen_t>(static_cast<int64_t>(f32_to_i32(x, rm, true)))
          : rs2 == 1 ? static_cast<xlen_t>(static_cast<int64_t>(static_cast<int32_t>(f32_to_ui32(x, rm, true))))
          : rs2 == 2 ? static_cast<xlen_t>(f32_to_i64(x, rm, true)) : static_cast<xlen_t>(f32_to_ui64(x, rm, true));
      }
      wrx(r);
      break;
    }
    case 0x1A: {   // fcvt.fmt.w / wu / l / lu
      if (rs2 > (XLEN == 64 ? 3 : 1)) { bad(); return; }
      if (!set_rm()) { bad(); return; }   // (Spike checks rm for fcvt.d.w / .wu too, though they are exact)
      const xlen_t x = state.gpr[rs1];
      if (D) {
        wrfD((rs2 == 0 ? i32_to_f64(static_cast<int32_t>(x)) : rs2 == 1 ? ui32_to_f64(static_cast<uint32_t>(x))
              : rs2 == 2 ? i64_to_f64(static_cast<int64_t>(x)) : ui64_to_f64(x)).v);
      } else {
        wrfS((rs2 == 0 ? i32_to_f32(static_cast<int32_t>(x)) : rs2 == 1 ? ui32_to_f32(static_cast<uint32_t>(x))
              : rs2 == 2 ? i64_to_f32(static_cast<int64_t>(x)) : ui64_to_f32(x)).v);
      }
      break;
    }
    case 0x1C:   // fmv.x.w / fmv.x.d (funct3 0), fclass (funct3 1)
      if (rs2 != 0 || funct3 > 1 || (XLEN == 32 && D && funct3 == 0)) { bad(); return; }   // fmv.x.d: rv64 only
      if (funct3 == 0) wrx(D ? static_cast<xlen_t>(state.fpr[rs1]) : static_cast<xlen_t>(static_cast<int64_t>(static_cast<int32_t>(state.fpr[rs1]))));
      else wrx(D ? fclass_generic<uint64_t, 11, 52>(a64) : fclass_generic<uint32_t, 8, 23>(a32));
      break;
    case 0x1E:   // fmv.w.x / fmv.d.x
      if (rs2 != 0 || funct3 != 0 || (XLEN == 32 && D)) { bad(); return; }   // fmv.d.x: rv64 only
      if (D) wrfD(state.gpr[rs1]); else wrfS(static_cast<uint32_t>(state.gpr[rs1]));
      break;
    default:
      bad();
      return;
    }
  }
  if (softfloat_exceptionFlags) {
    fcsr_fflags = static_cast<uint8_t>((fcsr_fflags | softfloat_exceptionFlags) & 0x1Fu);
    fs_dirty();
  }
  state.pc = next_pc;
}
#endif  // FP


void Ref_cpu::RV32IM() {
  // pc + 4
  xlen_t next_pc = state.pc + 4;
  uint32_t opcode = BITS(Instruction, 6, 0);
  uint32_t funct3 = BITS(Instruction, 14, 12);
  uint32_t funct7 = BITS(Instruction, 31, 25);
  uint32_t reg_d_index = BITS(Instruction, 11, 7);
  uint32_t reg_a_index = BITS(Instruction, 19, 15);
  uint32_t reg_b_index = BITS(Instruction, 24, 20);
#if FP
  // flw/fld/fsw/fsd run the integer load/store code (same translation, faults, MMIO, store path); only the register file
  // differs.  funct3 2 / 3 of LOAD-FP / STORE-FP equals lw / ld (sw / sd).
  bool fp_mem = false;
  if (opcode == 0x07 || opcode == 0x27) {
    if (!fp_enabled() || (funct3 != 2 && funct3 != 3)) {
      illegal_exception = true;
      return;
    }
    fp_mem = true;
    opcode = (opcode == 0x07) ? number_5_opcode_lb : number_6_opcode_sb;
  }
#endif
  // shift-immediate encodings: rv64 has a 6-bit shamt, so bit 25 belongs to it and only funct7[6:1] selects the operation
  const uint32_t funct7s = (XLEN == 64) ? (funct7 & ~1u) : funct7;

  const xlen_t reg_rdata1 = state.gpr[reg_a_index];
#if FP
  const xlen_t reg_rdata2 = fp_mem ? static_cast<xlen_t>(state.fpr[reg_b_index]) : state.gpr[reg_b_index];
#else
  const xlen_t reg_rdata2 = state.gpr[reg_b_index];
#endif
  const sxlen_t s_rdata1 = static_cast<sxlen_t>(reg_rdata1);
  const sxlen_t s_rdata2 = static_cast<sxlen_t>(reg_rdata2);
  bool handled = false;   // OP / OP-IMM: set when an encoding selects an implemented operation
  auto wr = [&](xlen_t v) { state.gpr[reg_d_index] = v; handled = true; };

  switch (opcode) {
  case number_0_opcode_lui: { // lui
    state.gpr[reg_d_index] = static_cast<xlen_t>(immU(Instruction));
    break;
  }
  case number_1_opcode_auipc: { // auipc
    state.gpr[reg_d_index] = static_cast<xlen_t>(immU(Instruction)) + state.pc;
    break;
  }
  case number_2_opcode_jal: { // jal
    is_br = true;
    br_taken = true;
    next_pc = state.pc + static_cast<xlen_t>(immJ(Instruction));
    state.gpr[reg_d_index] = state.pc + 4;
    break;
  }
  case number_3_opcode_jalr: { // jalr
    is_br = true;
    br_taken = true;
    next_pc = (reg_rdata1 + static_cast<xlen_t>(immI(Instruction))) & ~xlen_t(3);
    state.gpr[reg_d_index] = state.pc + 4;
    break;
  }
  case number_4_opcode_beq: { // beq, bne, blt, bge, bltu, bgeu
    is_br = true;
    bool taken = false;
    switch (funct3) {
    case 0: taken = reg_rdata1 == reg_rdata2; break;
    case 1: taken = reg_rdata1 != reg_rdata2; break;
    case 4: taken = s_rdata1 < s_rdata2; break;
    case 5: taken = s_rdata1 >= s_rdata2; break;
    case 6: taken = reg_rdata1 < reg_rdata2; break;
    case 7: taken = reg_rdata1 >= reg_rdata2; break;
    default: break;
    }
    if (taken) {
      br_taken = true;
      next_pc = state.pc + static_cast<xlen_t>(immB(Instruction));
    }
    break;
  }
  case number_5_opcode_lb: { // lb, lh, lw, lbu, lhu (rv64: ld, lwu)
    xlen_t v_addr = reg_rdata1 + static_cast<xlen_t>(immI(Instruction));
    uint32_t p_addr = static_cast<uint32_t>(v_addr);
    uint32_t access_size;
    bool is_unsigned = (funct3 & 0b100) != 0;
#if XLEN == 64
    switch (funct3) {
    case 0: case 4: access_size = 1; break;
    case 1: case 5: access_size = 2; break;
    case 2: case 6: access_size = 4; break;
    case 3: access_size = 8; break;
    default: illegal_exception = true; return;
    }
#else
    const uint32_t size = funct3 & 0b11;
    access_size = (size == 0b00) ? 1 : ((size == 0b01) ? 2 : 4);
#if FP
    if (fp_mem && funct3 == 3) access_size = 8;   // fld on rv32: 8 bytes (two words); funct3 == 3 is not a legal integer load there
#endif
#endif
    if (v_addr % access_size != 0) {
      std::cerr << "Misaligned Load! addr: 0x" << std::hex << v_addr << std::endl;
      exit(-1);
    }

    const xlen_t load_mstatus = state.csr[csr_mstatus];
    const uint32_t load_eff_priv =
        (load_mstatus & MSTATUS_MPRV) ? GET_MPP(load_mstatus) : privilege;
    if (satp_enabled(state.csr[csr_satp]) && load_eff_priv != RISCV_MODE_M) {
      page_fault_load = !va2pa(p_addr, v_addr, 1);
    } else {
      p_addr = bare_pa(v_addr);
    }

    if (!is_ram_range(p_addr, access_size)) {
      is_io = true;
    }

    if (page_fault_load) {
      exception(v_addr);
      return;
    }
    check_mem_range_or_log("load", p_addr, access_size);
    uint64_t data;   // (the FP double load is 64 bits wide on rv32 too)
    if (access_size == 8) {
      uint32_t lo = load_word(p_addr), hi = load_word(p_addr + 4);
      if (device_effects_enable) {
        lo = timer_word_override(p_addr, lo, sim_time);
        hi = timer_word_override(p_addr + 4, hi, sim_time);
      }
      data = (static_cast<uint64_t>(hi) << 32) | lo;
    } else {
      uint32_t word = load_word(p_addr);
      const uint32_t raw_word = word;
      word >>= (p_addr & 0b11) * 8;
      uint32_t mask = access_size == 1 ? 0xFFu : (access_size == 2 ? 0xFFFFu : 0xFFFFFFFFu);
      word &= mask;
      if (is_unsigned) {
        data = word;   // lbu, lhu, lwu
      } else if (access_size == 1) {
        data = static_cast<xlen_t>(static_cast<sxlen_t>(static_cast<int8_t>(word)));
      } else if (access_size == 2) {
        data = static_cast<xlen_t>(static_cast<sxlen_t>(static_cast<int16_t>(word)));
      } else {
        data = sext32(word);   // lw
      }
      data = static_cast<uint64_t>(static_cast<xlen_t>(data));   // (rv32: the register-width value)
      if (p_addr >= UART_BASE && p_addr < (UART_BASE + UART_MMIO_SIZE)) {
        trace_uart_load(sim_time, state.pc, p_addr, static_cast<uint8_t>(funct3), raw_word,
                        static_cast<uint32_t>(data));
      }
      if (device_effects_enable) {
        const uint32_t ov = timer_word_override(p_addr, static_cast<uint32_t>(data), sim_time);
        if (ov != static_cast<uint32_t>(data)) data = is_unsigned ? static_cast<xlen_t>(ov) : sext32(ov);
      }
    }
#if FP
    if (fp_mem) {
      state.fpr[reg_d_index] = access_size == 4 ? (0xFFFFFFFF00000000ull | static_cast<uint32_t>(data)) : data;   // flw boxes
      fs_dirty();
      break;
    }
#endif
    state.gpr[reg_d_index] = static_cast<xlen_t>(data);
    break;
  }
  case number_6_opcode_sb: { // sb, sh, sw (rv64: sd)
    xlen_t v_addr = reg_rdata1 + static_cast<xlen_t>(immS(Instruction));
    uint32_t p_addr = static_cast<uint32_t>(v_addr);
    uint32_t access_size;
    switch (funct3) {
    case 0: access_size = 1; break;
    case 1: access_size = 2; break;
    case 2: access_size = 4; break;
#if XLEN == 64
    case 3: access_size = 8; break;
#endif
    default:
#if XLEN == 64
      illegal_exception = true; return;
#else
#if FP
      if (fp_mem && funct3 == 3) { access_size = 8; break; }   // fsd on rv32: 8 bytes (two words)
#endif
      access_size = 4; break;
#endif
    }
    if (v_addr % access_size != 0) {
      std::cerr << "Misaligned Store! addr: 0x" << std::hex << v_addr << std::endl;
      exit(-1);
    }

    const xlen_t store_mstatus = state.csr[csr_mstatus];
    const uint32_t store_eff_priv =
        (store_mstatus & MSTATUS_MPRV) ? GET_MPP(store_mstatus) : privilege;
    if (satp_enabled(state.csr[csr_satp]) && store_eff_priv != RISCV_MODE_M) {
      page_fault_store = !va2pa(p_addr, v_addr, 2);
    } else {
      p_addr = bare_pa(v_addr);
    }

    if (!is_ram_range(p_addr, access_size)) {
      is_io = true;
    }

    if (page_fault_store) {
      exception(v_addr);
      return;
    }
    check_mem_range_or_log("store", p_addr, access_size);

#if FP && XLEN == 32
    if (access_size == 8) {   // fsd on rv32: the double is 64 bits wide, the register width is 32: two word writes
      const uint64_t d = state.fpr[reg_b_index];
      state.store = true;
      state.store_strb = 0b1111;
      state.store_addr = p_addr;
      state.store_data = static_cast<uint32_t>(d);
      store_data_word();
      state.store_addr = p_addr + 4;
      state.store_data = static_cast<uint32_t>(d >> 32);
      store_data_word();
      state.store_addr = p_addr;                       // report the whole store (the low word's data)
      state.store_data = static_cast<uint32_t>(d);
      state.store_strb = 0xFFu;
      break;
    }
#endif
    state.store = true;
    state.store_addr = p_addr;
    state.store_data = reg_rdata2;
    if (access_size == 1) {
      state.store_strb = 0b1;
      state.store_data &= 0xFF;
    } else if (access_size == 2) {
      state.store_strb = 0b11;
      state.store_data &= 0xFFFF;
    } else if (access_size == 4) {
      state.store_strb = 0b1111;
      state.store_data &= 0xFFFFFFFFu;
    } else {
      state.store_strb = 0xFF;
    }
    store_data();
    break;
  }
  case number_7_opcode_addi: { // OP-IMM: addi, slti, sltiu, xori, ori, andi, slli, srli, srai, Zbb/Zbs immediates
    const xlen_t imm = static_cast<xlen_t>(immI(Instruction));
    const uint32_t shamt = (XLEN == 64) ? BITS(Instruction, 25, 20) : BITS(Instruction, 24, 20);
    switch (funct3) {
    case 0: wr(reg_rdata1 + imm); break;                                   // addi
    case 2: wr(s_rdata1 < static_cast<sxlen_t>(imm) ? 1 : 0); break;       // slti
    case 3: wr(reg_rdata1 < imm ? 1 : 0); break;                           // sltiu
    case 4: wr(reg_rdata1 ^ imm); break;                                   // xori
    case 6: wr(reg_rdata1 | imm); break;                                   // ori
    case 7: wr(reg_rdata1 & imm); break;                                   // andi
    case 1: { // slli, bseti, bclri, binvi, clz, ctz, cpop, sext.b, sext.h
      if (funct7s == 0) {
        wr(reg_rdata1 << shamt);
      } else if (funct7 == 0x30) { // Zbb unary: the rs2 field selects the operation (rv64: shamt[5] must be 0)
        if (reg_b_index == 0) wr(xlen_clz(reg_rdata1));
        else if (reg_b_index == 1) wr(xlen_ctz(reg_rdata1));
        else if (reg_b_index == 2) wr(xlen_cpop(reg_rdata1));
        else if (reg_b_index == 4) wr(static_cast<xlen_t>(static_cast<sxlen_t>(static_cast<int8_t>(reg_rdata1 & 0xFF))));
        else if (reg_b_index == 5) wr(static_cast<xlen_t>(static_cast<sxlen_t>(static_cast<int16_t>(reg_rdata1 & 0xFFFF))));
      } else if (funct7s == 0x14) { // bseti
        wr(reg_rdata1 | (xlen_t(1) << shamt));
      } else if (funct7s == 0x24) { // bclri
        wr(reg_rdata1 & ~(xlen_t(1) << shamt));
      } else if (funct7s == 0x34) { // binvi
        wr(reg_rdata1 ^ (xlen_t(1) << shamt));
      }
      break;
    }
    case 5: { // srli, srai, rori, bexti, rev8, orc.b
      if (funct7s == 0) {
        wr(reg_rdata1 >> shamt);
      } else if (funct7s == 0x20) {
        wr(static_cast<xlen_t>(s_rdata1 >> shamt));
      } else if (funct7s == 0x30) { // rori
        wr(xlen_ror(reg_rdata1, shamt));
      } else if (funct7s == 0x24) { // bexti
        wr((reg_rdata1 >> shamt) & 1);
      } else if (funct7s == 0x34) { // rev8: rv32 imm = 0x698, rv64 imm = 0x6b8 (shamt = 56)
        if (reg_b_index == 24 && funct7 == (XLEN == 64 ? 0x35u : 0x34u)) wr(xlen_rev8(reg_rdata1));
      } else if (funct7s == 0x14) { // orc.b
        if (reg_b_index == 7) wr(xlen_orcb(reg_rdata1));
      }
      break;
    }
    }
    if (!handled) illegal_exception = true;   // reserved encoding
    break;
  }
  case number_8_opcode_add: { // OP: add, sub, sll, slt, sltu, xor, srl, sra, or, and; M; Zba/Zbb/Zbs/Zbc
    if (funct7 == 1) {        // mul div
      switch (funct3) {
      case 0: wr(reg_rdata1 * reg_rdata2); break;               // mul
      case 1: wr(mul_hi_ss(reg_rdata1, reg_rdata2)); break;     // mulh
      case 2: wr(mul_hi_su(reg_rdata1, reg_rdata2)); break;     // mulhsu
      case 3: wr(mul_hi_uu(reg_rdata1, reg_rdata2)); break;     // mulhu
      case 4: wr(xlen_div(s_rdata1, s_rdata2)); break;          // div
      case 5: wr(xlen_divu(reg_rdata1, reg_rdata2)); break;     // divu
      case 6: wr(xlen_rem(s_rdata1, s_rdata2)); break;          // rem
      case 7: wr(xlen_remu(reg_rdata1, reg_rdata2)); break;     // remu
      }
    } else {
      switch (funct3) {
      case 0: { // add, sub
        if (funct7 == 0) wr(reg_rdata1 + reg_rdata2);
        else if (funct7 == 0x20) wr(reg_rdata1 - reg_rdata2);
        break;
      }
      case 1: { // sll, rol, bclr, bset, binv, clmul
        const uint32_t shift = reg_rdata2 & kShamtMask;
        if (funct7 == 0) wr(reg_rdata1 << shift);
        else if (funct7 == 0x30) wr(xlen_rol(reg_rdata1, shift));
        else if (funct7 == 0x24) wr(reg_rdata1 & ~(xlen_t(1) << shift));
        else if (funct7 == 0x14) wr(reg_rdata1 | (xlen_t(1) << shift));
        else if (funct7 == 0x34) wr(reg_rdata1 ^ (xlen_t(1) << shift));
        else if (funct7 == 0x05) wr(xlen_clmul(reg_rdata1, reg_rdata2));
        break;
      }
      case 2: { // slt, sh1add, clmulr
        if (funct7 == 0) wr(s_rdata1 < s_rdata2 ? 1 : 0);
        else if (funct7 == 0x10) wr(reg_rdata2 + (reg_rdata1 << 1));
        else if (funct7 == 0x05) wr(xlen_clmulr(reg_rdata1, reg_rdata2));
        break;
      }
      case 3: { // sltu, clmulh
        if (funct7 == 0) wr(reg_rdata1 < reg_rdata2 ? 1 : 0);
        else if (funct7 == 0x05) wr(xlen_clmulh(reg_rdata1, reg_rdata2));
        break;
      }
      case 4: { // xor, xnor, sh2add, min, pack
        if (funct7 == 0) wr(reg_rdata1 ^ reg_rdata2);
        else if (funct7 == 0x20) wr(~(reg_rdata1 ^ reg_rdata2));
        else if (funct7 == 0x10) wr(reg_rdata2 + (reg_rdata1 << 2));
        else if (funct7 == 0x05) wr((s_rdata1 < s_rdata2) ? reg_rdata1 : reg_rdata2);
        else if (funct7 == 0x04) {  // pack: low XLEN/2 bits of rs1, low XLEN/2 bits of rs2 above them
          const xlen_t lo_mask = (xlen_t(1) << (XLEN / 2)) - 1;
          wr((reg_rdata1 & lo_mask) | (reg_rdata2 << (XLEN / 2)));
        }
        break;
      }
      case 5: { // srl, sra, ror, bext, minu
        const uint32_t shift = reg_rdata2 & kShamtMask;
        if (funct7 == 0) wr(reg_rdata1 >> shift);
        else if (funct7 == 0x20) wr(static_cast<xlen_t>(s_rdata1 >> shift));
        else if (funct7 == 0x30) wr(xlen_ror(reg_rdata1, shift));
        else if (funct7 == 0x24) wr((reg_rdata1 >> shift) & 1);
        else if (funct7 == 0x05) wr((reg_rdata1 < reg_rdata2) ? reg_rdata1 : reg_rdata2);
        break;
      }
      case 6: { // or, orn, max, sh3add
        if (funct7 == 0) wr(reg_rdata1 | reg_rdata2);
        else if (funct7 == 0x20) wr(reg_rdata1 | (~reg_rdata2));
        else if (funct7 == 0x05) wr((s_rdata1 > s_rdata2) ? reg_rdata1 : reg_rdata2);
        else if (funct7 == 0x10) wr(reg_rdata2 + (reg_rdata1 << 3));
        break;
      }
      case 7: { // and, andn, maxu, packh
        if (funct7 == 0) wr(reg_rdata1 & reg_rdata2);
        else if (funct7 == 0x20) wr(reg_rdata1 & (~reg_rdata2));
        else if (funct7 == 0x05) wr((reg_rdata1 > reg_rdata2) ? reg_rdata1 : reg_rdata2);
        else if (funct7 == 0x04) wr((reg_rdata1 & 0xFF) | ((reg_rdata2 & 0xFF) << 8));
        break;
      }
      }
    }
    if (!handled) illegal_exception = true;   // reserved encoding
    break;
  }
#if XLEN == 64
  case 0b0011011: { // OP-IMM-32: addiw, slliw, srliw, sraiw, slli.uw, clzw, ctzw, cpopw, roriw
    const xlen_t imm = static_cast<xlen_t>(immI(Instruction));
    const uint32_t shamt = BITS(Instruction, 24, 20);
    const uint32_t a32 = static_cast<uint32_t>(reg_rdata1);
    switch (funct3) {
    case 0: state.gpr[reg_d_index] = sext32(static_cast<uint32_t>(reg_rdata1 + imm)); break;   // addiw
    case 1:
      if (funct7 == 0) {                                // slliw
        state.gpr[reg_d_index] = sext32(a32 << shamt);
      } else if (funct7s == 0x04) {                     // slli.uw (6-bit shamt)
        state.gpr[reg_d_index] = static_cast<xlen_t>(a32) << BITS(Instruction, 25, 20);
      } else if (funct7 == 0x30) {
        if (reg_b_index == 0) state.gpr[reg_d_index] = a32 == 0 ? 32 : __builtin_clz(a32);       // clzw
        else if (reg_b_index == 1) state.gpr[reg_d_index] = a32 == 0 ? 32 : __builtin_ctz(a32);  // ctzw
        else if (reg_b_index == 2) state.gpr[reg_d_index] = __builtin_popcount(a32);             // cpopw
        else illegal_exception = true;
      } else {
        illegal_exception = true;
      }
      break;
    case 5:
      if (funct7 == 0) state.gpr[reg_d_index] = sext32(a32 >> shamt);                            // srliw
      else if (funct7 == 0x20) state.gpr[reg_d_index] = sext32(static_cast<uint32_t>(static_cast<int32_t>(a32) >> shamt));  // sraiw
      else if (funct7 == 0x30) state.gpr[reg_d_index] = sext32(w_ror(a32, shamt));               // roriw
      else illegal_exception = true;
      break;
    default: illegal_exception = true; break;
    }
    break;
  }
  case 0b0111011: { // OP-32: addw, subw, sllw, srlw, sraw, mulw, divw, divuw, remw, remuw, add.uw, shNadd.uw, rolw, rorw, zext.h
    const uint32_t a32 = static_cast<uint32_t>(reg_rdata1);
    const uint32_t b32 = static_cast<uint32_t>(reg_rdata2);
    const uint32_t shift = b32 & 31;
    if (funct7 == 1) {
      switch (funct3) {
      case 0: state.gpr[reg_d_index] = sext32(a32 * b32); break;                                  // mulw
      case 4: state.gpr[reg_d_index] = w_div(static_cast<int32_t>(a32), static_cast<int32_t>(b32)); break;
      case 5: state.gpr[reg_d_index] = w_divu(a32, b32); break;
      case 6: state.gpr[reg_d_index] = w_rem(static_cast<int32_t>(a32), static_cast<int32_t>(b32)); break;
      case 7: state.gpr[reg_d_index] = w_remu(a32, b32); break;
      default: illegal_exception = true; break;
      }
    } else {
      switch (funct3) {
      case 0:
        if (funct7 == 0) state.gpr[reg_d_index] = sext32(a32 + b32);                              // addw
        else if (funct7 == 0x20) state.gpr[reg_d_index] = sext32(a32 - b32);                      // subw
        else if (funct7 == 0x04) state.gpr[reg_d_index] = static_cast<xlen_t>(a32) + reg_rdata2;  // add.uw
        else illegal_exception = true;
        break;
      case 1:
        if (funct7 == 0) state.gpr[reg_d_index] = sext32(a32 << shift);                           // sllw
        else if (funct7 == 0x30) state.gpr[reg_d_index] = sext32(w_rol(a32, shift));              // rolw
        else illegal_exception = true;
        break;
      case 2:
        if (funct7 == 0x10) state.gpr[reg_d_index] = reg_rdata2 + (static_cast<xlen_t>(a32) << 1); // sh1add.uw
        else illegal_exception = true;
        break;
      case 4:
        if (funct7 == 0x10) state.gpr[reg_d_index] = reg_rdata2 + (static_cast<xlen_t>(a32) << 2); // sh2add.uw
        else if (funct7 == 0x04) state.gpr[reg_d_index] = sext32((a32 & 0xFFFFu) | (b32 << 16));   // packw (zext.h = packw with rs2 = x0)
        else illegal_exception = true;
        break;
      case 5:
        if (funct7 == 0) state.gpr[reg_d_index] = sext32(a32 >> shift);                           // srlw
        else if (funct7 == 0x20) state.gpr[reg_d_index] = sext32(static_cast<uint32_t>(static_cast<int32_t>(a32) >> shift));  // sraw
        else if (funct7 == 0x30) state.gpr[reg_d_index] = sext32(w_ror(a32, shift));              // rorw
        else illegal_exception = true;
        break;
      case 6:
        if (funct7 == 0x10) state.gpr[reg_d_index] = reg_rdata2 + (static_cast<xlen_t>(a32) << 3); // sh3add.uw
        else illegal_exception = true;
        break;
      default: illegal_exception = true; break;
      }
    }
    break;
  }
#endif
  case number_9_opcode_fence: { // fence, fence.i
    break;
  }
  default: {
    illegal_exception = true;
    break;
  }
  }

  if (illegal_exception) {
    return;
  }
  state.pc = next_pc;
}

uint32_t Ref_cpu::load_word(uint32_t addr) const {
  const uint32_t word_addr = addr & ~0x3u;
  if (is_ddr_range(word_addr, 4)) {
    return memory[(word_addr - kRamBase) >> 2];
  }
  if (is_sdram_range(word_addr, 4)) {
    return sdram_memory[(word_addr - kSdramBase) >> 2];
  }

  check_mem_range_or_log("word load", word_addr, 4);
  if (word_addr >= UART_BASE && word_addr < (UART_BASE + UART_MMIO_SIZE)) {
    auto it = io_words.find(word_addr);
    if (it == io_words.end()) {
      return 0;
    }
    return it->second;
  }
  if (word_addr >= XPS_INTC_BASE &&
      word_addr < (XPS_INTC_BASE + XPS_INTC_MMIO_SIZE)) {
    return xps_intc_read_reg(word_addr);
  }
  if (word_addr >= OCSDC_BASE && word_addr < (OCSDC_BASE + OCSDC_MMIO_SIZE)) {
    return ocsdc_read_reg(word_addr);
  }
  auto it = io_words.find(word_addr);
  return (it == io_words.end()) ? 0 : it->second;
}

void Ref_cpu::store_word(uint32_t addr, uint32_t data) {
  const uint32_t word_addr = addr & ~0x3u;
  if (device_effects_enable && word_addr == TIMER_BASE) {
    const uint64_t now = sim_time;
    const uint64_t current = ref_timer_now(now);
    ref_write_timer_value((current & 0xFFFFFFFF00000000ull) |
                              static_cast<uint64_t>(data),
                          now);
    return;
  }
  if (device_effects_enable && word_addr == (TIMER_BASE + 4u)) {
    const uint64_t now = sim_time;
    const uint64_t current = ref_timer_now(now);
    ref_write_timer_value((static_cast<uint64_t>(data) << 32) |
                              (current & 0x00000000FFFFFFFFull),
                          now);
    return;
  }
  if (device_effects_enable && word_addr == (TIMER_BASE + 8u)) {
    g_ref_timer_cmp = (g_ref_timer_cmp & 0xFFFFFFFF00000000ull) |
                      static_cast<uint64_t>(data);
    if (kLogVerboseTimer) {
      std::cerr << "[RefCPU][TIMER] mtimecmp_lo <- 0x" << std::hex << data
                << " full=0x" << g_ref_timer_cmp << " now=0x"
                << ref_timer_now(sim_time) << std::dec
                << std::endl;
    }
    return;
  }
  if (device_effects_enable && word_addr == (TIMER_BASE + 12u)) {
    g_ref_timer_cmp = (static_cast<uint64_t>(data) << 32) |
                      (g_ref_timer_cmp & 0x00000000FFFFFFFFull);
    if (kLogVerboseTimer) {
      std::cerr << "[RefCPU][TIMER] mtimecmp_hi <- 0x" << std::hex << data
                << " full=0x" << g_ref_timer_cmp << " now=0x"
                << ref_timer_now(sim_time) << std::dec
                << std::endl;
    }
    return;
  }
  if (is_sdram_range(word_addr, 4)) {
    if (const uint8_t f = pt_page_flag[word_addr >> 12]) page_flag_hit(f);
    sdram_memory[(word_addr - kSdramBase) >> 2] = data;
    return;
  }
  if (is_ddr_range(word_addr, 4)) {
    if (const uint8_t f = pt_page_flag[word_addr >> 12]) page_flag_hit(f);
    memory[(word_addr - kRamBase) >> 2] = data;
    return;
  }

  check_mem_range_or_log("word store", word_addr, 4);
  if (word_addr >= UART_BASE && word_addr < (UART_BASE + UART_MMIO_SIZE)) {
    io_words[word_addr] = data;
    return;
  }
  if (word_addr >= XPS_INTC_BASE &&
      word_addr < (XPS_INTC_BASE + XPS_INTC_MMIO_SIZE)) {
    xps_intc_write_reg(word_addr, data);
    return;
  }
  if (word_addr >= OCSDC_BASE && word_addr < (OCSDC_BASE + OCSDC_MMIO_SIZE)) {
    ocsdc_write_reg(word_addr, data);
    return;
  }
  io_words[word_addr] = data;
}

void Ref_cpu::store_data() {
  if (state.store_strb == 0xFFu) {  // sd / amo*.d: aligned 8 bytes = two word writes
    const xlen_t d = state.store_data;
    const uint32_t a = state.store_addr;
    state.store_addr = a;
    state.store_data = static_cast<uint32_t>(d);
    state.store_strb = 0b1111;
    store_data_word();
    state.store_addr = a + 4;
    state.store_data = static_cast<uint32_t>(d >> 32);
    state.store_strb = 0b1111;
    store_data_word();
    state.store_addr = a;   // report the whole store
    state.store_data = d;
    state.store_strb = 0xFFu;
    return;
  }
  store_data_word();
}

void Ref_cpu::store_data_word() {
  uint32_t p_addr = state.store_addr;
  uint32_t write_size =
      (state.store_strb == 0b1) ? 1 : ((state.store_strb == 0b11) ? 2 : 4);
  check_mem_range_or_log("store_data", p_addr, write_size);
  if (state.store && state.reserve_valid && state.reserve_addr == p_addr) {
    // Keep exported state semantics aligned with the old in-tree refcpu.
    state.reserve_valid = false;
  }
  int offset = p_addr & 0x3;
  uint32_t wstrb = state.store_strb << offset;
  uint32_t wdata = static_cast<uint32_t>(state.store_data) << (offset * 8);
  const bool trace_uart =
      p_addr >= UART_BASE && p_addr < (UART_BASE + UART_MMIO_SIZE);
  const uint32_t trace_uart_pc = state.pc;
  const uint32_t trace_uart_before0 =
      trace_uart ? load_word(UART_BASE) : 0u;
  uint32_t old_data = load_word(p_addr);
  uint32_t mask = 0;

  if (wstrb & 0b1)
    mask |= 0xFF;
  if (wstrb & 0b10)
    mask |= 0xFF00;
  if (wstrb & 0b100)
    mask |= 0xFF0000;
  if (wstrb & 0b1000)
    mask |= 0xFF000000;

  const bool suppress_external_device_store =
      !device_effects_enable && !is_ram_range(p_addr & ~0x3u, 4);
  if (state.store && !suppress_external_device_store) {
    store_word(p_addr, (mask & wdata) | (~mask & old_data));
  }

  if (device_effects_enable && p_addr >= UART_BASE &&
      p_addr < (UART_BASE + UART_MMIO_SIZE)) {
    const uint32_t reg = p_addr - UART_BASE;
    const uint8_t value =
        static_cast<uint8_t>((wdata >> (offset * 8)) & 0xffu);
    const uint8_t lcr = uart_read_reg8(io_words, UART_BASE + kUartRegLcr);
    const bool dlab = (lcr & kUartLcrDlab) != 0;

    if (reg == kUartRegRxTxDll && !dlab) {
      uart_write_reg8(io_words, UART_BASE + kUartRegRxTxDll, 0);
      uart_write_reg8(io_words, UART_BASE + kUartRegLsr,
                      kUartLsrThre | kUartLsrTemt);
      if (uart_print && privilege != RISCV_MODE_U &&
          !g_disable_uart_console_output) {
        const char ch = static_cast<char>(value);
        std::cout << ch << std::flush;
        if (observe_uart_console_byte(sim_time, ch)) {
          sim_end = true;
        }
      }
      uart_refresh_interrupt();
    } else if (reg == kUartRegRxTxDll && dlab) {
      uart_write_reg8(io_words, UART_BASE + kUartRegRxTxDll, value);
    } else if (reg == kUartRegIerDlm && dlab) {
      uart_write_reg8(io_words, UART_BASE + kUartRegIerDlm, value);
    } else if (reg == kUartRegIerDlm && !dlab) {
      uart_write_reg8(io_words, UART_BASE + kUartRegIerDlm, value);
      uart_refresh_interrupt();
    } else if (reg == kUartRegIirFcr) {
      uart_write_reg8(io_words, UART_BASE + kUartRegIirFcr,
                      static_cast<uint8_t>(kUartIirNoInt |
                                           kUartIirFifoEnabled));
      uart_refresh_interrupt();
    } else if (reg == kUartRegLcr) {
      uart_write_reg8(io_words, UART_BASE + kUartRegLcr, value);
    } else if (reg == kUartRegMcr) {
      uart_write_reg8(io_words, UART_BASE + kUartRegMcr, value);
    } else if (reg == kUartRegScr) {
      uart_write_reg8(io_words, UART_BASE + kUartRegScr, value);
    }
  }

  if (trace_uart) {
    trace_uart_store_effect(sim_time, trace_uart_pc, p_addr,
                            static_cast<uint8_t>(wstrb), wdata,
                            trace_uart_before0, load_word(UART_BASE),
                            load_word(UART_BASE + 4u));
  }

  state.store_data = state.store_data << offset * 8;
  state.store_strb = state.store_strb << offset * 8;
}

void Ref_cpu::tlb_flush() {
  if (++tlb_epoch == 0) {  // epoch wrapped: make every entry invalid explicitly
    for (auto &e : tlb) e.epoch = 0;
    tlb_epoch = 1;
  }
  for (uint32_t p : pt_marked) pt_page_flag[p] &= ~1;
  pt_marked.clear();
  icache_flush();
}

void Ref_cpu::icache_flush() {
  if (++icache_epoch >= (1u << 30)) {  // the epoch is 30 bits in a block's ctx: start over, every block invalid
    for (auto &fb : fblocks) fb.ctx = 0;
    icache_epoch = 1;
  }
  for (uint32_t p : code_marked) pt_page_flag[p] &= ~2;
  code_marked.clear();
}

bool Ref_cpu::va2pa(uint32_t &p_addr, xlen_t v_addr, uint32_t type) {
  const xlen_t mstatus = state.csr[csr_mstatus];
  const xlen_t satp = state.csr[csr_satp];
  uint32_t eff_priv = privilege;  // as in va2pa_walk: MPRV applies to everything but instruction fetch
  if (type != 0 && (mstatus & MSTATUS_MPRV)) {
    eff_priv = (mstatus >> MSTATUS_MPP_SHIFT) & 0x3;
  }
  const uint32_t ctx = (type & 3u) | (eff_priv << 2) | ((mstatus & MSTATUS_MXR) ? 16u : 0u) |
                       ((mstatus & MSTATUS_SUM) ? 32u : 0u);
  const uint64_t vpn = v_addr >> 12;
  TlbEntry &e = tlb[(static_cast<uint32_t>(vpn) ^ (type << 7)) & (kTlbSize - 1)];
  if (e.epoch == tlb_epoch && e.vpn == vpn && e.satp == satp && e.ctx == ctx) {
    p_addr = (e.frame << 12) | (v_addr & 0xFFF);
    return true;
  }
  if (!va2pa_walk(p_addr, v_addr, type)) {
    return false;  // faults are not cached
  }
  e.epoch = tlb_epoch;
  e.vpn = vpn;
  e.satp = satp;
  e.ctx = ctx;
  e.frame = p_addr >> 12;
  return true;
}

#if XLEN == 32
bool Ref_cpu::va2pa_walk(uint32_t &p_addr, xlen_t v_addr, uint32_t type) {
  uint32_t mstatus = state.csr[csr_mstatus];
  uint32_t sstatus = state.csr[csr_sstatus];
  uint32_t satp = state.csr[csr_satp];

  // 1. 提取状态位 (直接位运算，极快)
  bool mxr = (mstatus & MSTATUS_MXR) != 0;
  bool sum = (mstatus & MSTATUS_SUM) != 0;
  bool mprv = (mstatus & MSTATUS_MPRV) != 0;

  // 确定有效特权级 (Effective Privilege Mode)
  // 如果 MPRV=1 且不是取指(type!=0)，则使用 MPP 作为特权级进行检查
  int eff_priv = privilege;
  if (type != 0 && mprv) {
    eff_priv = (mstatus >> MSTATUS_MPP_SHIFT) & 0x3;
  }

  // 2. Level 1 Page Table Walk
  // satp 的 PPN 字段在 SV32 中是低 22 位 (0-21)
  // VPN[1] 是 v_addr 的 [31:22] 位
  // pte1_addr = (satp.ppn << 12) + (vpn1 * 4)
  // 你的原代码逻辑：(satp << 12) | ((v_addr >> 20) & 0xFFC)
  // 等价于下面的位操作：
  uint32_t ppn_root = satp & 0x3FFFFF; // 提取 SATP 中的 PPN
  uint32_t vpn1 = (v_addr >> 22) & 0x3FF;
  uint32_t pte1_addr = (ppn_root << 12) | (vpn1 << 2);

  // 直接读取，注意这里需要确保 memory 是按字寻址还是字节寻址
  check_mem_range_or_log("ptw-l1", pte1_addr, 4);
  mark_pt_page(pte1_addr);
  uint32_t pte1 = load_word(pte1_addr);

  // 3. 检查 PTE 有效性
  // !V 或者 (!R && W) 都是无效的
  if (!(pte1 & PTE_V) || (!(pte1 & PTE_R) && (pte1 & PTE_W))) {
    return false;
  }

  // 4. 判断是否是叶子节点 (R=1 或 X=1)
  if ((pte1 & PTE_R) || (pte1 & PTE_X)) {
    // --- Superpage (4MB) ---

    // 权限检查 (Permission Check)
    // Fetch (0): 需要 X
    if (type == 0 && !(pte1 & PTE_X))
      return false;
    // Load (1): 需要 R，或者 (MXR=1 且 X=1)
    if (type == 1 && !(pte1 & PTE_R) && !(mxr && (pte1 & PTE_X)))
      return false;
    // Store (2): 需要 W
    if (type == 2 && !(pte1 & PTE_W))
      return false;

    // 用户权限检查 (User/Supervisor Check)
    bool is_user_page = (pte1 & PTE_U) != 0;
    if (eff_priv == 0 && !is_user_page)
      return false; // U-mode 访问 S-page -> Fault
    if (eff_priv == 1 && is_user_page) {
      if (type == 0 || !sum)
        return false; // SUM never permits S-mode instruction fetch from U pages.
    }

    // 对齐检查 (Superpage 要求 PPN[0] 为 0)
    // PPN[0] 对应 PTE 的 [19:10] 位
    if ((pte1 >> 10) & 0x3FF)
      return false;

    // A/D 位检查
    if (!(pte1 & PTE_A))
      return false; // Accessed 必须为 1 (硬件不自动设置时需报错)
    if (type == 2 && !(pte1 & PTE_D))
      return false; // 写操作 Dirty 必须为 1

    // 计算物理地址 (Superpage)
    // PA = PPN[1] | VPN[0] | Offset
    // PPN[1] 是 PTE[31:20]，对应 PA[31:22]
    // v_addr & 0x3FFFFF 保留低 22 位 (VPN[0] + Offset)
    p_addr = ((pte1 << 2) & 0xFFC00000) | (v_addr & 0x3FFFFF);
    return true;
  }

  // 5. Level 2 Page Table Walk (非叶子节点，指向下一级页表)
  // PPN 是 PTE 的 [31:10] 位
  uint32_t ppn1 = (pte1 >> 10) & 0x3FFFFF;
  uint32_t vpn0 = (v_addr >> 12) & 0x3FF;
  uint32_t pte2_addr = (ppn1 << 12) | (vpn0 << 2);

  check_mem_range_or_log("ptw-l2", pte2_addr, 4);
  mark_pt_page(pte2_addr);
  uint32_t pte2 = load_word(pte2_addr);

  // 重复有效性检查
  if (!(pte2 & PTE_V) || (!(pte2 & PTE_R) && (pte2 & PTE_W))) {
    return false;
  }

  // Level 2 必须是叶子节点 (SV32 只有两级)
  if ((pte2 & PTE_R) || (pte2 & PTE_X)) {
    // --- 4KB Page ---

    // 权限检查 (逻辑同上)
    if (type == 0 && !(pte2 & PTE_X))
      return false;
    if (type == 1 && !(pte2 & PTE_R) && !(mxr && (pte2 & PTE_X)))
      return false;
    if (type == 2 && !(pte2 & PTE_W))
      return false;

    // 用户权限检查
    bool is_user_page = (pte2 & PTE_U) != 0;
    if (eff_priv == 0 && !is_user_page)
      return false;
    if (eff_priv == 1 && is_user_page) {
      if (type == 0 || !sum)
        return false;
    }

    // A/D 位检查
    if (!(pte2 & PTE_A))
      return false;
    if (type == 2 && !(pte2 & PTE_D))
      return false;

    // 计算物理地址 (4KB Page)
    // PA = PPN | Offset
    // PPN 是 PTE[31:10]，对应 PA[31:12]
    // Offset 是 v_addr[11:0]
    p_addr = ((pte2 >> 10) << 12) | (v_addr & 0xFFF);
    return true;
  }

  return false; // 如果 Level 2 还不是叶子节点，则是非法页表
}
#else  // XLEN == 64: Sv39

// Three-level page walk (RISC-V privileged spec 4.3.2).  Accessed/dirty bits are not updated by hardware: a leaf with
// A = 0 (or D = 0 on a store) faults, as in the Sv32 walk.  Physical addresses above 4 GiB are not backed by this SoC
// and fault.
bool Ref_cpu::va2pa_walk(uint32_t &p_addr, xlen_t v_addr, uint32_t type) {
  const xlen_t mstatus = state.csr[csr_mstatus];
  const xlen_t satp = state.csr[csr_satp];
  const bool mxr = (mstatus & MSTATUS_MXR) != 0;
  const bool sum = (mstatus & MSTATUS_SUM) != 0;
  const bool mprv = (mstatus & MSTATUS_MPRV) != 0;
  int eff_priv = privilege;
  if (type != 0 && mprv) {
    eff_priv = (mstatus >> MSTATUS_MPP_SHIFT) & 0x3;
  }

  // the virtual address must be sign-extended from bit 38
  const int64_t top = static_cast<int64_t>(v_addr) >> 38;
  if (top != 0 && top != -1) {
    return false;
  }

  uint64_t table = (satp & 0xFFFFFFFFFFFull) << 12;  // satp.PPN (44 bits)
  for (int level = 2; level >= 0; --level) {
    const uint64_t vpn = (v_addr >> (12 + 9 * level)) & 0x1FF;
    const uint64_t pte_addr = table + vpn * 8;
    if ((pte_addr >> 32) != 0) {
      return false;
    }
    check_mem_range_or_log("ptw", static_cast<uint32_t>(pte_addr), 8);
    mark_pt_page(static_cast<uint32_t>(pte_addr));
    const uint64_t pte = (static_cast<uint64_t>(load_word(static_cast<uint32_t>(pte_addr) + 4)) << 32) |
                         load_word(static_cast<uint32_t>(pte_addr));

    if (!(pte & PTE_V) || (!(pte & PTE_R) && (pte & PTE_W))) {
      return false;
    }
    if (pte >> 54) {  // reserved bits / Svnapot / Svpbmt are not implemented: must be zero
      return false;
    }
    if (!(pte & PTE_R) && !(pte & PTE_X)) {  // pointer to the next level
      if (level == 0) {
        return false;
      }
      table = ((pte >> 10) & 0xFFFFFFFFFFFull) << 12;
      continue;
    }

    // leaf: permissions
    if (type == 0 && !(pte & PTE_X)) return false;
    if (type == 1 && !(pte & PTE_R) && !(mxr && (pte & PTE_X))) return false;
    if (type == 2 && !(pte & PTE_W)) return false;
    const bool is_user_page = (pte & PTE_U) != 0;
    if (eff_priv == 0 && !is_user_page) return false;
    if (eff_priv == 1 && is_user_page) {
      if (type == 0 || !sum) return false;
    }
    // superpage alignment: the PPN bits below the leaf level must be zero
    const uint64_t ppn = (pte >> 10) & 0xFFFFFFFFFFFull;
    if (level > 0 && (ppn & ((1ull << (9 * level)) - 1)) != 0) {
      return false;
    }
    if (!(pte & PTE_A)) return false;
    if (type == 2 && !(pte & PTE_D)) return false;

    const uint64_t offset_mask = (1ull << (12 + 9 * level)) - 1;
    const uint64_t pa = ((ppn << 12) & ~offset_mask) | (v_addr & offset_mask);
    if ((pa >> 32) != 0) {
      return false;
    }
    p_addr = static_cast<uint32_t>(pa);
    return true;
  }
  return false;
}
#endif
