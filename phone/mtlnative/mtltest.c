/*
 * mtltest: the native Metal route (AIR -> NIR -> ir3 -> freedreno -> SweetGPU) on the phone, no system process involved.
 * Run once through sweetrun, on a boot without sweetmtl so it is the GPU's only client. Unpacks the blob (pack.py) to
 * /private/var/tmp/sweetrun, then runs Chlorophyll's checks, each in its own process so a crash ends only that one:
 *   bgair_test              the library alone: three compute kernels
 *   bgair_vertex_test       the library alone: QuartzCore's real std_vert0_lpf + a fragment of ours
 *   bgmtl_probe             the Metal device (BGMTLDevice) on iOS 16's Metal.framework: queue, buffers, compute, blit
 *   bgmtl_render_probe      the Metal device: textures, samplers, a render pass
 *   bgmtl_quad_probe        the Metal device: indexed quads laid out as CoreAnimation does
 *   bgmtl_carenderer_probe  iOS 16's CoreAnimation itself rendering offscreen on the Metal device, against its CPU renderer
 * Last line MTLTEST PASS or MTLTEST FAIL, with one "TEST name: ..." line per check.
 */
#include <compression.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define DIR_ "/private/var/tmp/sweetrun/"
#define QUARTZCORE "/System/Library/Frameworks/QuartzCore.framework/default.metallib"
#define TIMEOUT_S 120

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

/* The child loads the test and returns its exit code. */
static void child(const char *lib, const char *arg1, const char *arg2)
{
	char path[128], *argv[4] = { (char *)lib, (char *)arg1, (char *)arg2, NULL };
	snprintf(path, sizeof(path), DIR_ "%s.dylib", lib);
	void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (h == NULL) {
		printf("FAIL dlopen: %s\n", dlerror());
		_exit(120);
	}
	int (*fn)(int, char **) = (int (*)(int, char **))dlsym(h, "test_main");
	if (fn == NULL) {
		printf("FAIL no test_main in %s\n", lib);
		_exit(121);
	}
	int r = fn(arg1 == NULL ? 1 : arg2 == NULL ? 2 : 3, argv);
	fflush(stdout);
	_exit(r);
}

static int run(const char *lib, const char *arg1, const char *arg2)
{
	printf("== %s\n", lib);
	pid_t pid = fork();
	if (pid < 0) {
		printf("TEST %s: fork failed\n", lib);
		return 1;
	}
	if (pid == 0) {
		child(lib, arg1, arg2);
	}
	int st = 0;
	for (int ms = 0; ; ms += 100) {
		pid_t w = waitpid(pid, &st, WNOHANG);
		if (w == pid) {
			break;
		}
		if (ms >= TIMEOUT_S * 1000) {
			kill(pid, SIGKILL);
			waitpid(pid, &st, 0);
			printf("TEST %s: TIMEOUT after %d s\n", lib, TIMEOUT_S);
			return 1;
		}
		usleep(100000);
	}
	if (WIFSIGNALED(st)) {
		printf("TEST %s: CRASH signal %d\n", lib, WTERMSIG(st));
		return 1;
	}
	printf("TEST %s: exit %d\n", lib, WEXITSTATUS(st));
	return WEXITSTATUS(st) != 0;
}

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IOLBF, 0);
	dup2(1, 2);
	mkdir(DIR_, 0755);
	mkdir(DIR_ "ca-out", 0755);
	if (unpack() != 0) {
		printf("MTLTEST FAIL\n");
		return 1;
	}
	int bad = run("bgair_test", DIR_ "bg_kernels.metallib", NULL);
	bad |= run("bgair_vertex_test", QUARTZCORE, DIR_ "bg_render.metallib");
	bad |= run("bgmtl_probe", DIR_ "bg_kernels.metallib", NULL);
	bad |= run("bgmtl_render_probe", DIR_ "bg_kernels.metallib", DIR_ "bg_render.metallib");
	bad |= run("bgmtl_quad_probe", DIR_ "bg_quad.metallib", NULL);
	bad |= run("bgmtl_carenderer_probe", DIR_ "ca-out", NULL);
	printf("MTLTEST %s\n", bad ? "FAIL" : "PASS");
	fflush(stdout);
	return bad ? 1 : 0;
}
