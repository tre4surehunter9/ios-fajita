/*
 * mtl2install: put SweetMetal2.bundle on the phone's root without a root rebuild (GPU-PLAN.md 3.3).
 *
 * Runs at boot through sweetrun/bigrun (root, the root filesystem is writable with rootrw=1). Carries the
 * bundle as one raw-DEFLATE archive (blob.S, packed by phone/mtl2install/pack.py in the "metal2" workflow):
 * u64 size, then { "SWAR", u32 count, count x { u16 path length, path, u32 mode, u64 size, bytes } }.
 * Writes it to /System/Library/Extensions/SweetMetal2.bundle, makes /var/local with mtl2-clients (SpringBoard;
 * backboardd always gets a device) and restarts backboardd and SpringBoard so they open Metal now. The bundle
 * stays on the root: later boots need only sweetgpu=3 sweetmtl=2. Last line "MTL2INSTALL DONE" or "... FAIL".
 */
#include <compression.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define BUNDLE "/System/Library/Extensions/SweetMetal2.bundle"

extern const uint8_t install_blob_start[], install_blob_end[];

static int fail(const char *what)
{
	printf("mtl2install: %s: %s\nMTL2INSTALL FAIL\n", what, strerror(errno));
	return 1;
}

static void mkdirs(const char *path)
{
	char p[512];
	snprintf(p, sizeof(p), "%s", path);
	for (char *c = p + 1; *c; c++) {
		if (*c == '/') {
			*c = 0;
			mkdir(p, 0755);
			*c = '/';
		}
	}
	mkdir(p, 0755);
}

/* SIGKILL every process whose name is name (launchd restarts the daemons). */
static int kill_named(const char *name)
{
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0 };
	size_t len = 0;
	if (sysctl(mib, 3, NULL, &len, NULL, 0) != 0)
		return 0;
	struct kinfo_proc *procs = malloc(len + 4096);
	if (!procs || sysctl(mib, 3, procs, &len, NULL, 0) != 0)
		return 0;
	int n = 0;
	for (size_t i = 0; i < len / sizeof(*procs); i++) {
		if (strncmp(procs[i].kp_proc.p_comm, name, MAXCOMLEN) == 0 && kill(procs[i].kp_proc.p_pid, SIGKILL) == 0)
			n++;
	}
	free(procs);
	return n;
}

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	uint64_t size;
	memcpy(&size, install_blob_start, 8);
	size_t packed = (size_t)(install_blob_end - install_blob_start) - 8;
	uint8_t *ar = malloc(size);
	if (!ar || compression_decode_buffer(ar, size, install_blob_start + 8, packed, NULL, COMPRESSION_ZLIB) != size)
		return fail("unpacking the archive");
	if (memcmp(ar, "SWAR", 4) != 0)
		return fail("bad archive");
	uint32_t count;
	memcpy(&count, ar + 4, 4);
	size_t off = 8;
	mkdirs(BUNDLE);
	for (uint32_t i = 0; i < count; i++) {
		uint16_t plen;
		uint32_t mode;
		uint64_t fsize;
		memcpy(&plen, ar + off, 2);
		char rel[256];
		if (plen >= sizeof(rel))
			return fail("path too long");
		memcpy(rel, ar + off + 2, plen);
		rel[plen] = 0;
		memcpy(&mode, ar + off + 2 + plen, 4);
		memcpy(&fsize, ar + off + 6 + plen, 8);
		const uint8_t *data = ar + off + 14 + plen;
		off += 14 + plen + fsize;
		if (off > size || strstr(rel, ".."))
			return fail("archive entry out of range");
		char path[512];
		snprintf(path, sizeof(path), BUNDLE "/%s", rel);
		char *slash = strrchr(path, '/');
		*slash = 0;
		mkdirs(path);
		*slash = '/';
		unlink(path);
		int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode & 0777);
		if (fd < 0)
			return fail(path);
		for (const uint8_t *p = data; p < data + fsize;) {
			ssize_t w = write(fd, p, (size_t)(data + fsize - p));
			if (w <= 0)
				return fail(path);
			p += w;
		}
		fchmod(fd, mode & 0777);
		close(fd);
		printf("mtl2install: %s (%llu bytes)\n", path, (unsigned long long)fsize);
	}
	free(ar);

	mkdir("/var/local", 0777);
	chmod("/var/local", 0777);
	FILE *f = fopen("/var/local/mtl2-clients", "w");
	if (!f)
		return fail("/var/local/mtl2-clients");
	fputs("SpringBoard\n", f);
	fclose(f);
	int log = open("/var/local/mtl2.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
	if (log >= 0) {
		fchmod(log, 0666);
		close(log);
	}
	printf("mtl2install: /var/local/mtl2-clients = SpringBoard (backboardd always)\n");
	sync();
	int sb = kill_named("SpringBoard"), bb = kill_named("backboardd");
	printf("mtl2install: restarted backboardd (%d) and SpringBoard (%d)\nMTL2INSTALL DONE\n", bb, sb);
	return 0;
}
