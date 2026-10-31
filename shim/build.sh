#!/bin/bash
# Build the shim and wrap it in a boot.img that mirrors the stock header.
#   BOARD=sweet (default): header v2, raw Image, dtb section  -> out/shim-boot.img
#   BOARD=fajita:          header v0, Image.gz + appended DTBs -> out-fajita/shim-boot.img
#                          (the OnePlus 6T stock layout; DTBs from dtb/fajita/dtbs,
#                          see tools/extract-appended-dtbs.py)
set -euo pipefail
cd "$(dirname "$0")"
BOARD=${BOARD:-sweet}
CROSS=${CROSS:-aarch64-linux-gnu-}
CC=${CROSS}gcc
OBJCOPY=${CROSS}objcopy
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
case "$BOARD" in
sweet)	OUT=out; BOARDDEF= ;;
fajita)	OUT=out-fajita; BOARDDEF=-DBOARD_FAJITA
	# GPUPROBE=1: the no-payload shim runs the GPU probe (shim/gpuprobe.c)
	if [ -n "${GPUPROBE:-}" ]; then OUT=out-fajita-gpu; BOARDDEF="$BOARDDEF -DSHIM_GPUPROBE"; fi ;;
*)	echo "error: unknown BOARD=$BOARD" >&2; exit 1 ;;
esac
mkdir -p $OUT
for f in start.S shim.c console.c mmu.c fdt.c loader.c inflate.c ufs.c gpuprobe.c; do
	$CC $CFLAGS $BOARDDEF -c $f -o $OUT/${f%.*}.o
done
# GPU probe builds embed the GMU firmware (research/gpu/fw, never in git).
GPUFW_OBJ=
if [ -n "${GPUPROBE:-}" ]; then
	$CC $CFLAGS -I ../research/gpu/fw -c gpufw.S -o $OUT/gpufw.o
	GPUFW_OBJ=$OUT/gpufw.o
fi
# The image runs wherever ABL puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$(${CROSS}readelf -r $OUT/shim.o $OUT/console.o $OUT/mmu.o $OUT/fdt.o $OUT/loader.o $OUT/inflate.o $OUT/ufs.o $OUT/gpuprobe.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	$OUT/start.o $OUT/shim.o $OUT/console.o $OUT/mmu.o $OUT/fdt.o $OUT/loader.o $OUT/inflate.o $OUT/ufs.o $OUT/gpuprobe.o $GPUFW_OBJ -o $OUT/shim.elf
$OBJCOPY -O binary $OUT/shim.elf $OUT/Image
# ABL copies the file; pad to image_size so .bss/stacks exist as zeroed bytes.
size=$(${CROSS}nm $OUT/shim.elf | awk '$3=="_end"{print strtonum("0x"$1)}')
truncate -s "$size" $OUT/Image
# Ramdisk: an xnu payload (tools/mkpayload.py) if given as $1, else an empty
# newc cpio (ABL dislikes a zero-size ramdisk on some builds) -> diagnostics.
if [ -n "${1:-}" ]; then
	cp "$1" $OUT/ramdisk.bin
else
	(cd $OUT && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
fi
if [ "$BOARD" = fajita ]; then
	# Mirrors the boot_a image this ABL boots today (Sailfish, PORTING.md):
	# header v0, base 0x80000000, gzip kernel with the three generic SDM845
	# DTBs (v1/v2/v2.1) appended, no os_version. Reported elsewhere that a v2
	# header or a raw Image is rejected.
	DTBS=${DTBS:-../dtb/fajita/dtbs}
	[ -s "$DTBS" ] || { echo "error: $DTBS missing (stock DTBs, see PORTING.md)" >&2; exit 1; }
	gzip -9 -n -c $OUT/Image > $OUT/Image.gz
	cat $OUT/Image.gz "$DTBS" > $OUT/Image.gz-dtb
	mkbootimg --header_version 0 --kernel $OUT/Image.gz-dtb --ramdisk $OUT/ramdisk.bin \
		--base 0x80000000 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 \
		--tags_offset 0x100 --pagesize 4096 \
		--cmdline "console=null" -o $OUT/shim-boot.img
else
	mkbootimg --header_version 2 --kernel $OUT/Image --ramdisk $OUT/ramdisk.bin \
		--dtb ../dtb/boot_unpacked/dtb --base 0 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 \
		--tags_offset 0x100 --dtb_offset 0x1f00000 --pagesize 4096 \
		--os_version 17.0.0 --os_patch_level 2035-06 \
		--cmdline "console=null" -o $OUT/shim-boot.img
fi
# Host render of the console layout.
gcc -O1 -Wall $BOARDDEF -o $OUT/host_test console.c host_test.c && ./$OUT/host_test $OUT/host.ppm
ls -l $OUT/Image $OUT/shim-boot.img
