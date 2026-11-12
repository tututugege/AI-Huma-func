#!/usr/bin/env bash
# Linux images for the non-SPEC workloads (GAPBS kernels, PolyBench/C 4.2.1 int LARGE), one per workload.
# Binaries/graphs were cross-built in the Lima VM rvbuild (GCC 13.3, rv32imab, static) into $WLDATA/bin and $WLDATA/*.sg.
# usage: mk_wl.sh [workload ...]      -> $WLDATA/img/<wl>.bin ; RUN line "RUN <wl> rc=N secs=S" in the console log
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); . "$HERE/../spec/env.sh"
WLDATA=${WLDATA:-$HOME/personal/AI-Huma-data/wl-rv32imab}
L=$LLM4TRACE/linux_rv32
# workload -> "binary|args|graph file"
cmd() { case $1 in
  gap_bfs)  echo "gap_bfs|-f g20.sg -n 32|g20.sg";;
  gap_pr)   echo "gap_pr|-f g20.sg -n 1 -i 20|g20.sg";;
  gap_cc)   echo "gap_cc|-f g20.sg -n 16|g20.sg";;
  gap_bc)   echo "gap_bc|-f g20.sg -n 1|g20.sg";;
  gap_sssp) echo "gap_sssp|-f g20.wsg -n 8 -d 2|g20.wsg";;
  gap_tc)   echo "gap_tc|-f g18.sg -n 1|g18.sg";;
  gap_tcu)  echo "gap_tc|-u 19 -n 1|";;   # EXP-022 merge workloads: tc on a uniform random graph
  mg_*|stl_*|eigen_*) echo "$1||";;
  ligra_tc) echo "ligra_tc|-s -b -rounds 6 rmatb|ligra/rmatb.adj ligra/rmatb.idx ligra/rmatb.config";;   # EXP-022: Ligra Triangle, RMAT 2^18                 # EXP-022 merge workloads (src in AI-Huma-data/src/merge_wl)
  pb_*)     echo "$1||";;
  ol_bh)        echo "ol_bh|20000 20|";;
  ol_bisort)    echo "ol_bisort|700000|";;
  ol_em3d)      echo "ol_em3d|1024 1000 125|";;
  ol_health)    echo "ol_health|9 20 1|";;
  ol_mst)       echo "ol_mst|1000|";;
  ol_perimeter) echo "ol_perimeter|10|";;
  ol_power)     echo "ol_power||";;
  ol_treeadd)   echo "ol_treeadd|22|";;
  ol_tsp)       echo "ol_tsp|1024000|";;
  ol_voronoi)   echo "ol_voronoi|1000000 20 32 7|";;
  *) echo "unknown workload $1" >&2; exit 1;;
esac; }
ALL="gap_bfs gap_pr gap_cc gap_bc gap_sssp gap_tc $(cd $WLDATA/bin && ls pb_* | tr '\n' ' ')"
mkdir -p "$WLDATA/img" "$WLDATA/stage"
for w in ${@:-$ALL}; do
  IFS='|' read -r exe args g <<< "$(cmd $w)"
  S=$WLDATA/stage/$w; rm -rf "$S"; mkdir -p "$S/wl" "$S/etc/init.d"
  cp "$WLDATA/bin/$exe" "$S/wl/"; chmod 755 "$S/wl/$exe"
  for x in $g; do cp "$WLDATA/$x" "$S/wl/"; done
  cat > "$S/wl/run.sh" <<EOF
#!/bin/sh
echo '=== WL run start ==='
cd /wl
t0=\$(cut -d. -f1 /proc/uptime)
./$exe $args > out.txt 2>&1; rc=\$?
t1=\$(cut -d. -f1 /proc/uptime)
tail -5 out.txt
echo "RUN $w rc=\$rc secs=\$((t1-t0))"
sync; sleep 1
echo '=== WL run done ==='
EOF
  chmod 755 "$S/wl/run.sh"
  printf '#!/bin/sh\ncase "$1" in start)\n  /wl/run.sh > /dev/console 2>&1\n  poweroff -f ;;\nesac\n' > "$S/etc/init.d/S99wl"
  chmod 755 "$S/etc/init.d/S99wl"
  bsdtar --format newc --uid 0 --gid 0 -cf "$S.cpio" -C "$S" .
  cat "$L/rootfs.cpio" "$S.cpio" > "$WLDATA/img/$w.cpio"
  MEM=0x70000000 bash "$MKIMAGE" "$L" "$WLDATA/img/$w.bin" "$WLDATA/img/$w.cpio" | tail -1
  rm -rf "$S" "$S.cpio"
done
