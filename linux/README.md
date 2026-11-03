# Booting Linux on the function model

The standalone simulator (`a.out.nospike`, built with `make no_spike`) boots a flat DDR image: the boot stub at 0x0 jumps to
0x80000000 with `a1 = 0x83e00000`.

- `ttg.dts`: devicetree for this platform (256 MiB RAM at 0x80000000, ns16550a UART at 0x10000000, polled; an ACLINT
  mtimer at 0x1fd0e000 with mtime at +0 and mtimecmp at +8; no PLIC; rv32ima; timebase 132 MHz).
- `mkimage.sh <dir> <out.bin> [rootfs.cpio]`: builds the image from `fw_jump.bin`, `Image` and the initramfs of a
  linux_rv32 directory: OpenSBI at 0x80000000, kernel at 0x80400000, devicetree at 0x82200000 and 0x83e00000,
  initramfs at 0x84400000. `MEM=0x30000000` sets a larger RAM for big initramfs images (the SPEC test image needs it).

```bash
make no_spike
MEM=0x10000000 bash linux/mkimage.sh ../../llm4trace/linux_rv32 /tmp/linux.bin
REF_UNBUF=1 ./a.out.nospike --image /tmp/linux.bin     # boots to "rv32 login:" in about 0.12 G instructions
```

Notes
- The timebase must stay around 132 MHz: `mtime` advances by one per executed instruction, and a faster timebase makes
  the tick handler (each rdtime is emulated by OpenSBI, about 340 instructions) outlast the tick period.
- `time` is not a hardware CSR here: S-mode `rdtime` traps and OpenSBI emulates it.
- A guest `poweroff` ends the simulation (SBI shutdown ecall from S mode).
- Verified: Linux 5.15.170 boots; the 12 SPECint2006 `test` programs run with matching output (libquantum differs only
  by CRLF line ends, astar by about 1e-4 in two numbers, within SPEC's tolerance).
- Debug options: `REF_PC_EVERY`, `REF_PC_RING`, `REF_TRAP_TRACE`, `REF_TRAP_EVERY`, `REF_UNBUF`.
