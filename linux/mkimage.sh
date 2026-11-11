#!/usr/bin/env bash
# Build a flat DDR image for the function model: OpenSBI at 0x80000000 (the boot stub at 0x0 jumps there with a1 =
# 0x83e00000, which OpenSBI itself reads), the kernel at 0x80400000 and a copy of the devicetree at 0x82200000 (this
# fw_jump build's fixed next-stage address and arg1), the initramfs at 0x84400000.
# Usage: [MEM=0x10000000] mkimage.sh <dir with fw_jump.bin Image rootfs.cpio> <out.bin> [rootfs.cpio]
# KOFF is the kernel offset in the image (default 0x400000: rv32; rv64 uses KOFF=0x200000) and DTS the devicetree source
# (default ttg.dts; rv64: DTS=ttg64.dts).  MEM is the RAM size written into the devicetree (default 256 MiB; the SPEC initramfs needs 0x30000000).
set -euo pipefail
D=$1; OUT=$2; RFS=${3:-$D/rootfs.cpio}
HERE=$(cd "$(dirname "$0")" && pwd)
RSZ=$(stat -f %z "$RFS" 2>/dev/null || stat -c %s "$RFS")
END=$(printf '0x%x' $((0x84400000 + RSZ)))
sed -e "s/linux,initrd-end = <0x[0-9a-f]*>/linux,initrd-end = <$END>/" -e "s/reg = <0x00 0x80000000 0x00 0x[0-9a-f]*>;/reg = <0x00 0x80000000 0x00 ${MEM:-0x10000000}>;/" "$HERE/${DTS:-ttg.dts}" > "$OUT.dts"
dtc -I dts -O dtb -o "$OUT.dtb" "$OUT.dts"
python3 - "$D" "$OUT" "$RFS" <<'PY'
import sys, os
d, out, rfs = sys.argv[1:4]
parts = [(0x0, d + "/fw_jump.bin"), (int(os.environ.get("KOFF", "0x400000"), 0), d + "/Image"), (0x2200000, out + ".dtb"), (0x3e00000, out + ".dtb"), (0x4400000, rfs)]
buf = bytearray()
for off, path in parts:
    data = open(path, "rb").read()
    assert len(buf) <= off, (path, len(buf), off)
    buf += bytes(off - len(buf)) + data
open(out, "wb").write(buf)
print("image:", out, len(buf), "bytes")
PY
