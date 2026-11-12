#!/usr/bin/env bash
# Official riscv-tests rv<XLEN>uf / rv<XLEN>ud (self-checking) on the function model with F/D (FP=1), each also compared with Spike after
# every instruction.  The tests end with `li a0, 0|1; ebreak` (0 = pass), the convention of riscv-tests/isa/riscv_test.h in the
# simulator tree; the wrapper header added here defines RVTEST_RV64UF and turns mstatus.FS on first (the official env does that
# in its reset code, which this bare-metal start.S does not have).
#   usage: [XLEN=32|64] tests/rv64/riscv-tests-fp.sh <work-dir>      (XLEN default 64)
#   env:   RISCV_TESTS (checkout with isa/, default ~/personal/simulator/baremetal/riscv-tests), TRACE (trace-model checkout,
#          default ../trace-model), SPIKE_SRC / SPIKE_BUILD (scripts/build_spike.sh), SPIKE_OUT (an existing FP=1 Spike-checked a.out)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd); W=${1:?work dir}; TRACE=${TRACE:-$ROOT/../trace-model}
RT=${RISCV_TESTS:-$HOME/personal/simulator/baremetal/riscv-tests}; L=${LLVM:-/opt/homebrew/opt/llvm/bin}
XLEN=${XLEN:-64}
if [ "$XLEN" = 64 ]; then CT="--target=riscv64 -march=rv64imafd_zicsr_zifencei -mabi=lp64d -mcmodel=medany"; else CT="--target=riscv32 -march=rv32imafd_zicsr_zifencei -mabi=ilp32d -mcmodel=medlow"; fi
mkdir -p "$W/env"
SP=${SPIKE_OUT:-}
if [ -z "$SP" ]; then XLEN=$XLEN FP=1 OUT="$W/spfp" bash "$ROOT/scripts/build_spike.sh" >/dev/null; SP="$W/spfp/a.out"; fi
BM=$TRACE/baremetal
sed -e 's/(wxa!ri)/(wxa)/' -e 's/^\( *\)\. = __stack_size;/\1. += __stack_size;/' -e 's/\(\.stack ORIGIN[^:]*\):/\1 (NOLOAD) :/' "$BM/link.lds" > "$W/env/link.lds.0"
python3 - "$W/env/link.lds.0" "$W/env/link.lds" <<'PY'
import sys
t=open(sys.argv[1]).read()
t=t.replace("  .data ","  .rodata : { *(.rodata .rodata.* .srodata .srodata.*) } >ram AT>ram\n  .data ",1)
t=t.replace("  .dma_buffer","  .bss (NOLOAD) : { *(.sbss .sbss.* .bss .bss.* COMMON) . = ALIGN(8); } >ram AT>ram\n\n  .dma_buffer",1)
open(sys.argv[2],"w").write(t)
PY
cat > "$W/env/riscv_test.h" <<HDR
#include "$RT/isa/riscv_test.h"
#define RVTEST_RV${XLEN}UF
#undef RVTEST_CODE_BEGIN
#define RVTEST_CODE_BEGIN .section .text; .globl main; main: li t0, 0x2000; csrs mstatus, t0;
HDR
$L/clang $CT -c "$BM/start.S" -o "$W/env/start.o"
pass=0; fail=0
for suite in rv${XLEN}uf rv${XLEN}ud; do
  for s in "$RT/isa/$suite"/*.S; do
    t=$suite-$(basename "$s" .S); d=$W/$t; mkdir -p "$d"
  [ "$XLEN$suite" = "32rv32ud" ] && [ "$(basename "$s" .S)" = move ] && continue   # rv32ud has no move test upstream (fmv.x.d / fmv.d.x are rv64 only)
    if ! $L/clang $CT -ffreestanding -nostdlib \
         -I"$W/env" -I"$RT/isa/macros/scalar" -c "$s" -o "$d/t.o" 2>"$d/cc.log" ||
       ! ${LLD:-/opt/homebrew/bin/ld.lld} -T "$W/env/link.lds" --gc-sections "$W/env/start.o" "$d/t.o" -o "$d/t.elf" 2>"$d/ld.log" ||
       ! $L/llvm-objcopy -O binary "$d/t.elf" "$d/t.bin"; then echo "BUILD FAIL $t"; fail=$((fail+1)); continue; fi
    out=$(REF_UNBUF=1 perl -e 'alarm 120; exec @ARGV' "$SP" --image "$d/t.bin" --diff 2>&1 || true)
    if echo "$out" | grep -q 'Success' && echo "$out" | grep -q 'code = 0x0'; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL $t"; echo "$out" | grep -m3 -E "Reason|code =|MISMATCH"; fi
  done
done
echo "riscv-tests rv${XLEN}uf/rv${XLEN}ud (self-check pass + Spike lockstep): pass=$pass fail=$fail"
