#pragma once
/*
 * Per-board constants. build.sh passes -DBOARD_FAJITA for the OnePlus 6T;
 * everything else is sweet (Redmi Note 10 Pro), unchanged.
 *
 * Shared by both boards (SDM845 and SM7150 use the same addresses):
 * GICD 0x17a00000, GICR 0x17a60000 (stride 0x20000), virtual timer INTID 19,
 * PSCI over SMC, MPIDR = core << 8, UFS host 0x1d84000.
 */

#if defined(BOARD_FAJITA)

#define BOARD_NAME		"fajita"
/* Mainline sdm845-oneplus-common.dtsi: cont_splash_mem 0x9d400000 + 36 MiB,
 * simple-framebuffer a8r8g8b8, stride 1080*4; fajita.dts height 2340. */
#define FB_BASE			0x9d400000UL
#define FB_W			1080
#define FB_H			2340
/*
 * Not written on fajita: 0x146bf65c (IMEM 0x146bf000 + 0x65c) is the
 * downstream value, but it is not in the mainline DT and is unverified.
 * Without it a reset boots the active slot; hold Vol Up + Power for fastboot.
 */
#define IMEM_RESTART_REASON	0UL

#else

#define BOARD_NAME		"sweet"
#define FB_BASE			0x9c000000UL	/* reserved-memory cont_splash_region */
#define FB_W			1080
#define FB_H			2400
#define IMEM_RESTART_REASON	0x146aa65cUL	/* qcom,msm-imem restart_reason@65c */

#endif

#define FB_STRIDE		(FB_W * 4)	/* a8r8g8b8, matches mainline simplefb */
