#!/usr/bin/env bash
# Starts pipeline.sh for each benchmark once its BBV run has finished (done.txt line with rc=0), at most $MAXJ (default 3)
# at a time.  SKIP="b1 b2" leaves out benchmarks already being handled (e.g. when the driver is restarted).
HERE=$(cd "$(dirname "$0")" && pwd); . "$HERE/env.sh"
R=$SPECDATA
LIST=${*:-$SPEC_ALL}
started=" ${SKIP:-} "
MAXJ=${MAXJ:-3}
while true; do
  left=0
  for b in $LIST; do
    case "$started" in *" $b "*) continue;; esac
    left=1
    line=$(grep -a "^$b " $R/bbv/done.txt 2>/dev/null | tail -1)
    [ -z "$line" ] && continue
    started="$started$b "
    if echo "$line" | grep -q "rc=0"; then
      while [ "$(pgrep -f "pipeline\.sh 4[0-9][0-9]\." | wc -l)" -ge $MAXJ ]; do sleep 20; done   # counts every pipeline, also ones from an earlier driver
      bash "$HERE/pipeline.sh" $b > /dev/null 2>&1 &
    else
      mkdir -p $R/status; echo "$(date '+%F %T') FAIL: BBV run exited with '$line'" > $R/status/$b.txt
    fi
  done
  [ $left = 0 ] && break
  sleep 60
done
wait
while pgrep -f "pipeline\.sh 4[0-9][0-9]\." > /dev/null; do sleep 60; done   # pipelines started by an earlier driver
echo "all pipelines finished" > $R/status/ALL_DONE.txt
