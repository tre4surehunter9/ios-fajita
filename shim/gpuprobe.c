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
 * Stage 2 boots the GMU firmware (a630_gmu.bin, legacy flat image) the way
 * edi194 logged it working on this phone (Bergamota's A630 ladder) plus
 * mainline's BOOT_CONFIG/PWR_COL_CP_RESP: firmware into the ITCM window,
 * HFI/log addresses at 0x60100000/0x60300000 (not mapped yet: the firmware
 * does not touch RAM while it boots), release SYSRESET, expect 0xbabeface.
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

extern const uint8_t gmu_fw_start[], gmu_fw_end[];

/* GMU registers, byte offsets from the GMU base (word index x 4). */
#define GMU_ITCM		0x3000
#define GMU_DTCM_VERSION	0xafe0		/* DTCM_START + 0xff8 words */
#define GMU_SYSRESET		0x14000
#define GMU_BOOT_CONFIG		0x14004
#define GMU_WFI_CONFIG		0x14008
#define GMU_FW_INIT_RESULT	0x14070
#define GMU_CM3_CFG		0x140b4
#define GMU_INTER_FRAME		0x14300
#define GMU_RPMH_CTRL		0x143a0
#define GMU_LOG_ADDR		0x14400		/* PWR_COL_CP_MSG */
#define GMU_CP_RESP		0x14404		/* PWR_COL_CP_RESP */
#define GMU_HFI_SFR		0x14608
#define GMU_HFI_QTBL_INFO	0x14610
#define GMU_HFI_QTBL_ADDR	0x14614
#define GMU_TCM_RET		0x14730		/* GENERAL_7 */
#define GMU_FENCE_RANGE0	0x24c44

static void gmu_set(const char *name, uint32_t off, uint32_t val)
{
	wr32(GMU + off, val);
	con_hex2(name, "off=", off, "val=", rd32(GMU + off));
}

static void gmu_boot(void)
{
	/* Fresh screen: stage 1 plus stage 2 do not fit in one. */
	con_clear(BLUE);
	con_str("GPU", "stage 1 ok: CX on, ITCM writable");
	con_str("GPU", "stage 2: GMU firmware boot");
	/* GPLL0 main + div outputs for the GPU clock controller, then gmu_clk 200 MHz. */
	wr32(GCC + 0x52004, rd32(GCC + 0x52004) | (1u << 15) | (1u << 16));
	uint32_t cfg = rd32(GPUCC + 0x1124);
	wr32(GPUCC + 0x1124, (cfg & ~(0x371fu | (1u << 20))) | (6u << 8) | 2u);	/* GPLL0 div / 1.5 */
	wr32(GPUCC + 0x1120, rd32(GPUCC + 0x1120) | 1u);
	int rc = wait_bits(GPUCC + 0x1120, 1u, 0, 10000);
	con_hex2(rc ? " gmuclk TIMEOUT" : " gmuclk 200M", "vote=", rd32(GCC + 0x52004), "cfg=", rd32(GPUCC + 0x1124));

	uint32_t sysreset = rd32(GMU + GMU_SYSRESET);
	if (sysreset != 1) {
		con_hex2("GPU", "GMU NOT IN RESET, cold boot needed, sysrst=", sysreset, 0, 0);
		return;
	}
	const uint32_t *fw = (const uint32_t *)gmu_fw_start;
	uint32_t words = (uint32_t)(gmu_fw_end - gmu_fw_start) / 4;
	if (words == 0 || words > 0x2000) {
		con_hex2("GPU", "bad firmware size words=", words, 0, 0);
		return;
	}
	con_hex2("GPU", "loading gmu fw words=", words, "first=", fw[0]);
	for (uint32_t i = 0; i < words; i++)
		*(volatile uint32_t *)(GMU + GMU_ITCM + 4 * i) = fw[i];
	__asm__ volatile("dsb sy" ::: "memory");
	uint32_t bad = 0;
	for (uint32_t i = 0; i < words; i += 257)
		bad += rd32(GMU + GMU_ITCM + 4 * i) != fw[i];
	con_hex2(bad ? " FW MISMATCH" : " fw ok", "samples bad=", bad, "dtcm ver=", rd32(GMU + GMU_DTCM_VERSION));

	gmu_set(" tcm ret", GMU_TCM_RET, 1);
	gmu_set(" init res", GMU_FW_INIT_RESULT, 0);
	gmu_set(" boot cfg", GMU_BOOT_CONFIG, 2);
	gmu_set(" hfi sfr", GMU_HFI_SFR, 0x06030100);
	gmu_set(" qtbl inf", GMU_HFI_QTBL_INFO, 1);
	gmu_set(" qtbl adr", GMU_HFI_QTBL_ADDR, 0x60100000);
	gmu_set(" log addr", GMU_LOG_ADDR, 0x60300000);
	gmu_set(" ifr ctrl", GMU_INTER_FRAME, 0x9c40400);
	gmu_set(" rpmh ctl", GMU_RPMH_CTRL, 0xf11);
	gmu_set(" fence", GMU_FENCE_RANGE0, 0x802800a0);
	gmu_set(" cm3 cfg", GMU_CM3_CFG, 0x4052);
	gmu_set(" wfi cfg", GMU_WFI_CONFIG, 0);

	con_str("GPU", "release GMU reset");
	wr32(GMU + GMU_SYSRESET, 1);
	wr32(GMU + GMU_CP_RESP, 0);
	wr32(GMU + GMU_SYSRESET, 0);
	uint32_t res = 0;
	for (unsigned i = 0; i < 200; i++) {		/* up to 200 ms */
		res = rd32(GMU + GMU_FW_INIT_RESULT);
		if (res == 0xbabeface)
			break;
		delay_us(1000);
	}
	con_hex2(res == 0xbabeface ? "GMU RUNS" : "GMU NO", "init=", res, "sysrst=", rd32(GMU + GMU_SYSRESET));
	/* Leave the GMU parked in reset (edi194: a running core cannot be reloaded). */
	wr32(GMU + GMU_SYSRESET, 1);
	con_str("GPU", "stage 2 done, GMU back in reset");
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
	if (got == 0xa5a5f00du)
		gmu_boot();
}
#endif
