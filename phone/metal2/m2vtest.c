/*
 * m2vtest: the metal2vulkan runtime on Turnip on the fajita Adreno 630 (phone/metal2/bridge self-test): a
 * compute kernel, a render pass, a render into imported host pages, then every QuartzCore kernel and
 * vertex/fragment pair translated and built into pipelines on the phone. From ios-flame's
 * phone/metal2/m2vtest.c (MIT), changed for this port like vktest: Turnip rides inside the executable
 * (phone/turnip/blob.S) and is unpacked before use, TU_DARWIN_SERVICE=SweetGPU, and the cache lives
 * under /private/var/tmp/sweetrun. Last line "M2VTEST PASS" or "M2VTEST FAIL".
 *   m2vtest [TURNIP] [CACHE_DIR] [METALLIB]
 */
#include <compression.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "bridge/NativeMetalBridge.h"

/*
 * libSystem's sys_dcache_flush assumes a coherent GPU (Apple's): here it must really clean and invalidate,
 * from user space (SCTLR_EL1.UCI is set). This definition wins over libSystem's at link time.
 */
void sys_dcache_flush(void *start, size_t len)
{
	uintptr_t a = (uintptr_t)start & ~(uintptr_t)63, end = (uintptr_t)start + len;
	for (; a < end; a += 64)
		__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static double t0;
static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void mark(uint32_t m)
{
	static const char *const names[] = { "", "device", "fixture library", "compute", "render", "import", "QuartzCore" };
	unsigned i = m & 0xff;
	printf("[%7.2f s] step %x %s\n", now() - t0, m, i < 7 ? names[i] : (i == 0xf ? "done" : "?"));
	fflush(stdout);
}

/* blob.S: 8-byte little-endian unpacked size, then the raw-deflate Turnip dylib */
extern const uint8_t turnip_blob_start[], turnip_blob_end[];
#define TURNIP_PATH "/private/var/tmp/sweetrun/libvulkan_freedreno.dylib"

static const char *unpack_turnip(void)
{
	uint64_t size;
	memcpy(&size, turnip_blob_start, 8);
	size_t packed = (size_t)(turnip_blob_end - turnip_blob_start) - 8;
	uint8_t *buf = malloc(size);
	if (!buf || compression_decode_buffer(buf, size, turnip_blob_start + 8, packed, NULL, COMPRESSION_ZLIB) != size) {
		printf("FAIL unpacking Turnip\n");
		return NULL;
	}
	mkdir("/private/var/tmp/sweetrun", 0755);
	unlink(TURNIP_PATH);
	int fd = open(TURNIP_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (fd < 0 || write(fd, buf, size) != (ssize_t)size) {
		printf("FAIL writing %s\n", TURNIP_PATH);
		return NULL;
	}
	close(fd);
	free(buf);
	return TURNIP_PATH;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	setenv("TU_DARWIN_SERVICE", "SweetGPU", 0);
	const char *lib = argc > 1 ? argv[1] : unpack_turnip();
	if (!lib) {
		printf("M2VTEST FAIL\n");
		return 1;
	}
	const char *cache = argc > 2 ? argv[2] : "/private/var/tmp/sweetrun/m2v-cache";
	const char *qc = argc > 3 ? argv[3] : "/System/Library/Frameworks/QuartzCore.framework/default.metallib";
	mkdir(cache, 0777);
	int fd = open(qc, O_RDONLY);
	struct stat st;
	uint8_t *bytes = NULL;
	size_t n = 0;
	if (fd >= 0 && fstat(fd, &st) == 0) {
		n = (size_t)st.st_size;
		bytes = malloc(n);
		if (read(fd, bytes, n) != (ssize_t)n)
			n = 0;
		close(fd);
	}
	printf("QuartzCore metallib: %zu bytes\n", n);
	struct NmbSelftest out = { 0 };
	t0 = now();
	uint32_t r = nmb_selftest(lib, cache, bytes, n, mark, &out);
	printf("result %x stage %x: compute bad %u/1000, render bad %u/64, import bad %u/4096\n", r, out.stage, out.compute_bad,
	       out.render_bad, out.import_bad);
	printf("QuartzCore: %u functions, kernels %u ok %u failed, vertex/fragment pairs %u ok %u failed (%.1f s)\n",
	       out.qc_functions, out.qc_kernels_ok, out.qc_kernels_failed, out.qc_pairs_ok, out.qc_pairs_failed, now() - t0);
	printf("message: %.96s\n", out.message);
	printf("M2VTEST %s\n", out.stage == 0 ? "PASS" : "FAIL");
	return out.stage != 0;
}
