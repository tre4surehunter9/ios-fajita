/*
 * unquiet: put launchd jobs that phone/quiet took out back into the root's job list, once.
 *
 * Copies each job below from /System/Library/xpc/launchd.plist.pre-quiet (quiet's backup) into
 * /System/Library/xpc/launchd.plist under its original key, keeping the file's format, owner and mode.
 * Jobs already present are left alone. Takes effect at the next boot. Last line "UNQUIET DONE ..." or
 * "UNQUIET FAIL ...".
 *
 * com.apple.bluetoothd: with it gone (0123 after quiet), launchd went from ~28 % to ~72 % of a core and
 * mediaserverd to ~47 %, mediaserverd sitting in xpc_pipe requests to launchd: most likely looking up the
 * missing Bluetooth service over and over. ios-apollo keeps bluetoothd and turns off fairplayd.H2.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PLIST	"/System/Library/xpc/launchd.plist"
#define BACKUP	PLIST ".pre-quiet"
#define TMP	PLIST ".unquiet-tmp"

static const char *const labels[] = {
	"com.apple.bluetoothd",
};
#define NLABELS (sizeof(labels) / sizeof(labels[0]))

static int fail(const char *what)
{
	printf("unquiet: %s: %s\nUNQUIET FAIL\n", what, strerror(errno));
	return 1;
}

/* The whole file as a mutable property list; *format and *st describe it. */
static CFMutableDictionaryRef load(const char *path, CFPropertyListFormat *format, struct stat *st)
{
	if (stat(path, st) != 0)
		return NULL;
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return NULL;
	uint8_t *buf = malloc((size_t)st->st_size);
	if (!buf || read(fd, buf, (size_t)st->st_size) != st->st_size) {
		close(fd);
		return NULL;
	}
	close(fd);
	CFDataRef data = CFDataCreate(NULL, buf, st->st_size);
	free(buf);
	CFPropertyListRef plist = CFPropertyListCreateWithData(NULL, data,
	    kCFPropertyListMutableContainersAndLeaves, format, NULL);
	CFRelease(data);
	if (!plist || CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
		errno = EINVAL;
		return NULL;
	}
	return (CFMutableDictionaryRef)plist;
}

static int has_label(CFDictionaryRef job, const char *want)
{
	if (CFGetTypeID(job) != CFDictionaryGetTypeID())
		return 0;
	CFStringRef label = CFDictionaryGetValue(job, CFSTR("Label"));
	char l[128];
	return label && CFGetTypeID(label) == CFStringGetTypeID() &&
	    CFStringGetCString(label, l, sizeof(l), kCFStringEncodingUTF8) && strcmp(l, want) == 0;
}

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	CFPropertyListFormat format = 0, bformat = 0;
	struct stat st, bst;
	CFMutableDictionaryRef cur = load(PLIST, &format, &st), old = load(BACKUP, &bformat, &bst);
	if (!cur)
		return fail(PLIST);
	if (!old)
		return fail(BACKUP);
	CFMutableDictionaryRef jobs = (CFMutableDictionaryRef)CFDictionaryGetValue(cur, CFSTR("LaunchDaemons"));
	CFDictionaryRef ojobs = CFDictionaryGetValue(old, CFSTR("LaunchDaemons"));
	if (!jobs || !ojobs) {
		errno = EINVAL;
		return fail("no LaunchDaemons dictionary");
	}

	CFIndex n = CFDictionaryGetCount(ojobs);
	const void **keys = malloc(sizeof(void *) * (size_t)n), **vals = malloc(sizeof(void *) * (size_t)n);
	CFDictionaryGetKeysAndValues(ojobs, keys, vals);
	int added = 0;
	for (size_t k = 0; k < NLABELS; k++) {
		for (CFIndex i = 0; i < n; i++) {
			if (!has_label(vals[i], labels[k]))
				continue;
			if (CFDictionaryContainsKey(jobs, keys[i])) {
				printf("unquiet: %s already in the job list\n", labels[k]);
			} else {
				CFDictionarySetValue(jobs, keys[i], vals[i]);
				printf("unquiet: put back %s\n", labels[k]);
				added++;
			}
		}
	}
	if (added == 0) {
		printf("UNQUIET DONE: nothing to put back\n");
		return 0;
	}

	CFDataRef out = CFPropertyListCreateData(NULL, cur, format, 0, NULL);
	if (!out) {
		errno = EINVAL;
		return fail("serializing");
	}
	int fd = open(TMP, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 07777);
	if (fd < 0)
		return fail(TMP);
	const uint8_t *p = CFDataGetBytePtr(out);
	size_t left = (size_t)CFDataGetLength(out);
	while (left) {
		ssize_t w = write(fd, p, left);
		if (w <= 0)
			return fail(TMP);
		p += w;
		left -= (size_t)w;
	}
	fsync(fd);
	close(fd);
	if (chown(TMP, st.st_uid, st.st_gid) != 0 || chmod(TMP, st.st_mode & 07777) != 0)
		return fail("owner/mode of " TMP);
	if (rename(TMP, PLIST) != 0)
		return fail("rename over " PLIST);
	sync();
	printf("unquiet: %s: %ld jobs now\n", PLIST, (long)CFDictionaryGetCount(jobs));
	printf("UNQUIET DONE: %d job(s) put back; reboot for it to take effect\n", added);
	return 0;
}
