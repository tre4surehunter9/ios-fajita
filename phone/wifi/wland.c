/*
 * ios-fajita: from ios-flame (MIT, NWylynko/ios-flame phone/wifi), ported to the OnePlus 6T (SDM845).
 * SDM845 changes are marked "SDM845:" with their mainline source (sdm845.dtsi, sdm845-oneplus-common.dtsi).
 *
 * wland: WLAN bring-up for the Pixel 4 from userland (/dev/pmem), the way
 * the GPU was brought up: the modem DSP hosts the WLAN firmware (icnss /
 * WCN3990 family), so: power votes, PAS-boot the modem, SMP2P, then GLINK +
 * QRTR + QMI to the WLFW service, then the WLAN copy engines for WMI.
 *
 *   wland smem              SMEM partitions and the items we care about
 *   wland smp2p             modem SMP2P state (in: 435, out: 428)
 *   wland vote RES LVL      RPMh ARC level vote (index) for RES (e.g. mss.lvl)
 *   wland levels RES        cmd-db levels of an ARC resource
 *   wland bcm RES X [Y]     RPMh BCM (bus bandwidth) vote, e.g. MC0 (DDR) / SH0 (memory NoC); 0x3fff = top
 *   wland smp2p-init        our outbound SMP2P item + doorbell; shows the modem's entries
 *   wland qmp MSG           message to the AOP over QMP (opens the link if needed)
 *   wland shutdown          PAS shutdown of the modem
 *   wland boot [META_PA]    power votes + PAS boot of the modem + wait for ready (META_PA: metadata there, fajita)
 *   wland boot-mba          fajita: the modem through MBA (Linux qcom_q6v5_mss.c sdm845), then wait for ready
 *   wland smmu              the WLAN DMA stream through a translation-off SMMU bank (radio.c)
 *   wland up [SECS]         all of it from a fresh boot, then the radio: copy engines, WMI, passive scans
 *   wland auto              the boot job (launchd org.iosonpixel.wland): `up 0` once per phone boot, log in
 *                           /var/local/wland.log (last boot's: .prev); off with /var/local/wland.off or sweetwifi=0
 * While joined, SIGUSR1 drops the link (tests the rejoin).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include "pmem.h"
#include "smem.h"
#include "scm.h"
#include "link.h"
#include "rmtfs.h"
#include "pdmapper.h"
#include "tftp.h"
#include "wlfw.h"
#include "radio.h"
#include "ipnet.h"
#include "bridge.h"

/* cmd-db aux levels (as in the GPU's arc votes) */
/* Every cmd-db entry: name, RPMh slave (3 ARC, 4 VRM, 5 BCM), address. */
static void cmddb_list(void)
{
	uint64_t db = rd32(0x0c3f000cull);
	if (rd32(db + 4) != 0x0c0330db)
		return;
	uint64_t data = db + 4 + 4 + 8 * 16 + 4 + 4;
	for (uint32_t r = 0; r < 8; r++) {
		uint64_t rh = db + 8 + 16 * r;
		uint32_t w0 = rd32(rh), w1 = rd32(rh + 4);
		uint32_t hoff = w0 >> 16, cnt = w1 >> 16;
		for (uint32_t e = 0; e < cnt; e++) {
			uint64_t ent = data + hoff + 24 * e;
			char name[9];
			for (uint32_t i = 0; i < 8; i++)
				name[i] = (char)(rd32(ent + (i & ~3u)) >> (8 * (i & 3)));
			name[8] = 0;
			printf("%-8s slave %u addr %05x\n", name, w0 & 0xffff, rd32(ent + 16));
		}
	}
}

static int cmddb_aux(const char *id, uint64_t *aux, uint32_t *len)
{
	uint64_t db = rd32(0x0c3f000cull);
	if (rd32(db + 4) != 0x0c0330db)
		return -1;
	uint64_t data = db + 4 + 4 + 8 * 16 + 4 + 4;
	for (uint32_t r = 0; r < 8; r++) {
		uint64_t rh = db + 8 + 16 * r;
		uint32_t w0 = rd32(rh), w1 = rd32(rh + 4);
		uint32_t hoff = w0 >> 16, doff = w1 & 0xffff, cnt = w1 >> 16;
		for (uint32_t e = 0; e < cnt; e++) {
			uint64_t ent = data + hoff + 24 * e;
			char name[9];
			for (uint32_t i = 0; i < 8; i++)
				name[i] = (char)(rd32(ent + (i & ~3u)) >> (8 * (i & 3)));
			name[8] = 0;
			if (strncmp(name, id, 8))
				continue;
			uint32_t w5 = rd32(ent + 20);
			*len = w5 & 0xffff;
			*aux = data + doff + (w5 >> 16);
			return 0;
		}
	}
	return -1;
}

static uint32_t aux16(uint64_t aux, uint32_t i)
{
	return (rd32(aux + 2 * (i & ~1u)) >> (16 * (i & 1))) & 0xffff;
}

static int levels(const char *res, int print)
{
	uint64_t aux;
	uint32_t len;
	if (cmddb_aux(res, &aux, &len)) {
		fprintf(stderr, "%s: not in cmd-db\n", res);
		return -1;
	}
	if (print) {
		printf("%s:", res);
		for (uint32_t i = 0; i < len / 2; i++)
			printf(" [%u]=%u", i, aux16(aux, i));
		printf("\n");
	}
	return (int)(len / 2);
}

static int vote(const char *res, uint32_t level_index)
{
	uint32_t off[1] = { 0 }, val[1] = { level_index };
	int rc = pm_rpmh(res, 1, off, val);
	printf("vote %s level index %u -> %d\n", res, level_index, rc);
	return rc;
}

/*
 * The WLAN supplies (icnss in the stock DT: vdd-cx-mx, vdd-1.8-xo, vdd-1.3-rfa, vdd-3.3-ch0), RPMh VRM
 * votes as Linux qcom-rpmh-regulator: voltage (mV) at 0, enable at 4, mode at 8 (PMIC5 LDO HPM = 7).
 * Without them the WLAN firmware takes an exception once calibration starts (after the cal report).
 */
static int qmp_send(const char *msg);

static int wlan_power(void)
{
	/* SDM845 (fajita &wifi): vdd-0.8-cx-mx l5a 800, vdd-1.8-xo l7a 1800, vdd-1.3-rfa l17a 1304,
	 * vdd-3.3-ch0 l25a and vdd-3.3-ch1 l23a 3312 (max of their DT ranges). */
	static const struct { const char *res; uint32_t mv; } ldo[] = {
		{ "ldoa5", 800 }, { "ldoa7", 1800 }, { "ldoa17", 1304 }, { "ldoa25", 3312 }, { "ldoa23", 3312 },
	};
	for (unsigned i = 0; i < sizeof(ldo) / sizeof(ldo[0]); i++) {
		uint32_t off[3] = { 0, 8, 4 }, val[3] = { ldo[i].mv, 7, 1 };
		int rc = pm_rpmh(ldo[i].res, 3, off, val);
		printf("wlan power: %s %u mV on -> %d\n", ldo[i].res, ldo[i].mv, rc);
		if (rc)
			return -1;
	}
	/* the WCN399x reference clock (mainline: clocks = <&rpmhcc RPMH_RF_CLK2>, "cxo_ref_clk_pin"): VRM enable */
	uint32_t off[1] = { 4 }, val[1] = { 1 };
	int rc = pm_rpmh("rfclka2", 1, off, val);
	printf("wlan power: rfclka2 on -> %d\n", rc);
	if (rc)
		return -1;
	/* mainline sm8150 wifi: clocks "cxo_ref_clk_pin", "qdss" = <&aoss_qmp>: the QDSS clock is an AOSS message.
	 * SDM845's wifi node has no qdss clock; ask anyway (it cost ios-flame a firmware exception), but a
	 * refusal is not fatal here. */
	if (qmp_send("{class: clock, res: qdss, val: 1}"))
		printf("wlan power: qdss clock message not acked (SDM845 does not list it); going on\n");
	return 0;
}

/* SMP2P item: magic "$SMP", version/features, local/remote pid, total/valid entries, flags, entries[16+4] */
static void smp2p_show(const char *what, unsigned item)
{
	uint64_t pa;
	uint32_t sz;
	if (smem_get(SMEM_HOST_MODEM, item, &pa, &sz)) {
		printf("smp2p %s (item %u): absent\n", what, item);
		return;
	}
	uint32_t w1 = rd32(pa + 4), pids = rd32(pa + 8), counts = rd32(pa + 12);
	printf("smp2p %s (item %u) at %llx: magic %08x ver %u feat %x pids %u->%u entries %u/%u flags %x\n", what, item,
	       (unsigned long long)pa, rd32(pa), w1 & 0xff, w1 >> 8, pids & 0xffff, pids >> 16, counts >> 16,
	       counts & 0xffff, rd32(pa + 16));
	for (uint32_t i = 0; i < (counts >> 16) && i < 16; i++) {
		uint64_t e = pa + 20 + 20ull * i;
		char name[17];
		for (int k = 0; k < 16; k++)
			name[k] = (char)(rd32(e + (k & ~3)) >> (8 * (k & 3)));
		name[16] = 0;
		printf("  entry %u: %-16s value %08x\n", i, name, rd32(e + 16));
	}
}

/* ---- SMP2P outbound (Linux qcom_smp2p_alloc_outbound_item) and the APCS doorbell ---- */

#define APCS_IPC	0x1799000cull	/* SDM845: apss_shared mailbox@17990000 + 12: write BIT(n) (SM8150: 0x17c0000c) */
#define IPC_MODEM_SMP2P	14
#define IPC_MODEM_GLINK	12
#define SMP2P_OUT	428
#define SMP2P_IN	435

static void kick(unsigned bit)
{
	wr32(APCS_IPC, 1u << bit);
}

static void wr_name(uint64_t pa, const char *name)
{
	char n[16] = { 0 };
	strncpy(n, name, sizeof(n));
	for (int i = 0; i < 16; i += 4) {
		uint32_t w;
		memcpy(&w, n + i, 4);
		wr32(pa + i, w);
	}
}

static uint64_t smp2p_out_pa;

/* Our outbound item (Linux allocates it at probe, before the modem boots: the modem needs it). */
static int smp2p_create(void)
{
	uint64_t pa;
	if (smem_alloc(SMEM_HOST_MODEM, SMP2P_OUT, 20 + 16 * 20, &pa)) {
		fprintf(stderr, "smp2p: cannot allocate item %u\n", SMP2P_OUT);
		return -1;
	}
	if (rd32(pa) != 0x504d5324) {
		wr32(pa + 8, 0 | (1u << 16));		/* local pid 0, remote pid 1 */
		wr32(pa + 12, 16 | (1u << 16));		/* total 16 entries, 1 valid */
		wr32(pa + 16, 0);			/* flags */
		wr_name(pa + 20, "master-kernel");
		wr32(pa + 36, 0);
		wr32(pa + 0, 0x504d5324);		/* "$SMP" */
		__asm__ volatile("dsb sy" ::: "memory");
		wr32(pa + 4, 1 | (1u << 8));		/* version 1 last, features: SSR ack */
	}
	smp2p_out_pa = pa;
	kick(IPC_MODEM_SMP2P);
	return 0;
}

/* Address of the value of the modem's inbound entry NAME (0 = found). */
static int smp2p_in_entry(const char *name, uint64_t *va)
{
	uint64_t pa;
	uint32_t sz;
	if (smem_get(SMEM_HOST_MODEM, SMP2P_IN, &pa, &sz))
		return -1;
	uint32_t n = rd32(pa + 12) >> 16;
	for (uint32_t i = 0; i < n && i < 16; i++) {
		uint64_t e = pa + 20 + 20ull * i;
		char nm[17];
		for (int k = 0; k < 16; k++)
			nm[k] = (char)(rd32(e + (k & ~3)) >> (8 * (k & 3)));
		nm[16] = 0;
		if (!strcmp(nm, name)) {
			*va = e + 16;
			return 0;
		}
	}
	return -1;
}

static int smp2p_in_value(const char *name, uint32_t *v)
{
	uint64_t va;
	if (smp2p_in_entry(name, &va))
		return -1;
	*v = rd32(va);
	return 0;
}

/*
 * Keep the SMP2P side going: SSR ack (Linux qcom_smp2p_do_ssr_ack: after a
 * restart the modem flips RESTART_DONE, flags bit 0, and waits for the same
 * in our RESTART_ACK, bit 1), and report changes of the modem's state bits
 * (slave-kernel: 0 err-fatal, 1 ready, 2 handover, 3 stop-ack).
 */
static uint32_t modem_state = 0xffffffff;
static void smp2p_service(void)
{
	uint64_t ip;
	uint32_t sz, v;
	if (!smp2p_out_pa && smem_get(SMEM_HOST_MODEM, SMP2P_OUT, &smp2p_out_pa, &sz))
		return;
	if (smem_get(SMEM_HOST_MODEM, SMP2P_IN, &ip, &sz) == 0) {
		uint32_t want = (rd32(ip + 16) & 1) ? 2 : 0;
		if ((rd32(smp2p_out_pa + 16) & 2) != want) {
			wr32(smp2p_out_pa + 16, (rd32(smp2p_out_pa + 16) & ~2u) | want);
			kick(IPC_MODEM_SMP2P);
			printf("smp2p: restart %s\n", want ? "acked" : "ack cleared");
		}
	}
	if (smp2p_in_value("slave-kernel", &v) == 0 && v != modem_state) {
		printf("[%6llu] smp2p: modem state %x%s%s%s%s\n", (unsigned long long)link_ms(), v, v & 1 ? " ERR-FATAL" : "", v & 2 ? " ready" : "",
		       v & 4 ? " handover" : "", v & 8 ? " stop-ack" : "");
		modem_state = v;
	}
}

static int smp2p_init(void)
{
	if (smp2p_create())
		return -1;
	printf("smp2p: outbound item %u at %llx ready, modem kicked\n", SMP2P_OUT, (unsigned long long)smp2p_out_pa);
	for (int t = 0; t < 50; t++) {
		smp2p_service();
		usleep(100000);
	}
	smp2p_show("in", SMP2P_IN);
	return 0;
}

/* ---- AOSS QMP (Linux qcom_aoss.c): messages to the always-on processor ---- */

#define QMP_RAM		0x0c300000ull
#define IPC_AOSS	0
#define QMP_UP		0x0000ffffu

static int wait_eq(uint64_t pa, uint32_t want, int ms)
{
	for (int i = 0; i < ms; i++) {
		if (rd32(pa) == want)
			return 0;
		usleep(1000);
	}
	return -1;
}

static int qmp_open(void)
{
	if (rd32(QMP_RAM) != 0x4d41494c || rd32(QMP_RAM + 4) != 1) {
		fprintf(stderr, "qmp: magic %08x version %u\n", rd32(QMP_RAM), rd32(QMP_RAM + 4));
		return -1;
	}
	if (rd32(QMP_RAM + 0x24) == QMP_UP && rd32(QMP_RAM + 0x2c) == QMP_UP && rd32(QMP_RAM + 0x30) == QMP_UP)
		return 0;	/* already open */
	wr32(QMP_RAM + 0x10, rd32(QMP_RAM + 0x0c));	/* ack ucore link state */
	wr32(QMP_RAM + 0x24, QMP_UP);			/* mcore link up */
	kick(IPC_AOSS);
	if (wait_eq(QMP_RAM + 0x28, QMP_UP, 1000)) {
		fprintf(stderr, "qmp: link not acked\n");
		return -1;
	}
	wr32(QMP_RAM + 0x2c, QMP_UP);			/* mcore channel up */
	kick(IPC_AOSS);
	if (wait_eq(QMP_RAM + 0x14, QMP_UP, 1000)) {
		fprintf(stderr, "qmp: ucore channel not up\n");
		return -1;
	}
	wr32(QMP_RAM + 0x18, QMP_UP);			/* ack ucore channel */
	kick(IPC_AOSS);
	if (wait_eq(QMP_RAM + 0x30, QMP_UP, 1000)) {
		fprintf(stderr, "qmp: mcore channel not acked\n");
		return -1;
	}
	return 0;
}

static int qmp_send(const char *msg)
{
	if (qmp_open())
		return -1;
	uint32_t off = rd32(QMP_RAM + 0x38), buf[16] = { 0 };
	strncpy((char *)buf, msg, sizeof(buf) - 1);
	for (int i = 0; i < 16; i++)
		wr32(QMP_RAM + off + 4 + 4 * i, buf[i]);
	wr32(QMP_RAM + off, sizeof(buf));
	(void)rd32(QMP_RAM + off);
	kick(IPC_AOSS);
	int rc = wait_eq(QMP_RAM + off, 0, 1000);
	printf("qmp: \"%s\" -> %s\n", msg, rc ? "TIMEOUT" : "acked");
	return rc;
}

/* ---- modem boot: TrustZone PAS (Linux qcom_q6v5_pas + qcom_mdt_loader) ---- */

#define PAS_MODEM	4
#define MPSS_PA		0x8e000000ull	/* SDM845: mpss_region (flame: 0x8dc00000) */
#define MPSS_SIZE	0x07800000ull	/* SDM845: 120 MB (flame: 0x0b400000) */
#define FW_DIR		"/var/local/fw/modem"
#define SCM_PIL(cmd)	(0x42000000ull | (2u << 8) | (cmd))

static int64_t scm(uint32_t cmd, uint64_t arginfo, uint64_t a, uint64_t b, uint64_t c, uint64_t *r1)
{
	uint64_t x[8] = { SCM_PIL(cmd), arginfo, a, b, c, 0, 0, 0 };
	pm_smc(x);
	if (r1)
		*r1 = x[1];
	return (int64_t)x[0];
}

static uint8_t *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*len = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *b = malloc(*len + 1);
	if (b && fread(b, 1, *len, f) != *len) {
		free(b);
		b = NULL;
	}
	fclose(f);
	return b;
}

struct phdr { uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align; };

static uint64_t boot_meta_pa;	/* fajita: `wland boot PA` puts the PAS metadata there (e.g. below 4 GiB) */

static int boot_modem(void)
{
	/* 1. proxy votes as downstream (vdd_cx/vdd_mss >= 385): the top levels */
	int ncx = levels("cx.lvl", 0), nmss = levels("mss.lvl", 0);
	uint64_t aux;
	uint32_t len, top_cx = 0, top_mss = 0;
	if (!cmddb_aux("cx.lvl", &aux, &len))
		for (uint32_t i = 0; i < len / 2; i++)
			if (aux16(aux, i))
				top_cx = i;
	if (!cmddb_aux("mss.lvl", &aux, &len))
		for (uint32_t i = 0; i < len / 2; i++)
			if (aux16(aux, i))
				top_mss = i;
	printf("boot: cx %d levels, mss %d levels: voting cx[%u] mss[%u]\n", ncx, nmss, top_cx, top_mss);
	if (vote("cx.lvl", top_cx) || vote("mss.lvl", top_mss))
		return -1;

	/* 2. metadata: ELF header + program headers + hash segment */
	size_t mlen;
	uint8_t *mdt = slurp(FW_DIR "/modem.mdt", &mlen);
	if (!mdt) {
		fprintf(stderr, "boot: no %s/modem.mdt\n", FW_DIR);
		return -1;
	}
	uint32_t phoff, phnum = 0;
	memcpy(&phoff, mdt + 0x1c, 4);
	memcpy(&phnum, mdt + 0x2c, 2);
	struct phdr *ph = (struct phdr *)(mdt + phoff);
	int hash = -1;
	for (uint32_t i = 0; i < phnum; i++)
		if (((ph[i].flags >> 24) & 7) == 2)
			hash = (int)i;
	if (hash < 0) {
		fprintf(stderr, "boot: no hash segment\n");
		return -1;
	}
	size_t ehsz = phoff + 32ull * phnum, meta_len = ehsz + ph[hash].filesz;
	uint8_t *meta = calloc(1, meta_len + 4);
	memcpy(meta, mdt, ehsz);
	if (ph[hash].offset + ph[hash].filesz <= mlen) {
		memcpy(meta + ehsz, mdt + ph[hash].offset, ph[hash].filesz);
	} else {
		char path[128];
		size_t hl;
		snprintf(path, sizeof(path), FW_DIR "/modem.b%02d", hash);
		uint8_t *h = slurp(path, &hl);
		if (!h || hl < ph[hash].filesz) {
			fprintf(stderr, "boot: hash segment %s missing\n", path);
			return -1;
		}
		memcpy(meta + ehsz, h, ph[hash].filesz);
		free(h);
	}
	/* fajita: XNU's memory is all above 4 GiB here, so pm_alloc's buffer is too; `wland boot PA` puts the
	 * metadata at PA instead (free low DRAM; /dev/pmem writes it as device memory, so TZ sees it). */
	uint64_t meta_pa = boot_meta_pa ? boot_meta_pa : pm_alloc(meta_len, 0x1000);
	if (!meta_pa)
		return -1;
	pm_wr(meta_pa, meta, (meta_len + 3) & ~(size_t)3);
	uint64_t r1 = 0;
	int64_t rc = scm(1, 2 | (2u << 6), PAS_MODEM, meta_pa, 0, &r1);
	printf("boot: init image (%zu bytes metadata at %llx) -> %lld/%llx\n", meta_len, (unsigned long long)meta_pa,
	       (long long)rc, (unsigned long long)r1);
	if (rc || r1)
		return -1;

	/* 3. memory setup for the relocatable image, then the segments at their addresses */
	rc = scm(2, 3, PAS_MODEM, MPSS_PA, MPSS_SIZE, &r1);
	printf("boot: mem setup %llx+%llx -> %lld/%llx\n", MPSS_PA, MPSS_SIZE, (long long)rc, (unsigned long long)r1);
	if (rc || r1)
		return -1;
	uint64_t total = 0;
	static uint8_t zeros[0x10000];
	for (uint32_t i = 0; i < phnum; i++) {
		if (ph[i].type != 1 || (int)i == hash || !ph[i].memsz)
			continue;
		uint64_t pa = ph[i].paddr;
		if (pa < MPSS_PA || pa + ph[i].memsz > MPSS_PA + MPSS_SIZE) {
			fprintf(stderr, "boot: segment %u at %llx outside the region\n", i, (unsigned long long)pa);
			return -1;
		}
		if (ph[i].filesz) {
			char path[128];
			size_t sl;
			snprintf(path, sizeof(path), FW_DIR "/modem.b%02u", i);
			uint8_t *seg = slurp(path, &sl);
			if (!seg || sl < ph[i].filesz) {
				fprintf(stderr, "boot: %s missing or short\n", path);
				return -1;
			}
			pm_wr(pa, seg, (ph[i].filesz + 3) & ~3u);
			free(seg);
			total += ph[i].filesz;
		}
		for (uint64_t o = (ph[i].filesz + 3) & ~3ull; o < ph[i].memsz; o += sizeof(zeros)) {
			uint64_t n = ph[i].memsz - o < sizeof(zeros) ? ph[i].memsz - o : sizeof(zeros);
			pm_wr(pa + o, zeros, (n + 3) & ~3ull);
		}
	}
	printf("boot: %llu bytes of segments loaded\n", (unsigned long long)total);

	/* 4. tell the AOP (Linux q6v5_prepare: before the Q6 runs), then authenticate and release reset */
	qmp_send("{class: image, res: load_state, name: modem, val: on}");
	/* a restart keeps the last run's state bits until the modem rewrites them: forget them */
	uint64_t ve;
	if (smp2p_in_entry("slave-kernel", &ve) == 0)
		wr32(ve, 0);
	modem_state = 0xffffffff;
	rc = scm(5, 1, PAS_MODEM, 0, 0, &r1);
	printf("boot: auth and reset -> %lld/%llx\n", (long long)rc, (unsigned long long)r1);
	if (rc || r1)
		return -1;

	/* 5. wait for SMP2P in: "ready" (bit 1) from the modem */
	for (int t = 0; t < 300; t++) {
		uint32_t v;
		smp2p_service();
		/* after a crash the old entry still says ready + fatal until the modem rewrites it */
		if (smp2p_in_value("slave-kernel", &v) == 0 && (v & 2) && !(v & 1)) {
			printf("boot: modem ready after %d00 ms\n", t);
			return 0;
		}
		usleep(100000);
	}
	printf("boot: no ready from the modem in 30 s\n");
	smp2p_show("in", 435);
	return -1;
}

/* ---- fajita: modem boot through MBA (Linux qcom_q6v5_mss.c, "qcom,sdm845-mss-pil"); SDM845's TZ refuses PAS ----
 * Order as q6v5_start -> q6v5_mba_load + q6v5_mpss_load for sdm845_mss (has_alt_reset, need_mem_protection).
 * Addresses: sdm845.dtsi mss_pil (qdsp6 0x04080000, rmb 0x04180000, halt-regs tcsr_regs_1 0x01f60000 +
 * 0x3000/0x5000/0x4000, resets aoss_reset 0x0c2a0000 + 0x10000 and pdc_reset 0x0b2e0000 + 0x100 bit 9,
 * clocks gcc-sdm845.c), mba_region 0x96500000 + 2 MB, mpss_region (MPSS_PA). */
#define MSS_QDSP6	0x04080000ull
#define MSS_RMB		0x04180000ull
#define HALT_Q6		0x01f63000ull
#define HALT_MODEM	0x01f65000ull
#define HALT_NC		0x01f64000ull
#define AOSS_MSS_RESTART 0x0c2b0000ull
#define PDC_SYNC_RESET	0x0b2e0100ull
#define PDC_MODEM_BIT	(1u << 9)
#define GCC		0x00100000ull
#define MBA_PA		0x96500000ull
#define MBA_SIZE	0x00200000ull
#define MBA_META_PA	0xE1000000ull	/* metadata in free low DRAM (as `boot 0xE1000000`) */
#define RMB_MBA_IMAGE	0x00
#define RMB_PBL_STATUS	0x04
#define RMB_MBA_COMMAND	0x08
#define RMB_MBA_STATUS	0x0c
#define RMB_PMI_META	0x10
#define RMB_PMI_START	0x14
#define RMB_PMI_LENGTH	0x18
#define RMB_MSS_STATUS	0x40
#define RMB_ALT_RESET	0x44
#define QDSP6SS_SLEEP	0x3c
#define QDSP6SS_BOOT_CORE_START 0x400
#define QDSP6SS_BOOT_CMD 0x404
#define PERM_HLOS	1u
#define PERM_MSS	2u

static uint32_t mba_perm = PERM_HLOS, mpss_perm = PERM_HLOS;

/* q6v5_xfer_mem_ownership: give [pa, pa+size) to HLOS (local) and/or the modem (remote) */
static int mss_assign(uint32_t *perm, int local, int remote, uint64_t pa, uint64_t size)
{
	uint32_t want = (local ? PERM_HLOS : 0) | (remote ? PERM_MSS : 0);
	if (*perm == want)
		return 0;
	uint32_t src[2], dst[2];
	int ns = 0, nd = 0;
	if (*perm & PERM_HLOS)
		src[ns++] = VMID_HLOS;
	if (*perm & PERM_MSS)
		src[ns++] = VMID_MSS_MSA;
	if (local)
		dst[nd++] = VMID_HLOS;
	if (remote)
		dst[nd++] = VMID_MSS_MSA;
	int rc = hyp_assign(pa, (size + 0xfff) & ~0xfffull, src, ns, dst, nd);
	if (!rc)
		*perm = want;
	return rc;
}

/* a GCC branch: set its enable bit (own register or the 0x52004 vote), then wait for CLK_OFF (bit 31) to clear */
static int gcc_on(const char *name, uint32_t reg, uint32_t bit, uint32_t halt)
{
	wr32(GCC + reg, rd32(GCC + reg) | bit);
	if (!halt) {
		usleep(10);
		printf("mba: clock %s on (%#x |= %#x)\n", name, reg, bit);
		return 0;
	}
	for (int i = 0; i < 200; i++) {
		if (!(rd32(GCC + halt) & 0x80000000u)) {
			printf("mba: clock %s on (%#x |= %#x, cbcr %#x)\n", name, reg, bit, rd32(GCC + halt));
			return 0;
		}
		usleep(5);
	}
	printf("mba: clock %s stays off (cbcr %#x)\n", name, rd32(GCC + halt));
	return -1;
}

/* reset-qcom-aoss.c: write 1, 200-300 us, write 0, 200-300 us */
static void aoss_mss_reset(void)
{
	wr32(AOSS_MSS_RESTART, 1);
	usleep(300);
	wr32(AOSS_MSS_RESTART, 0);
	usleep(300);
}

static void pdc_modem(int assert)
{
	uint32_t v = rd32(PDC_SYNC_RESET);
	wr32(PDC_SYNC_RESET, assert ? v | PDC_MODEM_BIT : v & ~PDC_MODEM_BIT);
}

/* q6v5_reset_deassert / q6v5_reset_assert with has_alt_reset */
static void mss_reset_deassert(void)
{
	pdc_modem(1);
	wr32(MSS_RMB + RMB_ALT_RESET, 1);
	aoss_mss_reset();
	wr32(MSS_RMB + RMB_ALT_RESET, 0);
	pdc_modem(0);
}

static void mss_reset_assert(void)
{
	pdc_modem(1);
	aoss_mss_reset();
	pdc_modem(0);
}

/* q6v5proc_halt_axi_port */
static void halt_axi(const char *name, uint64_t base)
{
	if (rd32(base + 8))
		return;	/* already idle */
	wr32(base, 1);
	for (int i = 0; i < 100 && !rd32(base + 4); i++)
		usleep(1000);
	if (!rd32(base + 8))
		printf("mba: %s port failed halt\n", name);
	wr32(base, 0);
}

static int32_t rmb_wait(uint32_t reg, uint32_t want, int ms)
{
	for (int i = 0; i < ms; i++) {
		int32_t v = (int32_t)rd32(MSS_RMB + reg);
		if (v < 0 || (!want && v) || (want && (uint32_t)v == want))
			return v;
		usleep(1000);
	}
	return 0x7fffffff;	/* timeout */
}

static void mss_fail_cleanup(void)
{
	halt_axi("q6", HALT_Q6);
	halt_axi("modem", HALT_MODEM);
	halt_axi("nc", HALT_NC);
	mss_assign(&mba_perm, 1, 0, MBA_PA, MBA_SIZE);
	mss_assign(&mpss_perm, 1, 0, MPSS_PA, MPSS_SIZE);
	mss_reset_assert();
	printf("mba: halted, reset asserted, memory back to HLOS\n");
}

static uint32_t arc_top(const char *res)
{
	uint64_t aux;
	uint32_t len, top = 0;
	if (!cmddb_aux(res, &aux, &len))
		for (uint32_t i = 0; i < len / 2; i++)
			if (aux16(aux, i))
				top = i;
	return top;
}

static int boot_modem_mba(void)
{
	/* q6v5_load: the MBA image into its region, while HLOS still owns it */
	size_t mlen;
	uint8_t *mba = slurp(FW_DIR "/mba.mbn", &mlen);
	if (!mba || mlen > 0x100000) {
		fprintf(stderr, "mba: no %s/mba.mbn (or over 1 MB)\n", FW_DIR);
		return -1;
	}
	if (mba_perm != PERM_HLOS || mpss_perm != PERM_HLOS) {
		fprintf(stderr, "mba: regions not ours (perm %u/%u): reboot the phone first\n", mba_perm, mpss_perm);
		return -1;
	}
	pm_wr(MBA_PA, mba, (mlen + 3) & ~(size_t)3);
	free(mba);
	printf("mba: mba.mbn (%zu bytes) at %llx\n", mlen, MBA_PA);

	/* qcom_q6v5_prepare: tell the AOP; proxy power domains cx, mx, mss at their top levels */
	qmp_send("{class: image, res: load_state, name: modem, val: on}");
	uint32_t tcx = arc_top("cx.lvl"), tmx = arc_top("mx.lvl"), tmss = arc_top("mss.lvl");
	if (vote("cx.lvl", tcx) || vote("mx.lvl", tmx) || vote("mss.lvl", tmss))
		return -1;
	printf("mba: voted cx[%u] mx[%u] mss[%u]\n", tcx, tmx, tmss);

	/* proxy clocks (prng; xo stays on while the apps run), reset clocks (iface, snoc_axi) */
	if (gcc_on("prng", 0x52004, 1u << 13, 0x34004) || gcc_on("iface", 0x8a000, 1, 0x8a000) ||
	    gcc_on("snoc_axi", 0x8a150, 1, 0x8a150))
		return -1;
	mss_reset_deassert();
	printf("mba: reset deasserted (alt reset), rmb mss status %#x\n", rd32(MSS_RMB + RMB_MSS_STATUS));
	/* active clocks: bus (q6 memnoc axi), mem (boot rom), gpll0_mss, mnoc_axi (mfab axis) */
	if (gcc_on("bus", 0x8a154, 1, 0) || gcc_on("mem", 0x52004, 1u << 10, 0x38004) ||
	    gcc_on("gpll0_mss", 0x52004, 1u << 17, 0) || gcc_on("mnoc_axi", 0x8a004, 1, 0)) {
		mss_reset_assert();
		return -1;
	}

	/* the Q6 may wipe the MPSS region: give it access; the MBA region too */
	if (mss_assign(&mpss_perm, 0, 1, MPSS_PA, MPSS_SIZE) || mss_assign(&mba_perm, 0, 1, MBA_PA, MBA_SIZE)) {
		mss_fail_cleanup();
		return -1;
	}
	wr32(MSS_RMB + RMB_MBA_IMAGE, (uint32_t)MBA_PA);

	/* q6v5proc_reset for MSS_SDM845: sleep clock, start core, boot FSM, then PBL */
	wr32(MSS_QDSP6 + QDSP6SS_SLEEP, rd32(MSS_QDSP6 + QDSP6SS_SLEEP) | 1);
	int ok = 0;
	for (int i = 0; i < 200 && !ok; i++) {
		ok = !(rd32(MSS_QDSP6 + QDSP6SS_SLEEP) & 0x80000000u);
		usleep(1);
	}
	if (!ok) {
		printf("mba: QDSP6SS sleep clock timed out (%#x)\n", rd32(MSS_QDSP6 + QDSP6SS_SLEEP));
		mss_fail_cleanup();
		return -1;
	}
	wr32(MSS_QDSP6 + QDSP6SS_BOOT_CORE_START, 1);
	wr32(MSS_QDSP6 + QDSP6SS_BOOT_CMD, 1);
	ok = 0;
	for (int i = 0; i < 1000 && !ok; i++) {
		ok = rd32(MSS_RMB + RMB_MSS_STATUS) & 1;
		usleep(10);
	}
	if (!ok) {
		printf("mba: boot FSM did not complete (rmb mss status %#x)\n", rd32(MSS_RMB + RMB_MSS_STATUS));
		mss_reset_deassert();
		mss_fail_cleanup();
		return -1;
	}
	int32_t pbl = rmb_wait(RMB_PBL_STATUS, 0, 1000);
	printf("mba: boot FSM done; PBL status %d\n", pbl);
	if (pbl != 1) {
		mss_fail_cleanup();
		return -1;
	}
	int32_t st = rmb_wait(RMB_MBA_STATUS, 0, 5000);
	printf("mba: MBA status %d (1 = xpu unlocked, 2 = unlocked + scribbled)\n", st);
	if (st != 1 && st != 2) {
		mss_fail_cleanup();
		return -1;
	}

	/* q6v5_mpss_load: metadata (ELF header + program headers + hash) for MBA to check */
	size_t mdlen;
	uint8_t *mdt = slurp(FW_DIR "/modem.mdt", &mdlen);
	if (!mdt) {
		fprintf(stderr, "mba: no %s/modem.mdt\n", FW_DIR);
		mss_fail_cleanup();
		return -1;
	}
	uint32_t phoff, phnum = 0;
	memcpy(&phoff, mdt + 0x1c, 4);
	memcpy(&phnum, mdt + 0x2c, 2);
	struct phdr *ph = (struct phdr *)(mdt + phoff);
	int hash = -1;
	for (uint32_t i = 0; i < phnum; i++)
		if (((ph[i].flags >> 24) & 7) == 2)
			hash = (int)i;
	size_t ehsz = phoff + 32ull * phnum;
	if (hash < 0 || ph[hash].offset + ph[hash].filesz > mdlen) {
		fprintf(stderr, "mba: no hash segment in modem.mdt\n");
		mss_fail_cleanup();
		return -1;
	}
	size_t meta_len = ehsz + ph[hash].filesz;
	uint8_t *meta = calloc(1, meta_len + 4);
	memcpy(meta, mdt, ehsz);
	memcpy(meta + ehsz, mdt + ph[hash].offset, ph[hash].filesz);
	pm_wr(MBA_META_PA, meta, (meta_len + 3) & ~(size_t)3);
	free(meta);
	wr32(MSS_RMB + RMB_PMI_LENGTH, 0);
	uint32_t meta_perm = PERM_HLOS;
	if (mss_assign(&meta_perm, 0, 1, MBA_META_PA, meta_len)) {
		mss_fail_cleanup();
		return -1;
	}
	wr32(MSS_RMB + RMB_PMI_META, (uint32_t)MBA_META_PA);
	wr32(MSS_RMB + RMB_MBA_COMMAND, 1);	/* META_DATA_READY */
	st = rmb_wait(RMB_MBA_STATUS, 3, 1000);
	printf("mba: metadata (%zu bytes at %llx) -> status %d (3 = authenticated)\n", meta_len, MBA_META_PA, st);
	mss_assign(&meta_perm, 1, 0, MBA_META_PA, meta_len);
	if (st != 3) {
		mss_fail_cleanup();
		return -1;
	}

	/* segments: the MPSS region back to HLOS, shared during the load, relocatable at its lowest paddr */
	mss_assign(&mpss_perm, 1, 0, MPSS_PA, MPSS_SIZE);
	if (mss_assign(&mpss_perm, 1, 1, MPSS_PA, MPSS_SIZE)) {
		mss_fail_cleanup();
		return -1;
	}
	uint64_t lo = ~0ull;
	int reloc = 0;
	for (uint32_t i = 0; i < phnum; i++)
		if (ph[i].type == 1 && (int)i != hash && ph[i].memsz) {
			if (ph[i].flags & (1u << 27))
				reloc = 1;
			if (ph[i].paddr < lo)
				lo = ph[i].paddr;
		}
	uint64_t mreloc = reloc ? lo : MPSS_PA, boot_addr = reloc ? MPSS_PA : lo;
	uint32_t total = 0;
	static uint8_t zeros[0x10000];
	for (uint32_t i = 0; i < phnum; i++) {
		if (ph[i].type != 1 || (int)i == hash || !ph[i].memsz)
			continue;
		uint64_t off = ph[i].paddr - mreloc;
		if (ph[i].paddr < mreloc || off + ph[i].memsz > MPSS_SIZE || ph[i].filesz > ph[i].memsz) {
			fprintf(stderr, "mba: segment %u does not fit the region\n", i);
			mss_fail_cleanup();
			return -1;
		}
		if (ph[i].filesz) {
			char path[128];
			size_t sl;
			snprintf(path, sizeof(path), FW_DIR "/modem.b%02u", i);
			uint8_t *seg = slurp(path, &sl);
			if (!seg || sl != ph[i].filesz) {
				fprintf(stderr, "mba: %s missing or wrong size\n", path);
				free(seg);
				mss_fail_cleanup();
				return -1;
			}
			pm_wr(MPSS_PA + off, seg, (ph[i].filesz + 3) & ~3u);
			free(seg);
		}
		for (uint64_t o = (ph[i].filesz + 3) & ~3ull; o < ph[i].memsz; o += sizeof(zeros)) {
			uint64_t n = ph[i].memsz - o < sizeof(zeros) ? ph[i].memsz - o : sizeof(zeros);
			pm_wr(MPSS_PA + off + o, zeros, (n + 3) & ~3ull);
		}
		total += ph[i].memsz;
		if (!rd32(MSS_RMB + RMB_PMI_LENGTH)) {
			wr32(MSS_RMB + RMB_PMI_START, (uint32_t)boot_addr);
			wr32(MSS_RMB + RMB_MBA_COMMAND, 2);	/* LOAD_READY */
		}
		wr32(MSS_RMB + RMB_PMI_LENGTH, total);
		st = (int32_t)rd32(MSS_RMB + RMB_MBA_STATUS);
		if (st < 0) {
			printf("mba: MPSS authentication failed at segment %u: %d\n", i, st);
			mss_fail_cleanup();
			return -1;
		}
	}
	free(mdt);
	printf("mba: %u bytes of segments loaded (relocatable %d, start %llx)\n", total, reloc,
	       (unsigned long long)boot_addr);
	if (mss_assign(&mpss_perm, 0, 1, MPSS_PA, MPSS_SIZE)) {
		mss_fail_cleanup();
		return -1;
	}
	st = rmb_wait(RMB_MBA_STATUS, 4, 10000);
	printf("mba: MPSS authentication -> status %d (4 = complete)\n", st);
	if (st != 4) {
		mss_fail_cleanup();
		return -1;
	}

	/* qcom_q6v5_wait_for_start: SMP2P "ready" (bit 1) from the modem; then reclaim the MBA region */
	uint64_t ve;
	if (smp2p_in_entry("slave-kernel", &ve) == 0)
		wr32(ve, 0);
	modem_state = 0xffffffff;
	for (int t = 0; t < 300; t++) {
		uint32_t v;
		smp2p_service();
		if (smp2p_in_value("slave-kernel", &v) == 0 && (v & 2) && !(v & 1)) {
			printf("mba: modem ready after %d00 ms\n", t);
			mss_assign(&mba_perm, 1, 0, MBA_PA, MBA_SIZE);
			return 0;
		}
		usleep(100000);
	}
	printf("mba: no ready from the modem in 30 s\n");
	smp2p_show("in", 435);
	return -1;
}

static void net_cb(const struct qrtr_hdr *h, const uint8_t *d, uint32_t n)
{
	uint32_t w[5] = { 0 };
	memcpy(w, d, n < sizeof(w) ? n : sizeof(w));
	if (h->type == QRTR_TYPE_DATA) {
		if (h->confirm_rx)
			qrtr_resume_tx(h);
		if (h->dst_port == RMTFS_PORT) {
			rmtfs_handle(h, d, n);
			return;
		}
		if (h->dst_port == WLFW_PORT) {
			wlfw_handle(h, d, n);
			return;
		}
		if (h->dst_port == PDMAPPER_PORT) {
			pdmapper_handle(h, d, n);
			return;
		}
		if (h->dst_port == TFTP_PORT || (h->dst_port >= TFTP_XFER_PORT0 && h->dst_port < TFTP_XFER_PORT0 + TFTP_XFERS)) {
			tftp_handle(h, d, n);
			return;
		}
		printf("qrtr: < DATA %u:%x -> %u:%x, %u bytes\n", h->src_node, h->src_port, h->dst_node, h->dst_port, n);
		return;
	}
	printf("[%6llu] qrtr: < ctrl type %u (cmd %u) from %u:%x: %x %x %x %x\n", (unsigned long long)link_ms(), h->type, w[0], h->src_node, h->src_port,
	       w[1], w[2], w[3], w[4]);
	if (w[0] == QRTR_TYPE_HELLO) {
		uint32_t hello[5] = { QRTR_TYPE_HELLO };	/* struct qrtr_ctrl_pkt: always 20 bytes */
		qrtr_send(QRTR_TYPE_HELLO, QRTR_PORT_CTRL, h->src_node, QRTR_PORT_CTRL, hello, sizeof(hello));
	} else if (w[0] == QRTR_TYPE_NEW_SERVER) {
		printf("qrtr:   server service %#x instance %#x at %u:%x\n", w[1], w[2], w[3], w[4]);
		if (w[1] == WLFW_SERVICE)
			wlfw_new_server(w[3], w[4]);
	}
}

/* the network to join: "ssid=NAME" and "psk=PASSPHRASE" lines (on the phone only, never in a repo) */
#define WLAN_CONF	"/var/local/wlan.conf"
static char conf_ssid[33], conf_psk[65];

static void read_conf(void)
{
	char line[128];
	FILE *f = fopen(WLAN_CONF, "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (!strncmp(line, "ssid=", 5))
			snprintf(conf_ssid, sizeof(conf_ssid), "%s", line + 5);
		else if (!strncmp(line, "psk=", 4))
			snprintf(conf_psk, sizeof(conf_psk), "%s", line + 4);
	}
	fclose(f);
	printf("net: will join '%s' (%s)\n", conf_ssid, conf_psk[0] ? "WPA2-PSK" : "open");
}

/*
 * Join after the first scan and stay joined: a failed try again after 5 s (4 times), then every 30 s with a
 * fresh scan first; a lost link (deauth, kickout, beacon miss) joins again after 3 s.
 */
static void join_poll(void)
{
	static int asked, joined, fails;
	static uint64_t next;
	if (!conf_ssid[0] || !radio_scans() || bridge_joins)
		return;				/* (wifid has taken over: bridge.c) */
	uint64_t now = link_ms();
	int r = radio_join_result();
	if (asked) {
		if (r == -1)
			return;				/* running */
		asked = 0;
		if (r == 0) {
			joined = 1;
			fails = 0;
			return;
		}
		fails++;
		next = now + (fails < 4 ? 5000 : 30000);
		if (fails >= 4)
			radio_rescan(25000);
		printf("[%6llu] net: join failed (%s), try %d, next in %llu s\n", (unsigned long long)now, strerror(r), fails,
		       (unsigned long long)(next - now) / 1000);
		return;
	}
	if (joined) {
		if (r != ENOTCONN)
			return;
		joined = 0;
		fails = 0;
		next = now + 3000;
		printf("[%6llu] net: link lost, joining again in 3 s\n", (unsigned long long)now);
		return;
	}
	if (next && now < next)
		return;
	r = radio_join(conf_ssid, conf_psk);
	printf("[%6llu] net: join '%s' (try %d) -> %d\n", (unsigned long long)now, conf_ssid, fails + 1, r);
	if (r == EINVAL || r == EIO) {
		printf("net: cannot join with this wlan.conf; not trying again\n");
		conf_ssid[0] = 0;
		return;
	}
	asked = 1;
}

static volatile sig_atomic_t drop_asked;

static void on_usr1(int sig)
{
	drop_asked = 1;
}

static void net_idle(void)
{
	if (drop_asked) {
		drop_asked = 0;
		radio_disconnect();
	}
	smp2p_service();
	radio_poll();
	ipnet_poll();
	bridge_poll();
	join_poll();
}

static int net(unsigned secs)
{
	if (getenv("WLAND_VERBOSE"))
		verbose_link = atoi(getenv("WLAND_VERBOSE"));
	link_idle = net_idle;
	signal(SIGUSR1, on_usr1);
	signal(SIGCHLD, SIG_IGN);	/* netsvc children (ipnet.c) reap themselves */
	if (link_up(net_cb, 5000))
		return -1;
	if (rmtfs_init())
		printf("net: no rmtfs\n");
	if (pdmapper_init() || tftp_init())
		printf("net: no pd-mapper/tftp\n");
	read_conf();
	uint64_t end = link_ms() + (uint64_t)secs * 1000, stats = link_ms() + 30000;
	while (!secs || link_ms() < end) {
		link_poll(100);
		if (link_ms() > stats) {
			radio_stats();
			stats = link_ms() + 30000;
		}
		if (!wlfw_pending())
			continue;
		if (wlfw_start()) {
			printf("net: WLFW bring-up failed\n");
		} else if (!getenv("WLAND_NO_RADIO")) {
			delay_ms(100);	/* topaz: only after mission mode; before it a CE access hangs the bus */
			if (radio_start())
				printf("net: radio start failed\n");
			else if (bridge_open() != 0 && conf_ssid[0] && ipnet_init())
				printf("net: no utun: joining without IP\n");
		}
	}
	radio_summary();
	radio_stats();
	return 0;
}

/* everything `up` does, from a fresh boot */
static int up(unsigned secs)
{
	if (smmu_wlan_identity())
		printf("up: no WLAN SMMU stream: the radio will not start\n");
	if (wlan_power() || smp2p_create() || boot_modem())
		return 1;
	return net(secs) ? 1 : 0;
}

/*
 * The boot job: `up 0` at most once per phone boot (a second modem boot in the same phone boot is untested and
 * MSA/rmtfs hangs wedge a CPU), unless switched off. stdout/stderr go to /var/local/wland.log.
 */
#define WLAND_LOG	"/var/local/wland.log"
#define WLAND_STAMP	"/var/local/wland.boot"
/* libsystem_kernel: jetsam control (root may change its own limits); MEMORYSTATUS_CMD_SET_MEMLIMIT_PROPERTIES */
int memorystatus_control(uint32_t command, int32_t pid, uint32_t flags, void *buffer, size_t size);
struct memlimit_properties { int32_t active; uint32_t active_attr; int32_t inactive; uint32_t inactive_attr; };

static int auto_up(void)
{
	/* launchd's per-process limit for a job killed us while the modem firmware loaded (jetsam reason 7) */
	struct memlimit_properties ml = { 2048, 0, 2048, 0 };
	int mrc = memorystatus_control(7, getpid(), 0, &ml, sizeof(ml));
	char args[1024] = "";
	size_t n = sizeof(args) - 1;
	sysctlbyname("kern.bootargs", args, &n, NULL, 0);
	if (!access("/var/local/wland.off", F_OK) || strstr(args, "sweetwifi=0")) {
		fprintf(stderr, "wland: off (/var/local/wland.off or sweetwifi=0)\n");
		return 0;
	}
	struct timeval bt = { 0 };
	n = sizeof(bt);
	sysctlbyname("kern.boottime", &bt, &n, NULL, 0);
	char stamp[32] = "", now[32];
	snprintf(now, sizeof(now), "%ld\n", (long)bt.tv_sec);
	FILE *f = fopen(WLAND_STAMP, "r");
	if (f) {
		if (!fgets(stamp, sizeof(stamp), f))
			stamp[0] = 0;
		fclose(f);
	}
	if (!strcmp(stamp, now)) {
		fprintf(stderr, "wland: already started in this boot\n");
		return 1;
	}
	if ((f = fopen(WLAND_STAMP, "w"))) {
		fputs(now, f);
		fclose(f);
	}
	rename(WLAND_LOG, WLAND_LOG ".prev");
	if (freopen(WLAND_LOG, "a", stdout))
		dup2(fileno(stdout), 2);
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("wland auto: boot %ld, memory limit 2048 MB -> %d\n", (long)bt.tv_sec, mrc);
	return up(0);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: wland smem | smp2p | vote RES LVL | levels RES | boot\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IONBF, 0);	/* a stalled bus access must not swallow the output */
	pm_init();
	if (smem_init())
		return 1;
	const char *c = argv[1];
	if (!strcmp(c, "smem")) {
		smem_dump();
	} else if (!strcmp(c, "smp2p")) {
		smp2p_show("in", 435);	/* qcom,smem = <435>, <428>: inbound, outbound */
		smp2p_show("out", 428);
	} else if (!strcmp(c, "levels") && argc > 2) {
		levels(argv[2], 1);
	} else if (!strcmp(c, "qmp") && argc > 2) {
		return qmp_send(argv[2]) ? 1 : 0;
	} else if (!strcmp(c, "smp2p-init")) {
		return smp2p_init() ? 1 : 0;
	} else if (!strcmp(c, "shutdown")) {
		uint64_t r1 = 0;
		int64_t rc = scm(6, 1, PAS_MODEM, 0, 0, &r1);	/* PAS_SHUTDOWN */
		printf("shutdown -> %lld/%llx\n", (long long)rc, (unsigned long long)r1);
		return rc ? 1 : 0;
	} else if (!strcmp(c, "items")) {
		smem_list();
	} else if (!strcmp(c, "item") && argc > 2) {
		uint64_t pa;
		uint32_t sz;
		unsigned host = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 0) : SMEM_HOST_MODEM;
		if (smem_get(host, (unsigned)atoi(argv[2]), &pa, &sz)) {
			printf("item %s: not found\n", argv[2]);
			return 1;
		}
		printf("item %s: pa %llx size %u\n", argv[2], (unsigned long long)pa, sz);
		for (uint32_t o = 0; o < sz && o < 4096; o += 16) {
			uint8_t b[16];
			for (int k = 0; k < 16; k += 4) {
				uint32_t w = rd32(pa + o + k);
				memcpy(b + k, &w, 4);
			}
			printf("%04x:", o);
			for (int k = 0; k < 16; k++)
				printf(" %02x", b[k]);
			printf("  ");
			for (int k = 0; k < 16; k++)
				putchar(b[k] >= 32 && b[k] < 127 ? b[k] : '.');
			printf("\n");
		}
	} else if (!strcmp(c, "net")) {
		return net(argc > 2 ? (unsigned)atoi(argv[2]) : 10) ? 1 : 0;
	} else if (!strcmp(c, "auto")) {
		return auto_up();
	} else if (!strcmp(c, "up")) {
		/* everything from a fresh phone boot: SMP2P, the modem, GLINK + QRTR; the WLAN SMMU stream first */
		return up(argc > 2 ? (unsigned)atoi(argv[2]) : 20);
	} else if (!strcmp(c, "smmu")) {
		return smmu_wlan_identity() ? 1 : 0;
	} else if (!strcmp(c, "cmddb")) {
		cmddb_list();
	} else if (!strcmp(c, "wlan-power")) {
		return wlan_power() ? 1 : 0;
	} else if (!strcmp(c, "boot-mba")) {
		return boot_modem_mba() ? 1 : 0;
	} else if (!strcmp(c, "boot")) {
		if (argc > 2)
			boot_meta_pa = strtoull(argv[2], NULL, 0);
		return boot_modem() ? 1 : 0;
	} else if (!strcmp(c, "bcm") && argc > 3) {
		/* a BCM (bus) vote: x = average, y = peak, in the BCM's own units (0x3fff = its top level); commit + valid */
		uint32_t x = (uint32_t)strtoul(argv[2 + 1], NULL, 0) & 0x3fff, y = argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 0) & 0x3fff : x;
		uint32_t off[1] = { 0 }, val[1] = { 1u << 30 | 1u << 29 | x << 14 | y };
		int rc = pm_rpmh(argv[2], 1, off, val);
		printf("bcm %s x %#x y %#x (%#x) -> %d\n", argv[2], x, y, val[0], rc);
		return rc ? 1 : 0;
	} else if (!strcmp(c, "vote") && argc > 3) {
		return vote(argv[2], (uint32_t)strtoul(argv[3], NULL, 0)) ? 1 : 0;
	} else {
		fprintf(stderr, "wland: unknown command\n");
		return 2;
	}
	return 0;
}
