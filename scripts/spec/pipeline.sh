#!/usr/bin/env bash
# One benchmark: validate the BBV run -> SimPoint (maxK 100, iters 100) -> checkpoints (warmup = 1 interval).
# usage: pipeline.sh <bench>      status goes to $SPECDATA/status/<bench>.txt
set -u
b=$1
HERE=$(cd "$(dirname "$0")" && pwd); . "$HERE/env.sh"
R=$SPECDATA
SP=$SIMPOINT
LOG=$R/bbv/$b.log
ST=$R/status/$b.txt
mkdir -p $R/status $R/sp $R/ckpt/$b
say() { echo "$(date '+%F %T') $*" | tee -a $ST; }
fail() { say "FAIL: $*"; exit 1; }

# number of runs per benchmark with the ref inputs
expect_runs() { case $1 in 400.perlbench) echo 3;; 401.bzip2) echo 6;; 403.gcc) echo 9;; 429.mcf) echo 1;; 445.gobmk) echo 5;;
  456.hmmer) echo 2;; 458.sjeng) echo 1;; 462.libquantum) echo 1;; 464.h264ref) echo 3;; 471.omnetpp) echo 1;; 473.astar) echo 2;;
  483.xalancbmk) echo 1;; *) echo 0;; esac; }
# outputs that are allowed to differ from SPEC's reference file text (CRLF / last-digit float differences, see linux/README.md)
TOLERATED="462.libquantum 473.astar"

: > $ST
say "start $b"
# ---- 1. validate the BBV run ----
grep -a -q "SBI shutdown requested" $LOG || fail "no SBI shutdown line in the log"
grep -a -q "Simulation Timeout" $LOG && fail "instruction cap reached (truncated BBV)"
grep -a -q "Oops\|Kernel panic" $LOG && fail "kernel Oops/panic in the log"
# the simulator's "Dump BBV at interval boundary: N" / "bbv dump:N" lines can land in the middle of a RUN/CHECK line; strip them first
CLEAN=$(mktemp); perl -0pe 's/Dump BBV at interval boundary: \d+\n//g; s/bbv dump:\d+\n//g' $LOG > $CLEAN; trap 'rm -f $CLEAN' EXIT
nrun=$(grep -a -c "^RUN $b " $CLEAN)
[ "$nrun" = "$(expect_runs $b)" ] || fail "expected $(expect_runs $b) RUN lines, found $nrun"
bad=$(grep -a "^RUN $b " $CLEAN | grep -a -vc "rc=0")
[ "$bad" = "0" ] || fail "$bad runs with a non-zero exit code"
ndiff=$(grep -a -c "^CHECK $b .* DIFF" $CLEAN)
nmatch=$(grep -a -c "^CHECK $b .* MATCH" $CLEAN)
if [ "$ndiff" != "0" ]; then
  case " $TOLERATED " in *" $b "*) say "WARN: $ndiff output file(s) differ textually from the reference (tolerated for $b); $nmatch match";;
    *) fail "$ndiff output file(s) differ from the reference";; esac
else
  say "outputs: $nmatch files match the reference"
fi
nint=$(wc -l < $R/bbv/$b.bbv)
say "BBV ok: $nint intervals, total_sim_time $(grep -a total_sim_time $LOG | tail -1)"

# ---- 2. SimPoint ----
cd $R/sp
$SP -loadFVFile $R/bbv/$b.bbv -saveSimpoints $b.points -saveSimpointWeights $b.weights -saveLabels $b.labels \
    -maxK 100 -seedproj 0721 -seedkm 11037 -numInitSeeds 5 -iters 100 -fixedLength on > $b.sp.log 2>&1 || fail "simpoint failed (see sp/$b.sp.log)"
np=$(wc -l < $b.points)
say "SimPoint ok: K=$np, weights sum $(awk '{s+=$1} END{printf "%.4f", s}' $b.weights)"

# ---- 3. checkpoints (the simulator walks the program once up to the last point) ----
t0=$(date +%s)
rm -f $R/ckpt/$b/*.gz
REF_UNBUF=1 REF_INTERVAL=10000000 REF_MAX_INSTS=20000000000000 $FM --image $R/img/$b.bin --mode ckpt --points $R/sp/$b.points --ckpt-dir $R/ckpt/$b > $R/ckpt/$b.log 2>&1
rc=$?
nck=$(ls $R/ckpt/$b/*.gz 2>/dev/null | wc -l)
[ $rc -eq 0 ] || fail "checkpoint run exit code $rc (see ckpt/$b.log)"
[ "$nck" = "$np" ] || fail "expected $np checkpoints, found $nck"
say "DONE: $nck checkpoints, $(du -sh $R/ckpt/$b | cut -f1), $(( $(date +%s) - t0 )) s"
