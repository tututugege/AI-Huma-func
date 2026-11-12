#!/usr/bin/env bash
# One non-SPEC workload: validate the BBV run -> SimPoint (maxK $MAXK, default 30) -> checkpoints (warmup = 1 interval).
# Same layout as the SPEC pipeline (../spec/pipeline.sh) under $WLDATA, so trace-model/scripts/pf/run_point.sh works with SPEC=$WLDATA.
# usage: pipeline.sh <wl>      status -> $WLDATA/status/<wl>.txt
set -u
w=$1
HERE=$(cd "$(dirname "$0")" && pwd); . "$HERE/../spec/env.sh"
R=${WLDATA:-$HOME/personal/AI-Huma-data/wl-rv32imab}
LOG=$R/bbv/$w.log; ST=$R/status/$w.txt
mkdir -p $R/status $R/sp $R/ckpt/$w
say() { echo "$(date '+%F %T') $*" | tee -a $ST; }
fail() { say "FAIL: $*"; exit 1; }
: > $ST
grep -a -q "SBI shutdown requested" $LOG || fail "no SBI shutdown line"
grep -a -q "Simulation Timeout" $LOG && fail "instruction cap reached"
grep -a -q "Oops\|Kernel panic" $LOG && fail "kernel Oops/panic"
CLEAN=$(mktemp); perl -0pe 's/Dump BBV at interval boundary: \d+\n//g; s/bbv dump:\d+\n//g' $LOG > $CLEAN; trap 'rm -f $CLEAN' EXIT
grep -a -q "^RUN $w rc=0" $CLEAN || fail "no 'RUN $w rc=0' line"
nint=$(wc -l < $R/bbv/$w.bbv)
say "BBV ok: $nint intervals"
cd $R/sp
$SIMPOINT -loadFVFile $R/bbv/$w.bbv -saveSimpoints $w.points -saveSimpointWeights $w.weights -saveLabels $w.labels \
    -maxK ${MAXK:-30} -seedproj 0721 -seedkm 11037 -numInitSeeds 5 -iters 100 -fixedLength on > $w.sp.log 2>&1 || fail "simpoint failed"
np=$(wc -l < $w.points | tr -d ' ')
say "SimPoint ok: K=$np"
t0=$(date +%s)
rm -f $R/ckpt/$w/*.gz
REF_UNBUF=1 REF_INTERVAL=10000000 REF_MAX_INSTS=2000000000000 $FM --image $R/img/$w.bin --mode ckpt --points $R/sp/$w.points --ckpt-dir $R/ckpt/$w > $R/ckpt/$w.log 2>&1
rc=$?
nck=$(ls $R/ckpt/$w/*.gz 2>/dev/null | wc -l | tr -d ' ')
[ $rc -eq 0 ] || fail "checkpoint run exit code $rc"
[ "$nck" = "$np" ] || say "WARN: $np points, $nck checkpoints (a point at interval 1 has no warm-up interval and is skipped)"
say "DONE: $nck checkpoints, $(du -sh $R/ckpt/$w | cut -f1), $(( $(date +%s) - t0 )) s"
