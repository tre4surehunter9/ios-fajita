/*
 * fajita (Adreno 630) GPU probe, stage 1, built with -DSHIM_GPUPROBE into the
 * no-payload shim: powers only the GPU's CX side and shows what the GMU does.
 * Modelled on ios-flame's shim/gpuprobe.c (Adreno 640, MIT); every SDM845
 * value comes from mainline (gcc-sdm845.c, gpucc-sdm845.c, sdm845.dtsi) and
 * from the stage-1 measurements in edi194/ios-fajita's notes:
 *
 *   GCC   gpu_cfg_ahb 0x71004, ddrss_gpu_axi 0x44038 (not 0x71154 as on
 *         SM8150), gpu_memnoc_gfx 0x7100c, gpu_snoc_dvm_gfx 0x71018
 *   GPUCC 0x5090000: CX GDSC 0x106c (ABL leaves it collapsed, 0x222001),
 *         gmu_clk_src RCG 0x1120 kept at XO here, cxo 0x109c, cx_gmu 0x1098,
 *         ahb 0x1078
 *   GMU   0x506a000, registers indexed in words from the GMU base:
 *         ITCM +0x3000, SYSRESET +0x14000, FW_INIT_RESULT +0x14070
 *
 * No SMMU and no GX-side access yet. A read of an unclocked block hangs the
 * bus, so every step prints first: the last line on screen says where it
 * stopped.
 */
#if defined(BOARD_FAJITA) && defined(SHIM_GPUPROBE)
#include <stdint.h>
#include "console.h"
#include "gpuprobe.h"

#define GCC		0x00100000UL
#define GPUCC		0x05090000UL
#define GMU		0x0506a000UL

#define CLK_OFF		(1u << 31)

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v)
{
	*(volatile uint32_t *)a = v;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void delay_us(unsigned us)
{
	uint64_t f, t0, t;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t0));
	do {
		__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t));
	} while (t - t0 < f / 1000000 * us);
}

/* Poll until (reg & mask) is set (want != 0) or clear; returns 0 on success. */
static int wait_bits(uintptr_t reg, uint32_t mask, int want, unsigned us)
{
	for (unsigned i = 0; i <= us; i++) {
		if (!!(rd32(reg) & mask) == !!want)
			return 0;
		delay_us(1);
	}
	return -1;
}

static void branch_on(const char *name, uintptr_t reg)
{
	con_str("GPU", name);
	wr32(reg, rd32(reg) | 1u);
	int rc = wait_bits(reg, CLK_OFF, 0, 10000);
	con_hex2(rc ? " HALTED" : " on", "reg=", (uint32_t)reg, "val=", rd32(reg));
}

void gpuprobe_run(void)
{
	con_str("GPU", "stage 1: CX power (A630)");
	con_hex2("before", "cx=", rd32(GPUCC + 0x106c), "gx=", rd32(GPUCC + 0x100c));
	con_hex2(" gmurcg", "cmd=", rd32(GPUCC + 0x1120), "cfg=", rd32(GPUCC + 0x1124));
	con_hex2(" cxgmu", "=", rd32(GPUCC + 0x1098), "cxo=", rd32(GPUCC + 0x109c));
	con_hex2(" gcc", "ahb=", rd32(GCC + 0x71004), "axi=", rd32(GCC + 0x44038));

	branch_on("gcc gpu_cfg_ahb", GCC + 0x71004);
	branch_on("gcc ddrss_gpu_axi", GCC + 0x44038);
	branch_on("gcc gpu_memnoc_gfx", GCC + 0x7100c);
	branch_on("gcc gpu_snoc_dvm_gfx", GCC + 0x71018);

	con_str("GPU", "cx gdsc on");
	wr32(GPUCC + 0x106c, rd32(GPUCC + 0x106c) & ~1u);		/* SW_COLLAPSE off */
	int rc = wait_bits(GPUCC + 0x106c, 1u << 31, 1, 10000);	/* PWR_ON */
	con_hex2(rc ? " TIMEOUT" : " on", "cx=", rd32(GPUCC + 0x106c), "hwctl=", rd32(GPUCC + 0x1540));

	con_str("GPU", "gmu_clk_src = XO");
	uint32_t cfg = rd32(GPUCC + 0x1124);
	wr32(GPUCC + 0x1124, (cfg & ~(0x371fu | (1u << 20))) | 1u);	/* src XO, div 1 */
	wr32(GPUCC + 0x1120, rd32(GPUCC + 0x1120) | 1u);			/* UPDATE */
	rc = wait_bits(GPUCC + 0x1120, 1u, 0, 10000);
	con_hex2(rc ? " TIMEOUT" : " ok", "cmd=", rd32(GPUCC + 0x1120), "cfg=", rd32(GPUCC + 0x1124));

	/* cx_gmu: recommended wake/sleep counts (gpucc-sdm845.c probe), then enable. */
	wr32(GPUCC + 0x1098, (rd32(GPUCC + 0x1098) & ~(0xffu << 8)) | (0xfu << 12) | (0xfu << 8));
	branch_on("gpucc cxo", GPUCC + 0x109c);
	branch_on("gpucc cx_gmu", GPUCC + 0x1098);
	branch_on("gpucc ahb", GPUCC + 0x1078);

	con_str("GPU", "gmu reads");
	con_hex2(" gmu", "sysrst=", rd32(GMU + 0x14000), "init=", rd32(GMU + 0x14070));

	con_str("GPU", "itcm write test");
	uint32_t old = rd32(GMU + 0x3000);
	wr32(GMU + 0x3000, 0xa5a5f00du);
	uint32_t got = rd32(GMU + 0x3000);
	wr32(GMU + 0x3000, old);
	con_hex2(got == 0xa5a5f00du ? " ITCM OK" : " ITCM BAD", "old=", old, "got=", got);
	con_str("GPU", "stage 1 done");
}
#endif
