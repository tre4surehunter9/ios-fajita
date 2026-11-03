/*
 * sweetsetup: the on-phone setup of a freshly flashed root, once (quiet + unquiet + extfix in one).
 *
 * Runs at boot through sweetrun (root; the root filesystem is writable with rootrw=1). Needs kernel 0124 or
 * later. A marker file makes every later run do nothing, so the image can stay in boot_a.
 *  - /System/Library/xpc/launchd.plist: takes out com.apple.voicemail.vmd, com.apple.fairplayd.H2 and
 *    com.apple.ReportCrash (see phone/quiet); com.apple.bluetoothd stays, because mediaserverd and launchd
 *    spin without it (see phone/unquiet). The original is kept once as launchd.plist.pre-quiet.
 *  - /private/var/preferences/FeatureFlags/Domain/ExtensionKit.plist: enforce_xpc_cache_code_signing off,
 *    and LaunchServices' database deleted + lsd killed so it seeds again with the extensions (see
 *    phone/extfix): wallpapers and widgets.
 * launchd reads its job list at boot: reboot once afterwards. Last line "SWEETSETUP DONE ..." or
 * "SWEETSETUP FAIL ..."; a failure writes no marker, so the next boot tries again.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define MARKER_DIR "/private/var/local"
#define MARKER MARKER_DIR "/sweetsetup.done"
#define PLIST "/System/Library/xpc/launchd.plist"
#define BACKUP PLIST ".pre-quiet"
#define TMP PLIST ".sweetsetup-tmp"
#define FLAG_DIR "/private/var/preferences/FeatureFlags/Domain"
#define FLAG FLAG_DIR "/ExtensionKit.plist"
#define STORE_PREFIX "com.apple.LaunchServices"

static const char *const labels[] = {
	"com.apple.voicemail.vmd",
	"com.apple.fairplayd.H2",
	"com.apple.ReportCrash",
};
#define NLABELS (sizeof(labels) / sizeof(labels[0]))

static const char flag_xml[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n<dict>\n\t<key>enforce_xpc_cache_code_signing</key>\n\t<dict>\n"
    "\t\t<key>Enabled</key>\n\t\t<false/>\n\t</dict>\n</dict>\n</plist>\n";

static int fail(const char *what)
{
	printf("sweetsetup: %s: %s\nSWEETSETUP FAIL\n", what, strerror(errno));
	return 1;
}

static void mkdirs(const char *path)
{
	char p[256];
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

static int write_all(const char *path, const void *data, size_t n, mode_t mode)
{
	const uint8_t *p = data;
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (fd < 0)
		return -1;
	while (n) {
		ssize_t w = write(fd, p, n);
		if (w <= 0) {
			close(fd);
			return -1;
		}
		p += w;
		n -= (size_t)w;
	}
	fsync(fd);
	return close(fd);
}

/* Jobs removed, or -1 with errno set. */
static long remove_jobs(void)
{
	struct stat st;
	if (stat(PLIST, &st) != 0)
		return -1;
	int fd = open(PLIST, O_RDONLY);
	if (fd < 0)
		return -1;
	uint8_t *buf = malloc((size_t)st.st_size);
	if (!buf || read(fd, buf, (size_t)st.st_size) != st.st_size) {
		close(fd);
		return -1;
	}
	close(fd);
	CFDataRef data = CFDataCreateWithBytesNoCopy(NULL, buf, st.st_size, kCFAllocatorNull);
	CFPropertyListFormat format = 0;
	CFPropertyListRef plist = CFPropertyListCreateWithData(NULL, data, kCFPropertyListMutableContainersAndLeaves,
	    &format, NULL);
	errno = EINVAL;
	if (!plist || CFGetTypeID(plist) != CFDictionaryGetTypeID())
		return -1;
	CFMutableDictionaryRef jobs = (CFMutableDictionaryRef)CFDictionaryGetValue(plist, CFSTR("LaunchDaemons"));
	if (!jobs || CFGetTypeID(jobs) != CFDictionaryGetTypeID())
		return -1;
	CFIndex n = CFDictionaryGetCount(jobs);
	const void **keys = malloc(sizeof(void *) * (size_t)n), **vals = malloc(sizeof(void *) * (size_t)n);
	CFDictionaryGetKeysAndValues(jobs, keys, vals);
	long removed = 0;
	for (CFIndex i = 0; i < n; i++) {
		if (CFGetTypeID(vals[i]) != CFDictionaryGetTypeID())
			continue;
		CFStringRef label = CFDictionaryGetValue(vals[i], CFSTR("Label"));
		char l[128];
		if (!label || CFGetTypeID(label) != CFStringGetTypeID() ||
		    !CFStringGetCString(label, l, sizeof(l), kCFStringEncodingUTF8))
			continue;
		for (size_t k = 0; k < NLABELS; k++) {
			if (strcmp(l, labels[k]) == 0) {
				CFDictionaryRemoveValue(jobs, keys[i]);
				printf("sweetsetup: removed job %s\n", l);
				removed++;
			}
		}
	}
	if (removed == 0)
		return 0;
	CFDataRef out = CFPropertyListCreateData(NULL, plist, format, 0, NULL);
	errno = EINVAL;
	if (!out)
		return -1;
	if (access(BACKUP, F_OK) != 0 && write_all(BACKUP, buf, (size_t)st.st_size, st.st_mode & 07777) != 0)
		return -1;
	if (write_all(TMP, CFDataGetBytePtr(out), (size_t)CFDataGetLength(out), st.st_mode & 07777) != 0)
		return -1;
	if (chown(TMP, st.st_uid, st.st_gid) != 0 || chmod(TMP, st.st_mode & 07777) != 0)
		return -1;
	if (rename(TMP, PLIST) != 0)
		return -1;
	printf("sweetsetup: %ld jobs left, original kept as %s\n", (long)(n - removed), BACKUP);
	return removed;
}

/* Regular files named com.apple.LaunchServices* under the container trees (the UUID is per phone). */
static int delete_stores(void)
{
	char *const roots[] = {
		"/private/var/mobile/Containers/Data/InternalDaemon",
		"/private/var/mobile/Containers/Data/System",
		"/private/var/containers/Data/System",
		NULL,
	};
	FTS *fts = fts_open(roots, FTS_PHYSICAL | FTS_NOCHDIR, NULL);
	if (!fts)
		return -1;
	int n = 0;
	for (FTSENT *e; (e = fts_read(fts)) != NULL;) {
		if (e->fts_info == FTS_F && strncmp(e->fts_name, STORE_PREFIX, strlen(STORE_PREFIX)) == 0) {
			int rc = unlink(e->fts_path);
			printf("sweetsetup: delete %s: %s\n", e->fts_path, rc ? strerror(errno) : "ok");
			n += rc == 0;
		}
	}
	fts_close(fts);
	return n;
}

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
	if (access(MARKER, F_OK) == 0) {
		printf("SWEETSETUP DONE: already set up (%s), nothing to do\n", MARKER);
		return 0;
	}
	long removed = remove_jobs();
	if (removed < 0)
		return fail("editing " PLIST);
	mkdirs(FLAG_DIR);
	if (write_all(FLAG, flag_xml, sizeof(flag_xml) - 1, 0644) != 0 || chown(FLAG, 0, 0) != 0 || chmod(FLAG, 0644) != 0)
		return fail(FLAG);
	printf("sweetsetup: wrote %s\n", FLAG);
	int stores = delete_stores();
	if (stores < 0)
		return fail("searching the containers");
	int killed = kill_named("lsd");
	mkdirs(MARKER_DIR);
	if (write_all(MARKER, "1\n", 2, 0644) != 0)
		return fail(MARKER);
	sync();
	printf("SWEETSETUP DONE: %ld jobs removed, %d store file(s) deleted, %d lsd killed; reboot once it has seeded\n",
	    removed, stores, killed);
	return 0;
}
