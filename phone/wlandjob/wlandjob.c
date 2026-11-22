/*
 * wlandjob: the Wi-Fi boot job. Adds (or removes) launchd job com.sweet.wland in
 * /System/Library/xpc/launchd.plist, which runs `/var/local/bin/wland auto` once per boot as root:
 * wland's own boot mode (phone/wifi/wland.c auto_up): once per phone boot, off with /var/local/wland.off
 * or boot-arg sweetwifi=0, log in /var/local/wland.log (.prev = the boot before).
 *
 *   wlandjob install     add the job (the original job list is kept once as launchd.plist.pre-wland)
 *   wlandjob remove      take it out again
 *   wlandjob show        print the job if it is there
 *
 * Written the way phone/quiet does it: same format, owner and mode, a temp file renamed over the original.
 * launchd reads its job list at boot: reboot afterwards. Run it as root (sweetcmd or sweetrun).
 * Needs kernel 0135+ (/dev/pmem with plain device accesses) and wland, netsvc, the firmware and
 * /var/local/wlan.conf on the phone (phone/wifi/README.md).
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
#define BACKUP	PLIST ".pre-wland"
#define TMP	PLIST ".wlandjob"
#define KEY	"/System/Library/LaunchDaemons/com.sweet.wland.plist"
#define WLAND	"/var/local/bin/wland"

static int fail(const char *what)
{
	printf("wlandjob: %s: %s\nWLANDJOB FAIL\n", what, strerror(errno));
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

static CFMutableDictionaryRef job(void)
{
	CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFStringRef args[] = { CFSTR(WLAND), CFSTR("auto") };
	CFArrayRef argv = CFArrayCreate(NULL, (const void **)args, 2, &kCFTypeArrayCallBacks);
	CFDictionarySetValue(d, CFSTR("Label"), CFSTR("com.sweet.wland"));
	CFDictionarySetValue(d, CFSTR("Program"), CFSTR(WLAND));
	CFDictionarySetValue(d, CFSTR("ProgramArguments"), argv);
	CFDictionarySetValue(d, CFSTR("RunAtLoad"), kCFBooleanTrue);
	CFDictionarySetValue(d, CFSTR("LaunchOnlyOnce"), kCFBooleanTrue);
	CFDictionarySetValue(d, CFSTR("UserName"), CFSTR("root"));
	CFDictionarySetValue(d, CFSTR("ProcessType"), CFSTR("Interactive"));
	CFRelease(argv);
	return d;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	const char *cmd = argc > 1 ? argv[1] : "";
	int install = !strcmp(cmd, "install"), remove = !strcmp(cmd, "remove"), show = !strcmp(cmd, "show");
	if (!install && !remove && !show) {
		printf("usage: wlandjob install | remove | show\n");
		return 2;
	}
	if (install && access(WLAND, X_OK) != 0)
		return fail(WLAND);
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
	CFStringRef key = CFSTR(KEY);
	Boolean present = CFDictionaryContainsKey(jobs, key);
	if (show) {
		printf("wlandjob: com.sweet.wland is %s (%ld jobs)\n", present ? "installed" : "not installed",
		    (long)CFDictionaryGetCount(jobs));
		if (present)
			CFShow(CFDictionaryGetValue(jobs, key));
		return 0;
	}
	if (install == present) {
		printf("wlandjob: already %s\nWLANDJOB DONE: nothing to do\n", present ? "installed" : "removed");
		return 0;
	}
	if (install) {
		CFMutableDictionaryRef j = job();
		CFDictionarySetValue(jobs, key, j);
		CFRelease(j);
	} else {
		CFDictionaryRemoveValue(jobs, key);
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
	printf("wlandjob: %s com.sweet.wland (%ld jobs now; the first original is kept as %s)\n",
	    install ? "added" : "removed", (long)CFDictionaryGetCount(jobs), BACKUP);
	printf("WLANDJOB DONE: %s; reboot for launchd to read it\n", install ? "installed" : "removed");
	return 0;
}
