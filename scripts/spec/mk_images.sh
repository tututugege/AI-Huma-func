#!/usr/bin/env bash
# Linux image per benchmark with its ref inputs: initramfs = rootfs + SPEC overlay (mk_testimg.py ref), RAM 0x70000000.
# usage: mk_images.sh [bench ...]     -> $SPECDATA/img/<bench>.{cpio,bin}
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); . "$HERE/env.sh"
L=$LLM4TRACE/linux_rv32
mkdir -p "$SPECDATA/img" "$SPECDATA/stage"
for b in ${@:-$SPEC_ALL}; do
  OUTCPIO=$SPECDATA/img/$b.cpio STAGE=$SPECDATA/stage/${b#*.} python3 -I "$HERE/mk_testimg.py" ref $b | tail -1
  MEM=0x70000000 bash "$MKIMAGE" "$L" "$SPECDATA/img/$b.bin" "$SPECDATA/img/$b.cpio" | tail -1
  rm -rf "$SPECDATA/stage/${b#*.}" "$SPECDATA/stage/${b#*.}.cpio"
done
