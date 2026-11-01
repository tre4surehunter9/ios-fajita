/*
 * extfix: let LaunchServices register the app extensions (wallpapers, widgets), once.
 *
 * Runs at boot through sweetrun (root; the root filesystem is writable with rootrw=1). Needs kernel 0124,
 * which answers lsd's __mac_syscall("AMFI", 0x60) so the extension catalog gets read. Then:
 *  - writes /private/var/preferences/FeatureFlags/Domain/ExtensionKit.plist with
 *    enforce_xpc_cache_code_signing off (on in /System/Library/FeatureFlags/Domain/ExtensionKit.plist);
 *  - deletes LaunchServices' database, which was seeded without the extensions and is not seeded again
 *    while it looks complete. Its container has a per-phone UUID, so the container trees are searched;
 *  - kills lsd so it cannot write the old database back; launchd restarts it and it seeds a new one.
 * Facts from ios-apollo's and Bergamota's notes on the same 20H392 build. A reboot afterwards gives
 * every process the new database. Last line "EXTFIX DONE ..." or "EXTFIX FAIL ...".
 */
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define FLAG_DIR "/private/var/preferences/FeatureFlags/Domain"
#define FLAG FLAG_DIR "/ExtensionKit.plist"
#define STORE_PREFIX "com.apple.LaunchServices"

static const char flag_xml[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n<dict>\n\t<key>enforce_xpc_cache_code_signing</key>\n\t<dict>\n"
    "\t\t<key>Enabled</key>\n\t\t<false/>\n\t</dict>\n</dict>\n</plist>\n";

static int fail(const char *what)
{
	printf("extfix: %s: %s\nEXTFIX FAIL\n", what, strerror(errno));
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

/* Regular files named com.apple.LaunchServices* under the container trees. */
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
			printf("extfix: delete %s (%lld bytes): %s\n", e->fts_path, (long long)e->fts_statp->st_size,
			    rc ? strerror(errno) : "ok");
			n += rc == 0;
		}
	}
	fts_close(fts);
	return n;
}

/* SIGKILL every process whose name is name (launchd restarts the daemon). */
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
	mkdirs(FLAG_DIR);
	int fd = open(FLAG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return fail(FLAG);
	if (write(fd, flag_xml, sizeof(flag_xml) - 1) != (ssize_t)sizeof(flag_xml) - 1)
		return fail(FLAG);
	fsync(fd);
	close(fd);
	if (chown(FLAG, 0, 0) != 0 || chmod(FLAG, 0644) != 0)
		return fail(FLAG);
	printf("extfix: wrote %s\n", FLAG);
	int stores = delete_stores();
	if (stores < 0)
		return fail("searching the containers");
	int killed = kill_named("lsd");
	sync();
	printf("EXTFIX DONE: %d store file(s) deleted, %d lsd killed; reboot once it has seeded\n", stores, killed);
	return 0;
}
