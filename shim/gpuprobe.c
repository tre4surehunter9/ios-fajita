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
 * Stage 3 adds the GPU SMMU (0x5040000) and the HFI queues, per the recipe
 * edi194 recorded working on this chip (Bergamota's A630 stage 4): micro-MMU
 * halt + the vendor's attach-impl-defs, context bank 0 (+0x8000) with our own
 * 4 KiB tables, exact SMRs (mask 0) for the GMU's streams 5 and 4 (S2CR
 * first), then mainline's HFI v1 table (a6xx_hfi.c: 3 queues at +4K/+8K/+12K,
 * legacy ids 0/4/5) and the INIT / FW_VERSION / TEST handshake.
 *
 * No GX-side access yet. A read of an unclocked block hangs the
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

/*
 * Plain [xN] loads and stores only. The hypervisor traps the GPU SMMU's
 * stream table (sCR0 bit 21) and emulates each access from the abort
 * syndrome; a writeback form (`ldr w4, [x20], #4`, which gcc folded into the
 * S2CR read-back) carries no syndrome and the hypervisor resets the SoC.
 */
static inline uint32_t rd32(uintptr_t a)
{
	uint32_t v;
	__asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
	return v;
}
static inline void wr32(uintptr_t a, uint32_t v)
{
	__asm__ volatile("str %w0, [%1]\n\tdsb sy" :: "r"(v), "r"(a) : "memory");
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
	if (rd32(GMU + off) != val)
		con_hex2(name, "MISMATCH off=", off, "got=", rd32(GMU + off));
}

/* ---- stage 3: GPU SMMU + HFI ---- */

#define SMMU		0x05040000UL
#define SMMU_GR1	(SMMU + 0x1000)
#define SMMU_CB0	(SMMU + 0x8000)		/* 8 global 4 KiB pages (IDR1 0x20008005) */
#define POOL		0xb0000000UL		/* 64 KiB, low bank, unused at shim time */
#define POOL_L1		(POOL + 0x0000)
#define POOL_L2		(POOL + 0x1000)
#define POOL_L3A	(POOL + 0x2000)		/* 0x60000000-0x601fffff */
#define POOL_L3B	(POOL + 0x3000)		/* 0x60200000-0x603fffff */
#define POOL_DUMMY	(POOL + 0x4000)
#define POOL_HFI	(POOL + 0x5000)		/* table page + 3 queue pages */
#define POOL_DUMP	(POOL + 0xa000)
#define POOL_LOG	(POOL + 0xe000)
#define VA_HFI		0x60100000u
#define VA_DUMP		0x60200000u
#define PTE_TABLE	3u
#define PTE_PAGE	0xe43u			/* AF, outer shareable, AP, nG, attr 0 (NC) */

static inline void wr64(uintptr_t a, uint64_t v)
{
	__asm__ volatile("str %0, [%1]\n\tdsb sy" :: "r"(v), "r"(a) : "memory");
}

static void map_pages(uintptr_t l3, uint32_t va, uintptr_t pa, unsigned pages)
{
	for (unsigned i = 0; i < pages; i++)
		wr64(l3 + 8 * (((va >> 12) & 0x1ff) + i), (pa + 0x1000 * i) | PTE_PAGE);
}

static int smmu_setup(void)
{
	for (uintptr_t a = POOL; a < POOL + 0x10000; a += 8)
		*(volatile uint64_t *)a = 0;
	wr64(POOL_L1 + 8 * 1, POOL_L2 | PTE_TABLE);		/* VA[31:30] = 1 */
	wr64(POOL_L2 + 8 * 256, POOL_L3A | PTE_TABLE);		/* VA[29:21] = 256 */
	wr64(POOL_L2 + 8 * 257, POOL_L3B | PTE_TABLE);		/* VA[29:21] = 257 */
	map_pages(POOL_L3A, 0x60000000u, POOL_DUMMY, 1);
	map_pages(POOL_L3A, VA_HFI, POOL_HFI, 4);
	map_pages(POOL_L3B, VA_DUMP, POOL_DUMP, 4);
	map_pages(POOL_L3B, 0x60300000u, POOL_LOG, 2);

	/* Vendor qsmmuv2 reset: halt the micro-MMU, write attach-impl-defs, resume. */
	uint32_t ctrl = rd32(SMMU + 0x6000);
	wr32(SMMU + 0x6000, ctrl | (1u << 2));			/* HALT_REQ */
	int rc = wait_bits(SMMU + 0x6000, 1u << 3, 1, 10000);	/* IDLE */
	static const uint32_t impl[][2] = {
		{ 0x6060, 0x1055 }, { 0x678c, 0x8 }, { 0x6794, 0x28 }, { 0x6800, 0x6 },
		{ 0x6900, 0x3ff }, { 0x6924, 0x204 }, { 0x6928, 0x11000 }, { 0x6930, 0x800 },
		{ 0x6960, 0xffffffff }, { 0x6b64, 0x1a5551 }, { 0x6b68, 0x9a82a382 },
	};
	for (unsigned i = 0; i < sizeof(impl) / sizeof(impl[0]); i++)
		wr32(SMMU + impl[i][0], impl[i][1]);
	wr32(SMMU + 0x6000, 0x2378);				/* resume (attach-impl-defs value) */
	con_hex2(rc ? " mmu TIMEOUT" : " micro-mmu", "ctrl=", rd32(SMMU + 0x6000), "scr0=", rd32(SMMU + 0x0));

	/* Context bank 0: stage 1 translate, stage 2 bypass, 64-bit TTBR0. */
	wr32(SMMU_GR1 + 0x0, (1u << 16) | (3u << 8) | (0xfu << 12));	/* CBAR(0) */
	wr32(SMMU_GR1 + 0x800, 1);					/* CBA2R(0) */
	wr32(SMMU_CB0 + 0x4, 0x70000000);				/* ACTLR */
	wr32(SMMU_CB0 + 0x10, 1u | (7u << 15) | (1u << 4));		/* TCR2 */
	wr64(SMMU_CB0 + 0x20, (1ull << 48) | POOL_L1);			/* TTBR0, ASID 1 */
	wr32(SMMU_CB0 + 0x30, (2u << 12) | (1u << 23) | 32u);		/* TCR */
	wr32(SMMU_CB0 + 0x38, 0x0004ff44);				/* MAIR0 */
	wr32(SMMU_CB0 + 0x0, 0x00c01067);				/* SCTLR last */
	con_hex2(" cb0", "sctlr=", rd32(SMMU_CB0 + 0x0), "tcr=", rd32(SMMU_CB0 + 0x30));

	/* Streams 5 (gmu_kernel) and 4 (gmu_user): S2CR first, exact SMR (mask 0). */
	static const uint32_t sid[2] = { 5, 4 };
	int ok = 1;
	for (unsigned i = 0; i < 2; i++) {
		wr32(SMMU + 0xc00 + 4 * i, 0x300);			/* S2CR: translate via CB0 */
		wr32(SMMU + 0x800 + 4 * i, (1u << 31) | sid[i]);	/* SMR: VALID | sid */
		uint32_t smr = rd32(SMMU + 0x800 + 4 * i);
		ok &= smr == ((1u << 31) | sid[i]);
		con_hex2(" smr", "got=", smr, "s2cr=", rd32(SMMU + 0xc00 + 4 * i));
	}
	wr32(SMMU + 0x68, 0);					/* TLBIALLNSNH */
	wr32(SMMU + 0x70, 0);					/* sTLBGSYNC */
	rc = wait_bits(SMMU + 0x74, 1u, 0, 10000);		/* sTLBGSTATUS */
	con_hex2(ok ? " smmu ok" : " SMMU BAD", "gfsr=", rd32(SMMU + 0x48), "tlbsync=", (uint32_t)rc);
	return ok ? 0 : -1;
}

/* HFI v1 table (a6xx_hfi.c layout) in the HFI pages, as the GMU will see it at VA_HFI. */
static void hfi_table(void)
{
	volatile uint32_t *t = (volatile uint32_t *)POOL_HFI;
	t[0] = 0; t[1] = 6 + 3 * 12; t[2] = 6; t[3] = 12; t[4] = 3; t[5] = 3;
	static const uint32_t ids[3] = { 0, 4, 5 };		/* CMD, MSG (legacy 4), DBG (legacy 5) */
	for (unsigned q = 0; q < 3; q++) {
		volatile uint32_t *h = t + 6 + 12 * q;
		h[0] = 1;					/* status */
		h[1] = VA_HFI + 0x1000 * (q + 1);		/* iova */
		h[2] = (10u << 8) | ids[q];			/* type */
		h[3] = 0x1000 >> 2;				/* size in dwords */
		h[4] = 0; h[5] = 0; h[6] = 1; h[7] = 1;		/* msg_size, dropped, rx/tx watermark */
		h[8] = 1; h[9] = 0; h[10] = 0; h[11] = 0;	/* rx/tx request, read/write index */
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

#define GMU_HFI_STATUS		0x14600
#define GMU_HFI_VERSION		0x14604
#define GMU_HFI_INIT		0x14618
#define GMU_G2H_CLR		0x14644
#define GMU_G2H_INFO		0x14648
#define GMU_H2G_SET		0x14650

/* Send one command (a6xx_hfi.c header layout), wait for the MSG queue, return the reply header. */
static uint32_t hfi_cmd(uint32_t id, const uint32_t *payload, uint32_t n, uint32_t *reply)
{
	static uint32_t seq;
	volatile uint32_t *t = (volatile uint32_t *)POOL_HFI;
	volatile uint32_t *cmdh = t + 6, *msgh = t + 6 + 12;
	volatile uint32_t *cmdq = (volatile uint32_t *)(POOL_HFI + 0x1000);
	volatile uint32_t *msgq = (volatile uint32_t *)(POOL_HFI + 0x2000);
	uint32_t w = cmdh[11], dwords = n + 1;

	seq = (seq + 1) % 0xfff;
	cmdq[w] = (seq << 20) | (0u << 16) | (dwords << 8) | id;	/* HFI_MSG_CMD */
	for (uint32_t i = 0; i < n; i++)
		cmdq[(w + 1 + i) % 1024] = payload[i];
	__asm__ volatile("dsb sy" ::: "memory");
	cmdh[11] = (w + dwords) % 1024;
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(GMU + GMU_H2G_SET, 1);

	if (wait_bits(GMU + GMU_G2H_INFO, 1u, 1, 100000))
		return 0;
	wr32(GMU + GMU_G2H_CLR, rd32(GMU + GMU_G2H_INFO));
	uint32_t r = msgh[10];
	if (r == msgh[11])
		return 0;
	uint32_t hdr = msgq[r], size = (hdr >> 8) & 0xff;
	for (uint32_t i = 0; i < 4; i++)
		reply[i] = i < size ? msgq[(r + i) % 1024] : 0;
	msgh[10] = (r + size) % 1024;
	__asm__ volatile("dsb sy" ::: "memory");
	return hdr;
}

/* ---- stage 4: PDC/RSC microcode, GX rail, SPTPRAC, first GPU-core reads ---- */

#define RSCC			(GMU + 0x23000)		/* mainline gmu->rscc; regs are word offsets */
#define PDC			0x0b280000UL		/* sdm845.dtsi gmu "gmu_pdc" */
#define PDC_SEQ			0x0b480000UL		/* "gmu_pdc_seq" */
#define PDC_GFX_ARC		0x30050u		/* gfx.lvl from this phone's cmd-db (edi194;
							   Linux hardcodes 0x30080 for a630) */
#define GX_ARC_IDX		3u			/* 257 MHz boot level: gfx and mx ARC index 3
							   on this phone (edi194, from cmd-db) */
#define GMU_SPTPRAC_CTRL	0x204
#define GMU_BOOT_SLUMBER	0x8fe0
#define GMU_GX_VOTE		0x8fe4
#define GMU_MX_VOTE		0x8fe8
#define GMU_PWR_COUNTER_EN	0x14100
#define GMU_AO_COUNTER_L	0x14220
#define GMU_SPTPRAC_STATUS	0x14340
#define GMU_RPMH_PSTATE		0x143b0
#define GMU_RSCC_REQ		0x24c1c
#define GMU_RSCC_ACK		0x24c20
#define GMU_AO_FENCE_CTRL	0x24c40
#define GPU_CORE		0x05000000UL
#define GPU_RBBM_STATUS		0x840			/* a6xx.xml 0x0210 */
#define GPUCC_GX_GDSCR		0x100c
#define OOB_BOOT_SLUMBER_SET	22
#define OOB_BOOT_SLUMBER_ACK	30

/* a6xx_gmu_rpmh_init (a630 branch): RSC handshake + sequencer, PDC sequencer and TCS1/TCS3. */
static void rpmh_init(void)
{
	wr32(RSCC + 4 * 0x004, 1u << 24);		/* RSC_STATUS0_DRV0: no SDE clock gating */
	wr32(RSCC + 4 * 0x00b, 1);			/* PDC_SLAVE_ID_DRV0 */
	for (unsigned i = 0; i < 3; i++) {		/* HIDDEN_TCS_CMD0 DATA/ADDR, +0/+2/+4 */
		wr32(RSCC + 4 * (0x00e + 2 * i), i == 2 ? 0x80000000u : 0);
		wr32(RSCC + 4 * (0x00d + 2 * i), 0);
	}
	wr32(RSCC + 4 * 0x100, 0);			/* OVERRIDE_START_ADDR */
	wr32(RSCC + 4 * 0x008, 0x4520);			/* PDC_SEQ_START_ADDR */
	wr32(RSCC + 4 * 0x009, 0x4510);			/* PDC_MATCH_VALUE_LO */
	wr32(RSCC + 4 * 0x00a, 0x4514);			/* PDC_MATCH_VALUE_HI */
	static const uint32_t rsc_seq[5] = { 0xa7a506a0, 0xa1e6a6e7, 0xa2e081e1, 0xe9a982e2, 0x0020e8a8 };
	static const uint32_t pdc_seq[5] = { 0xfebea1e1, 0xa5a4a3a2, 0x8382a6e0, 0xbce3e284, 0x002081fc };
	for (unsigned i = 0; i < 5; i++) {
		wr32(RSCC + 4 * (0x180 + i), rsc_seq[i]);	/* SEQ_MEM_0_DRV0 */
		wr32(PDC_SEQ + 4 * i, pdc_seq[i]);
	}
	/* TCS1 (sleep) and TCS3 (wake): mx, cx, gfx ARC votes. */
	static const uint32_t tcs[2] = { 0x1572, 0x15d6 };
	static const uint32_t addr[3] = { 0x30010, 0x30000, PDC_GFX_ARC };
	static const uint32_t data[2][3] = { { 1, 0, 0 }, { 2, 3, 3 } };
	for (unsigned t = 0; t < 2; t++) {
		uintptr_t b = PDC + 4 * tcs[t];		/* CONTROL; ENABLE_BANK +1, WAIT +2, CMD0 +3 */
		wr32(b + 4 * 1, 7);
		wr32(b + 4 * 2, 0);
		wr32(b, 0);
		for (unsigned c = 0; c < 3; c++) {
			wr32(b + 4 * (3 + 3 * c), 0x10108);		/* MSGID */
			wr32(b + 4 * (4 + 3 * c), addr[c]);		/* ADDR */
			wr32(b + 4 * (5 + 3 * c), data[t][c]);		/* DATA */
		}
	}
	wr32(PDC + 4 * 0x1148, 0);			/* GPU_SEQ_START_ADDR */
	wr32(PDC + 4 * 0x1140, 0x80000001);		/* GPU_ENABLE_PDC */
	con_hex2(" pdc/rsc", "pdc en=", rd32(PDC + 4 * 0x1140), "gfx arc=", PDC_GFX_ARC);

	/* a6xx_rpmh_start. edi194 never saw the ACK on this phone and carried on. */
	wr32(GMU + GMU_RSCC_REQ, 1u << 1);
	int ack = wait_bits(GMU + GMU_RSCC_ACK, 1u << 1, 1, 10000);
	int idle = wait_bits(RSCC + 4 * 0x101, ~0u, 0, 10000);	/* SEQ_BUSY_DRV0 */
	wr32(GMU + GMU_RSCC_REQ, 0);
	con_hex2(" rsc wake", "ack=", ack ? 0 : 1, "idle=", idle ? 0 : 1);
}

/* Boot OOB (legacy bits): set, wait for the ack, clear the ack. 0 on success. */
static int oob_boot_slumber(void)
{
	wr32(GMU + GMU_H2G_SET, 1u << OOB_BOOT_SLUMBER_SET);
	int rc = wait_bits(GMU + GMU_G2H_INFO, 1u << OOB_BOOT_SLUMBER_ACK, 1, 10000);
	uint32_t info = rd32(GMU + GMU_G2H_INFO);
	wr32(GMU + GMU_G2H_CLR, 1u << OOB_BOOT_SLUMBER_ACK);
	return rc ? -(int)(info | 1) : 0;
}

static void gx_stage(void)
{
	con_str("GPU", "stage 4: gx rail + sptprac");
	/* a6xx_gmu_gfx_rail_on */
	wr32(GMU + GMU_BOOT_SLUMBER, 0);
	wr32(GMU + GMU_GX_VOTE, GX_ARC_IDX);
	wr32(GMU + GMU_MX_VOTE, GX_ARC_IDX);
	int rc = oob_boot_slumber();
	con_hex2(rc ? " OOB NO ACK" : " gx rail on", "pstate=", rd32(GMU + GMU_RPMH_PSTATE),
	    "sptp=", rd32(GMU + GMU_SPTPRAC_STATUS));
	if (rc)
		return;

	/* a6xx_sptprac_enable */
	wr32(GMU + GMU_SPTPRAC_CTRL, 0x778000);
	int sp = -1;
	for (unsigned i = 0; i < 10000 && sp; i++) {
		if ((rd32(GMU + GMU_SPTPRAC_STATUS) & 0x38) == 0x28)
			sp = 0;
		else
			delay_us(1);
	}
	uint32_t gdscr = rd32(GPUCC + GPUCC_GX_GDSCR);
	con_hex2(sp ? " SPTPRAC FAIL" : " sptprac on", "status=", rd32(GMU + GMU_SPTPRAC_STATUS),
	    "gx gdscr=", gdscr);

	/* A read of a gated core hangs the bus: only with SPTPRAC up and the GX GDSC on. */
	if (!sp && (gdscr & (1u << 31))) {
		uint32_t rbbm = rd32(GPU_CORE + GPU_RBBM_STATUS);
		uint32_t c0 = rd32(GMU + GMU_AO_COUNTER_L);
		delay_us(2000);
		uint32_t c1 = rd32(GMU + GMU_AO_COUNTER_L);
		con_hex2("GPU CORE", "rbbm=", rbbm, "ao/2ms=", c1 - c0);
	} else {
		con_str("GPU", "GX not up: core registers not read");
	}

	/* a6xx_gmu_notify_slumber (legacy): give the rail back. */
	wr32(GMU + GMU_PWR_COUNTER_EN, 0);
	wr32(GPUCC + GPUCC_GX_GDSCR, rd32(GPUCC + GPUCC_GX_GDSCR) | (1u << 11));	/* retention */
	wr32(GMU + GMU_SPTPRAC_CTRL, 0x778001);
	int off = wait_bits(GMU + GMU_SPTPRAC_STATUS, 1u << 2, 1, 10000);
	wr32(GMU + GMU_BOOT_SLUMBER, 1);
	rc = oob_boot_slumber();
	wr32(GMU + GMU_H2G_SET, 1u << OOB_BOOT_SLUMBER_ACK);	/* a6xx_gmu_clear_oob */
	wr32(GMU + GMU_AO_FENCE_CTRL, 0);
	con_hex2(off ? " sptp off FAIL" : " sptp off", "st=", rd32(GMU + GMU_SPTPRAC_STATUS),
	    "gdscr=", rd32(GPUCC + GPUCC_GX_GDSCR));
	con_hex2(rc ? " slumber NOACK" : " slumber ok", "ps=", rd32(GMU + GMU_RPMH_PSTATE),
	    "info=", rd32(GMU + GMU_G2H_INFO));
}

static int hfi_handshake(void)
{
	wr32(GMU + GMU_HFI_INIT, 1);
	int rc = wait_bits(GMU + GMU_HFI_STATUS, 1u, 1, 100000);
	con_hex2(rc ? "HFI NOT UP" : "HFI UP", "status=", rd32(GMU + GMU_HFI_STATUS),
	    "ver=", rd32(GMU + GMU_HFI_VERSION));
	if (rc)
		return -1;
	uint32_t r[4];
	const uint32_t init[4] = { 0, VA_DUMP, 0x4000, 1 };	/* seg, dbg addr/size, COLD_BOOT */
	uint32_t h = hfi_cmd(0, init, 4, r);
	con_hex2(h ? " init ack" : " init NO REPLY", "hdr=", h, "err=", r[2]);
	const uint32_t fwver[1] = { 0x100a0000 };
	h = hfi_cmd(1, fwver, 1, r);
	con_hex2(h ? " fw ver" : " fwver NO REPLY", "hdr=", h, "ver=", r[3]);
	h = hfi_cmd(5, 0, 0, r);
	con_hex2(h ? " test" : " test NO REPLY", "hdr=", h, "data=", r[3]);
	return h ? 0 : -1;
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
	/* Mainline: RSC/PDC microcode at probe, RSC wake before the firmware load. */
	rpmh_init();
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
	/*
	 * Stage 3 with the GMU running. The SMMU sits behind gcc gpu_memnoc_gfx,
	 * which stage 1 left halted; skip rather than touch it if it still is.
	 */
	uint32_t memnoc = rd32(GCC + 0x7100c);
	con_hex2(" memnoc", "cbcr=", memnoc, "snoc=", rd32(GCC + 0x71018));
	if (res == 0xbabeface && (memnoc & CLK_OFF)) {
		con_str("GPU", "memnoc_gfx still halted: SMMU skipped");
	} else if (res == 0xbabeface) {
		con_str("GPU", "stage 3: smmu + hfi table");
		if (smmu_setup() == 0) {
			hfi_table();
			if (hfi_handshake() == 0)
				gx_stage();
		}
	}
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
