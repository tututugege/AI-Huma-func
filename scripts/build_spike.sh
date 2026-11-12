#!/usr/bin/env bash
# Build the Spike-checked function model (a.out with --diff) OUT OF TREE, so that its objects never mix with the plain
# build (the Makefile uses the same .o names for both and trace-model links librefcpu.a from the plain ones).
#   usage: scripts/build_spike.sh            env: XLEN (32 or 64, default 32), FP (1 = F/D, rv64 only, default 0), SPIKE_SRC (riscv-isa-sim checkout), SPIKE_BUILD (its built tree: libriscv.a libfesvr.a
#                                                libsoftfloat.a libdisasm.a libfdt.a config.h), OUT (default build-spike)
# Spike here is configured without PMP; the function model traps on every CSR it does not implement (force_sync in exec.cpp).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SPIKE_SRC=${SPIKE_SRC:?set SPIKE_SRC to a riscv-isa-sim checkout}
SPIKE_BUILD=${SPIKE_BUILD:?set SPIKE_BUILD to the built riscv-isa-sim tree}
XLEN=${XLEN:-32}
FP=${FP:-1}
OUT=${OUT:-$ROOT/build-spike}
rm -rf "$OUT"; mkdir -p "$OUT"
cp -R "$ROOT"/*.cpp "$ROOT"/include "$ROOT"/Makefile "$ROOT"/lib "$OUT"/
make -C "$OUT" SPIKE=1 XLEN="$XLEN" FP="$FP" SPIKE_SRC="$SPIKE_SRC" SPIKE_BUILD="$SPIKE_BUILD" -j"${JOBS:-8}" 2>&1 | grep -E "error|undefined" || true
ls -la "$OUT/a.out"
echo 'run: REF_UNBUF=1 REF_MAX_INSTS=<n> '"$OUT"'/a.out --image <image.bin> --diff'
