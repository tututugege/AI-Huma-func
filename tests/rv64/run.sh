#!/usr/bin/env bash
# rv64 function-model vs Spike: build the Spike-checked a.out (XLEN=64), generate and build the directed tests, run them with --diff.
#   usage: tests/rv64/run.sh <work-dir>     env: SPIKE_SRC / SPIKE_BUILD (see scripts/build_spike.sh), TRACE (trace-model checkout, default ../trace-model)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd); W=${1:?work dir}; TRACE=${TRACE:-$ROOT/../trace-model}
mkdir -p "$W"
XLEN=64 OUT="$W/sp64" bash "$ROOT/scripts/build_spike.sh" >/dev/null
python3 "$ROOT/tests/rv64/gen_isa.py" "$W/gen" >/dev/null
python3 "$ROOT/tests/rv64/gen_sv39.py" "$W/gen" >/dev/null
export CMODEL=medany TARGET=riscv64 ABI=lp64 EXTRA_DIR="$W/gen"
# the generated tests use Zbc/Zbkb mnemonics; the compiled C tests stay at rv64imab (Zba/Zbb/Zbs)
ARCH=rv64imab_zbc_zbkb_zicsr_zifencei bash "$TRACE/scripts/build_tests.sh" "$TRACE" "$W/t64" rv64-isa rv64-sv39 | grep -v ' OK$' || true
ARCH=rv64imab_zicsr_zifencei bash "$TRACE/scripts/build_tests.sh" "$TRACE" "$W/t64" $(cd "$TRACE/baremetal/test/cpu-tests" && ls *.c | sed 's/\.c$//' | grep -v -E '^(zfinx|matmul_|load-store$|string$|bitmanip-boundary$)') | grep -v ' OK$' || true
# software-interrupt-vectored / timer-interrupt-mask: mip.MSIP / MTIP come from the platform, Spike has no such source (known)
pass=0; fail=0
for d in "$W"/t64/*/; do
  t=$(basename "$d"); [ -f "$d/$t.bin" ] || continue
  if REF_UNBUF=1 perl -e 'alarm 300; exec @ARGV' "$W/sp64/a.out" --image "$d/$t.bin" --diff 2>&1 | grep -q 'Success'; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL $t"; fi
done
echo "rv64 vs Spike: pass=$pass fail=$fail (software-interrupt-vectored and timer-interrupt-mask are the known platform differences)"

# ---- F/D (the default build): the Spike-checked model uses Spike's rv64imafdb; directed tests (every FP instruction x rounding mode x
# special operands, trap/status/reserved encodings) and compiler-generated FP code, each compared with Spike after every instruction.
# FP_TESTS=0 skips this part.
if [ "${FP_TESTS:-1}" != 0 ]; then
  python3 "$ROOT/tests/rv64/gen_fp.py" "$W/genfp" >/dev/null
  python3 "$ROOT/tests/rv64/gen_fp_trap.py" "$W/genfp" >/dev/null
  cp "$ROOT/tests/rv64/fp-kernels.c" "$W/genfp/"
  export CMODEL=medany TARGET=riscv64 ABI=lp64d EXTRA_DIR="$W/genfp"
  ARCH=rv64imafd_zicsr_zifencei bash "$TRACE/scripts/build_tests.sh" "$TRACE" "$W/tfp" rv64-fp rv64-fp-trap fp-kernels | grep -v ' OK$' || true
  fpp=0; fpf=0
  for t in rv64-fp rv64-fp-trap fp-kernels; do
    if REF_UNBUF=1 perl -e 'alarm 900; exec @ARGV' "$W/sp64/a.out" --image "$W/tfp/$t/$t.bin" --diff 2>&1 | grep -q 'Success'; then fpp=$((fpp+1)); else fpf=$((fpf+1)); echo "FAIL $t (FP)"; fi
  done
  echo "rv64 F/D vs Spike: pass=$fpp fail=$fpf"
fi
