/*
 * chlinstall: put Chlorophyll.bundle on the phone's root (boot-arg sweetmtl=3), as mtl2install does for SweetMetal2.bundle.
 * Runs at boot through sweetrun (root, rootrw=1). The blob is pack.py's: name[56], raw size, packed size, raw DEFLATE.
 * Writes /System/Library/Extensions/Chlorophyll.bundle, with chlorophyll.allow listing only backboardd so no other process gets
 * this device yet, then restarts backboardd and SpringBoard so they open Metal now. Later boots need only sweetgpu=3 sweetmtl=3,
 * and sweetmtl=2 ignores the bundle. Last line CHLINSTALL DONE or CHLINSTALL FAIL.
 */
#include <compression.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define BUNDLE "/System/Library/Extensions/Chlorophyll.bundle/"

extern const uint8_t mtl_blob_start[], mtl_blob_end[];

struct entry { char name[56]; uint64_t raw, packed; };

static int fail(const char *what)
{
	printf("chlinstall: %s: %s\nCHLINSTALL FAIL\n", what, strerror(errno));
	return 1;
}

/* Written beside, then renamed: a process loading the bundle never sees half a file. */
static int put(const char *name, const void *data, size_t size, mode_t mode)
{
	char path[160], tmp[176];
	snprintf(path, sizeof(path), BUNDLE "%s", name);
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (fd < 0 || write(fd, data, size) != (ssize_t)size) {
		return -1;
	}
	fsync(fd);
	close(fd);
	return chmod(tmp, mode) == 0 && rename(tmp, path) == 0 ? 0 : -1;
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
	if (mkdir(BUNDLE, 0755) != 0 && errno != EEXIST)
		return fail("mkdir " BUNDLE);
	const uint8_t *p = mtl_blob_start;
	while (p + sizeof(struct entry) <= mtl_blob_end) {
		struct entry e;
		memcpy(&e, p, sizeof(e));
		p += sizeof(e);
		uint8_t *buf = malloc(e.raw);
		if (!buf || compression_decode_buffer(buf, e.raw, p, e.packed, NULL, COMPRESSION_ZLIB) != e.raw)
			return fail(e.name);
		if (put(e.name, buf, e.raw, strcmp(e.name, "Info.plist") ? 0755 : 0644) != 0)
			return fail(e.name);
		printf("chlinstall: %s: %llu bytes\n", e.name, (unsigned long long)e.raw);
		free(buf);
		p += e.packed;
	}
	static const char allow[] = "backboardd\n";
	if (put("chlorophyll.allow", allow, sizeof(allow) - 1, 0644) != 0)
		return fail("chlorophyll.allow");
	sync();
	int sb = kill_named("SpringBoard"), bb = kill_named("backboardd");
	printf("chlinstall: restarted backboardd (%d) and SpringBoard (%d)\nCHLINSTALL DONE\n", bb, sb);
	return 0;
}
