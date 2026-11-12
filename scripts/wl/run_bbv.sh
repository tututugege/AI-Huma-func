#!/usr/bin/env bash
# BBV (interval 10M) of the non-SPEC workloads, P in parallel.  -> $WLDATA/bbv/<wl>.{bbv,log}, bbv/done.txt
# usage: run_bbv.sh [wl ...]
HERE=$(cd "$(dirname "$0")" && pwd); . "$HERE/../spec/env.sh"
export WLDATA=${WLDATA:-$HOME/personal/AI-Huma-data/wl-rv32imab} FM
mkdir -p "$WLDATA/bbv"
one() {
  w=$1; s=$(date +%s)
  REF_UNBUF=1 REF_INTERVAL=10000000 REF_MAX_INSTS=2000000000000 "$FM" --mode bbv --image "$WLDATA/img/$w.bin" --out-bbv "$WLDATA/bbv/$w.bbv" > "$WLDATA/bbv/$w.log" 2>&1
  echo "$w rc=$? secs=$(( $(date +%s) - s ))" >> "$WLDATA/bbv/done.txt"
}
export -f one
WL=${@:-$(cd "$WLDATA/img" && ls *.bin | sed 's/\.bin$//')}
printf '%s\n' $WL | xargs -P${P:-8} -n1 bash -c 'one "$0"'
