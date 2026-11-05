#!/usr/bin/env bash
# BBV of every benchmark (interval 10M instructions), all in parallel.  -> $SPECDATA/bbv/<bench>.{bbv,log}, bbv/done.txt
# usage: run_bbv.sh [bench ...]
HERE=$(cd "$(dirname "$0")" && pwd); . "$HERE/env.sh"
mkdir -p "$SPECDATA/bbv"
one() {
  b=$1; s=$(date +%s)
  REF_UNBUF=1 REF_INTERVAL=10000000 "$FM" --mode bbv --image "$SPECDATA/img/$b.bin" --out-bbv "$SPECDATA/bbv/$b.bbv" > "$SPECDATA/bbv/$b.log" 2>&1
  echo "$b rc=$? secs=$(( $(date +%s) - s ))" >> "$SPECDATA/bbv/done.txt"
}
export -f one
rm -f "$SPECDATA/bbv/done.txt"
printf '%s\n' ${@:-$SPEC_ALL} | xargs -P12 -I{} bash -c 'one {}'
