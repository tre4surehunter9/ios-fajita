/*
 * sweetrun: run the test program that rides in the boot payload.
 *
 * Installed once on the root (/usr/local/bin/sweetrun, LaunchDaemon
 * com.sweet.run from tools/sweet-root-tweaks.py --sweetrun). At boot it reads
 * the ADT's /chosen "sweetrun" property (a Mach-O, put there by
 * tools/mkpayload.py --run) through the IODeviceTree plane, writes it to
 * /private/var/tmp/sweetrun/<sweetrun-name>, and runs it with its output on
 * /dev/console, so it reaches the screen console and the USB log. No
 * property: it does nothing. So a new test program needs a new boot image
 * only, never a new root.
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

/* IOKit has no headers in the iPhoneOS SDK; the functions are in IOKit.framework. */
typedef mach_port_t io_object_t;
typedef io_object_t io_registry_entry_t;
extern io_registry_entry_t IORegistryEntryFromPath(mach_port_t mainPort, const char *path);
extern CFTypeRef IORegistryEntryCreateCFProperty(io_registry_entry_t entry, CFStringRef key, CFAllocatorRef allocator,
    uint32_t options);
extern kern_return_t IOObjectRelease(io_object_t object);

extern char **environ;

#define DIR_PATH "/private/var/tmp/sweetrun"

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
	int con = open("/dev/console", O_WRONLY);
	if (con >= 0) {
		dup2(con, STDOUT_FILENO);
		dup2(con, STDERR_FILENO);
		if (con > STDERR_FILENO) {
			close(con);
		}
	}
	setvbuf(stdout, NULL, _IOLBF, 0);

	io_registry_entry_t chosen = IORegistryEntryFromPath(MACH_PORT_NULL, "IODeviceTree:/chosen");
	if (chosen == MACH_PORT_NULL) {
		printf("sweetrun: no IODeviceTree:/chosen\n");
		return 1;
	}
	CFDataRef prog = chosen_data(chosen, "sweetrun");
	CFDataRef name = chosen_data(chosen, "sweetrun-name");
	IOObjectRelease(chosen);
	if (prog == NULL) {
		printf("sweetrun: nothing to run (no /chosen sweetrun)\n");
		return 0;
	}

	char base[64] = "program";
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
	snprintf(path, sizeof(path), DIR_PATH "/%s", base);
	mkdir(DIR_PATH, 0755);
	unlink(path);
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (fd < 0) {
		printf("sweetrun: cannot create %s: %s\n", path, strerror(errno));
		CFRelease(prog);
		return 1;
	}
	const uint8_t *p = CFDataGetBytePtr(prog);
	size_t left = (size_t)CFDataGetLength(prog);
	while (left > 0) {
		ssize_t w = write(fd, p, left);
		if (w <= 0) {
			printf("sweetrun: write %s: %s\n", path, strerror(errno));
			close(fd);
			CFRelease(prog);
			return 1;
		}
		p += w;
		left -= (size_t)w;
	}
	fchmod(fd, 0755);
	close(fd);
	printf("sweetrun: running %s (%ld bytes)\n", path, (long)CFDataGetLength(prog));
	CFRelease(prog);

	char *argv[] = { path, NULL };
	pid_t pid;
	int rc = posix_spawn(&pid, path, NULL, NULL, argv, environ);
	if (rc != 0) {
		printf("sweetrun: spawn %s: %s\n", path, strerror(rc));
		return 1;
	}
	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
	}
	if (WIFEXITED(status)) {
		printf("sweetrun: %s exited %d\n", base, WEXITSTATUS(status));
	} else if (WIFSIGNALED(status)) {
		printf("sweetrun: %s killed by signal %d\n", base, WTERMSIG(status));
	}
	return 0;
}
