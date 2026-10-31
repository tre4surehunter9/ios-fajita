/*
 * bigrun: second stage of phone/sweetrun for programs over 16 MB.
 *
 * IOKit serializes a registry property's length in 24 bits
 * (kOSSerializeDataMask, libkern/OSSerializeBinary.h), so a single /chosen
 * property above 16 MB never reaches userland: sweetrun saw "nothing to run"
 * for the 26.7 MB m2vtest. tools/mkpayload.py --run therefore stores a big
 * program as /chosen "bigrun-0", "bigrun-1", ... (8 MB each) plus
 * "bigrun-name", and puts this stub into "sweetrun". It joins the pieces into
 * /private/var/tmp/sweetrun/<name>, runs it (output inherited: the console)
 * and exits with its status.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

typedef mach_port_t io_object_t;
typedef io_object_t io_registry_entry_t;
extern io_registry_entry_t IORegistryEntryFromPath(mach_port_t mainPort, const char *path);
extern CFTypeRef IORegistryEntryCreateCFProperty(io_registry_entry_t entry, CFStringRef key, CFAllocatorRef allocator,
    uint32_t options);
extern kern_return_t IOObjectRelease(io_object_t object);
extern char **environ;

static CFDataRef
chosen_data(io_registry_entry_t chosen, const char *key)
{
	CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
	CFTypeRef v = IORegistryEntryCreateCFProperty(chosen, k, NULL, 0);
	CFRelease(k);
	if (v != NULL && CFGetTypeID(v) != CFDataGetTypeID()) {
		CFRelease(v);
		v = NULL;
	}
	return (CFDataRef)v;
}

int
main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	io_registry_entry_t chosen = IORegistryEntryFromPath(MACH_PORT_NULL, "IODeviceTree:/chosen");
	if (chosen == MACH_PORT_NULL) {
		printf("bigrun: no IODeviceTree:/chosen\n");
		return 1;
	}
	char base[64] = "program";
	CFDataRef name = chosen_data(chosen, "bigrun-name");
	if (name != NULL) {
		size_t n = (size_t)CFDataGetLength(name);
		n = n < sizeof(base) - 1 ? n : sizeof(base) - 1;
		memcpy(base, CFDataGetBytePtr(name), n);
		base[n] = 0;
		for (char *c = base; *c; c++) {
			if (*c == '/') {
				*c = '_';
			}
		}
		CFRelease(name);
	}
	char path[160];
	snprintf(path, sizeof(path), "/private/var/tmp/sweetrun/%s", base);
	mkdir("/private/var/tmp/sweetrun", 0755);
	unlink(path);
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (fd < 0) {
		printf("bigrun: cannot create %s: %s\n", path, strerror(errno));
		return 1;
	}
	size_t total = 0;
	int parts = 0;
	for (;; parts++) {
		char key[32];
		snprintf(key, sizeof(key), "bigrun-%d", parts);
		CFDataRef d = chosen_data(chosen, key);
		if (d == NULL) {
			break;
		}
		const uint8_t *p = CFDataGetBytePtr(d);
		size_t left = (size_t)CFDataGetLength(d);
		while (left > 0) {
			ssize_t w = write(fd, p, left);
			if (w <= 0) {
				printf("bigrun: write %s: %s\n", path, strerror(errno));
				return 1;
			}
			p += w;
			left -= (size_t)w;
			total += (size_t)w;
		}
		CFRelease(d);
	}
	IOObjectRelease(chosen);
	fchmod(fd, 0755);
	close(fd);
	if (parts == 0) {
		printf("bigrun: no /chosen bigrun-0\n");
		return 1;
	}
	printf("bigrun: running %s (%zu bytes from %d parts)\n", path, total, parts);
	char *argv[] = { path, NULL };
	pid_t pid;
	int rc = posix_spawn(&pid, path, NULL, NULL, argv, environ);
	if (rc != 0) {
		printf("bigrun: spawn %s: %s\n", path, strerror(rc));
		return 1;
	}
	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
	}
	if (WIFSIGNALED(status)) {
		printf("bigrun: %s killed by signal %d\n", base, WTERMSIG(status));
		return 128 + WTERMSIG(status);
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
