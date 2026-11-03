#!/usr/bin/env bash
# Compare a front-door restore (restore program from ckpt2stub.py, only the memory loaded) with the back-door restore
# (CPU state and devices injected) from the same checkpoint: the instructions that follow must be identical.
# Every <EVERY>-th instruction the state digest (pc, x1..x31, privilege, CSRs without mstatus.MPP/MPIE and mepc) is printed
# by both runs and compared.
# usage: frontdoor_check.sh <simulator> <checkpoint.gz> <image.bin> <n instructions> [every]
set -u
SIM=$1; CK=$2; IMG=$3; N=$4; EVERY=${5:-9973}
T=$(cd "$(dirname "$0")" && pwd)
D=$(mktemp -d); trap 'rm -rf "$D"' EXIT
python3 "$T/ckpt2stub.py" "$CK" "$D/stub.bin"
REF_STATE_DIGEST=$EVERY REF_STATE_DIGEST_FULL=1 "$SIM" --mode restore --restore-file "$CK" --restore-mem-only \
  --flash-image "$D/stub.bin" --max-insts "$N" > "$D/front.out" 2> "$D/front.err"
REF_STATE_DIGEST=$EVERY REF_STATE_DIGEST_FULL=1 "$SIM" --mode restore --restore-file "$CK" --image "$IMG" \
  --max-insts "$N" > "$D/back.out" 2> "$D/back.err"
grep -a '^\[front-door\]' "$D/front.out"
nb=$(grep -ac '^\[digest\]' "$D/back.err")
# the front-door run also prints digests for the restore program itself, before the counters restart: skip them
if cmp -s <(grep -a '^\[digest\]' "$D/front.err" | tail -n "$nb") <(grep -a '^\[digest\]' "$D/back.err"); then
  echo "IDENTICAL: $nb digests over $N instructions"
else
  echo "DIFFERENT"; exit 1
fi
