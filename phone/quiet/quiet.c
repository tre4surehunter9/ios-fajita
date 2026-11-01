/*
 * quiet: take launchd jobs that only burn CPU on this phone out of the root's job list, once.
 *
 * Runs at boot through sweetrun (root; the root filesystem is writable with rootrw=1). launchd reads
 * /System/Library/xpc/launchd.plist at boot (launchd_unsecure_cache=1; tools/sweet-root-tweaks.py edits
 * the same file on the PC). Removed, by Label (checked in the 20H392 job list):
 *   com.apple.voicemail.vmd  voicemail: no modem, spins at ~100% of a core (0123's TOP; ios-apollo saw the same)
 *   com.apple.fairplayd.H2   FairPlay: no FairPlay hardware, crashes and is restarted every ~10 s
 *   com.apple.bluetoothd     no Bluetooth driver here: the same crash/restart loop
 *   com.apple.ReportCrash    writes reports for those loops (crashes still reach the kernel log)
 * The file keeps its plist format, owner and mode; the original is kept once as launchd.plist.pre-quiet.
 * Takes effect at the next boot. Running it again finds nothing to remove. Last line "QUIET DONE ..." or
 * "QUIET FAIL ...".
 */
#include <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PLIST	"/System/Library/xpc/launchd.plist"
#define BACKUP	PLIST ".pre-quiet"
#define TMP	PLIST ".quiet-tmp"

static const char *const labels[] = {
	"com.apple.voicemail.vmd",
	"com.apple.fairplayd.H2",
	"com.apple.bluetoothd",
	"com.apple.ReportCrash",
};
#define NLABELS (sizeof(labels) / sizeof(labels[0]))

static int fail(const char *what)
{
	printf("quiet: %s: %s\nQUIET FAIL\n", what, strerror(errno));
	return 1;
}

static int write_all(const char *path, const uint8_t *p, size_t n, mode_t mode)
{
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

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	struct stat st;
	if (stat(PLIST, &st) != 0)
		return fail(PLIST);
	int fd = open(PLIST, O_RDONLY);
	if (fd < 0)
		return fail(PLIST);
	uint8_t *buf = malloc((size_t)st.st_size);
	if (!buf || read(fd, buf, (size_t)st.st_size) != st.st_size)
		return fail("reading " PLIST);
	close(fd);

	CFDataRef data = CFDataCreateWithBytesNoCopy(NULL, buf, st.st_size, kCFAllocatorNull);
	CFPropertyListFormat format = 0;
	CFErrorRef err = NULL;
	CFPropertyListRef plist = CFPropertyListCreateWithData(NULL, data,
	    kCFPropertyListMutableContainersAndLeaves, &format, &err);
	if (!plist || CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
		errno = EINVAL;
		return fail("parsing " PLIST);
	}
	CFMutableDictionaryRef jobs = (CFMutableDictionaryRef)CFDictionaryGetValue(plist, CFSTR("LaunchDaemons"));
	if (!jobs || CFGetTypeID(jobs) != CFDictionaryGetTypeID()) {
		errno = EINVAL;
		return fail("no LaunchDaemons dictionary");
	}

	CFIndex n = CFDictionaryGetCount(jobs);
	const void **keys = malloc(sizeof(void *) * (size_t)n), **vals = malloc(sizeof(void *) * (size_t)n);
	CFDictionaryGetKeysAndValues(jobs, keys, vals);
	CFIndex removed = 0;
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
				printf("quiet: removed %s\n", l);
				removed++;
			}
		}
	}
	if (removed == 0) {
		printf("quiet: nothing to remove (already done)\nQUIET DONE: 0 jobs removed\n");
		return 0;
	}

	CFDataRef out = CFPropertyListCreateData(NULL, plist, format, 0, &err);
	if (!out) {
		errno = EINVAL;
		return fail("serializing");
	}
	if (access(BACKUP, F_OK) != 0 && write_all(BACKUP, buf, (size_t)st.st_size, st.st_mode & 07777) != 0)
		return fail(BACKUP);
	if (write_all(TMP, CFDataGetBytePtr(out), (size_t)CFDataGetLength(out), st.st_mode & 07777) != 0)
		return fail(TMP);
	if (chown(TMP, st.st_uid, st.st_gid) != 0 || chmod(TMP, st.st_mode & 07777) != 0)
		return fail("owner/mode of " TMP);
	if (rename(TMP, PLIST) != 0)
		return fail("rename over " PLIST);
	sync();
	printf("quiet: %s: %ld jobs left, original kept as %s\n", PLIST, (long)(n - removed), BACKUP);
	printf("QUIET DONE: %ld jobs removed; reboot for it to take effect\n", (long)removed);
	return 0;
}
