/*
 * GPU firmware for the fajita GPU probe (SHIM_GPUPROBE builds only).
 * build.sh assembles this with -I ../research/gpu/fw: the firmware never
 * enters git (see GPU-PLAN.md for its source and checksum).
 */
	.section .rodata
	.balign	8
	.globl	gmu_fw_start, gmu_fw_end
gmu_fw_start:
	.incbin	"a630_gmu.bin"
gmu_fw_end:
