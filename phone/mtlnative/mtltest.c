/*
 * mtltest: first run of the native Metal route (AIR -> NIR -> ir3 -> freedreno -> SweetGPU) on the phone.
 * Run once through sweetrun, on a boot without sweetmtl so it is the GPU's only client. Unpacks the blob (pack.py)
 * to /private/var/tmp/sweetrun, then runs Chlorophyll's two checks of the library, with no Metal framework involved:
 *   bgair_test         compute: three kernels of ours, results checked
 *   bgair_vertex_test  render: QuartzCore's real std_vert0_lpf + a fragment of ours, pixels checked
 * Last line MTLTEST PASS or MTLTEST FAIL. A watchdog ends a hung run.
 */
#include <compression.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DIR_ "/private/var/tmp/sweetrun/"
#define QUARTZCORE "/System/Library/Frameworks/QuartzCore.framework/default.metallib"
#define WATCHDOG_S 180

extern const uint8_t mtl_blob_start[], mtl_blob_end[];

struct entry { char name[56]; uint64_t raw, packed; };

static int unpack(void)
{
	const uint8_t *p = mtl_blob_start;
	while (p + sizeof(struct entry) <= mtl_blob_end) {
		struct entry e;
		memcpy(&e, p, sizeof(e));
		p += sizeof(e);
		uint8_t *buf = malloc(e.raw);
		if (buf == NULL || compression_decode_buffer(buf, e.raw, p, e.packed, NULL, COMPRESSION_ZLIB) != e.raw) {
			printf("FAIL unpacking %s\n", e.name);
			return -1;
		}
		char path[128];
		snprintf(path, sizeof(path), DIR_ "%s", e.name);
		unlink(path);
		int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
		if (fd < 0 || write(fd, buf, e.raw) != (ssize_t)e.raw) {
			printf("FAIL writing %s\n", path);
			return -1;
		}
		close(fd);
		free(buf);
		printf("unpacked %s: %llu bytes\n", e.name, (unsigned long long)e.raw);
		p += e.packed;
	}
	return 0;
}

static void *watchdog(void *arg)
{
	(void)arg;
	sleep(WATCHDOG_S);
	printf("watchdog: no result after %d s\nMTLTEST FAIL\n", WATCHDOG_S);
	fflush(stdout);
	_exit(9);
}

static int run(const char *lib, const char *arg1, const char *arg2)
{
	char path[128], *argv[4] = { (char *)lib, (char *)arg1, (char *)arg2, NULL };
	snprintf(path, sizeof(path), DIR_ "%s", lib);
	printf("== %s\n", lib);
	void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (h == NULL) {
		printf("FAIL dlopen: %s\n", dlerror());
		return 1;
	}
	int (*fn)(int, char **) = (int (*)(int, char **))dlsym(h, "test_main");
	if (fn == NULL) {
		printf("FAIL no test_main in %s\n", lib);
		return 1;
	}
	int r = fn(arg2 ? 3 : 2, argv);
	printf("== %s: exit %d\n", lib, r);
	return r;
}

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IOLBF, 0);
	dup2(1, 2);
	mkdir(DIR_, 0755);
	pthread_t t;
	pthread_create(&t, NULL, watchdog, NULL);
	if (unpack() != 0) {
		printf("MTLTEST FAIL\n");
		return 1;
	}
	int bad = run("bgair_test.dylib", DIR_ "bg_kernels.metallib", NULL);
	bad |= run("bgair_vertex_test.dylib", QUARTZCORE, DIR_ "bg_render.metallib");
	printf("MTLTEST %s\n", bad ? "FAIL" : "PASS");
	fflush(stdout);
	_exit(bad ? 1 : 0);     /* no teardown: a clean exit is not what this run tests */
}
