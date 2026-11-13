#!/usr/bin/env bash
# C extension (RVC=1) vs Spike, compared after every instruction: the generated directed tests (tests/rv64/gen_rvc.py: every 16-bit
# encoding, loads/stores, control transfers, 32-bit instructions straddling a cache line / page, Sv39 second-half page faults), the
# trace-model cpu-tests compiled with C, and the official riscv-tests rv<XLEN>uc.
#   usage: [XLEN=32|64] tests/rv64/rvc.sh <work-dir>      (XLEN default 64)
#   env:   SPIKE_SRC / SPIKE_BUILD (scripts/build_spike.sh), TRACE (trace-model checkout, default ../trace-model), RISCV_TESTS (see riscv-tests-uc.sh)
# Known differences, all in the test sources (written for 4-byte instructions), not in the model: interrupt-vectored,
# supervisor-external-interrupt-vectored (vector table of 4-byte slots; a c.j is 2 bytes), trap-precise-store (skips `ecall; sw` by +8),
# plus software-interrupt-vectored / timer-interrupt-mask (platform timer, as without C).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd); W=${1:?work dir}; TRACE=${TRACE:-$ROOT/../trace-model}
XLEN=${XLEN:-64}
mkdir -p "$W"
XLEN=$XLEN FP=1 RVC=1 OUT="$W/sp" bash "$ROOT/scripts/build_spike.sh" >/dev/null
SP="$W/sp/a.out"
if [ "$XLEN" = 64 ]; then export TARGET=riscv64 ABI=lp64 CMODEL=medany ARCH=rv64imac_zicsr_zifencei; else export TARGET=riscv32 ABI=ilp32 CMODEL=medlow ARCH=rv32imac_zicsr_zifencei; fi
python3 "$ROOT/tests/rv64/gen_rvc.py" "$W/gen" $XLEN >/dev/null
names="rvc-alu rvc-mem fd-rvc-mem rvc-flow rvc-straddle"; [ "$XLEN" = 64 ] && names="$names rvc-sv39"
EXTRA_DIR="$W/gen" bash "$TRACE/scripts/build_tests.sh" "$TRACE" "$W/t" $names | grep -v ' OK$' || true
ct=$(cd "$TRACE/baremetal/test/cpu-tests" && ls *.c | sed 's/\.c$//' | grep -v -E '^(zfinx|matmul_|load-store$|string$|bitmanip-boundary$)')
bash "$TRACE/scripts/build_tests.sh" "$TRACE" "$W/t" $ct | grep -v ' OK$' || true
pass=0; fail=0
for d in "$W"/t/*/; do
  t=$(basename "$d"); [ -f "$d/$t.bin" ] || continue
  if REF_UNBUF=1 perl -e 'alarm 600; exec @ARGV' "$SP" --image "$d/$t.bin" --diff > "$d/diff.log" 2>&1 && grep -q Success "$d/diff.log"; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL $t"; fi
done
echo "rv$XLEN C vs Spike: pass=$pass fail=$fail (known: interrupt-vectored supervisor-external-interrupt-vectored trap-precise-store software-interrupt-vectored timer-interrupt-mask)"
SPIKE_OUT="$SP" XLEN=$XLEN TRACE="$TRACE" bash "$ROOT/tests/rv64/riscv-tests-uc.sh" "$W/uc"
