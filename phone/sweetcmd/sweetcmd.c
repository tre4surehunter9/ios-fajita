/*
 * sweetcmd: run programs on the phone from the PC, over USB Ethernet.
 *
 * Listens on 192.168.7.2:2224 (the USB address only, never Wi-Fi), as root.
 * Each connection is one command:
 *
 *   RUN <argc>\n  then <argc> lines, one argument each (spaces allowed)
 *
 * The program's stdout and stderr stream back as they come, then one trailer
 * line "\n@@SWEETCMD EXIT <code>\n" (or "SIGNAL <n>", or "ERR <errno>" when it
 * could not start). Closing the connection kills the program (SIGKILL).
 * stdin is /dev/null. PATH covers /usr/local/bin. tools/sweet-phone.py run.
 *
 * Started from the boot payload through sweetrun (mkpayload.py --run
 * sweetcmd), so it needs no root change; it waits until ue0 has 192.168.7.2.
 * Files go through the kernel's file server on port 2223 (bsd/dev/sweet_files.c).
 */
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define PORT     2224
#define MAX_ARGS 64
#define MAX_LINE 1024

static int
read_line(int fd, char *buf, size_t max)
{
	size_t n = 0;
	while (n + 1 < max) {
		char c;
		ssize_t r = read(fd, &c, 1);
		if (r <= 0) {
			return -1;
		}
		if (c == '\n') {
			break;
		}
		buf[n++] = c;
	}
	buf[n] = 0;
	return (int)n;
}

static int
send_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;
	while (len > 0) {
		ssize_t w = write(fd, p, len);
		if (w < 0 && errno == EINTR) {
			continue;
		}
		if (w <= 0) {
			return -1;
		}
		p += w;
		len -= (size_t)w;
	}
	return 0;
}

static void
trailer(int so, const char *what, int value)
{
	char line[64];
	int n = snprintf(line, sizeof(line), "\n@@SWEETCMD %s %d\n", what, value);
	(void)send_all(so, line, (size_t)n);
}

static void *
serve(void *arg)
{
	int so = (int)(long)arg;
	char line[MAX_LINE], *argv[MAX_ARGS + 1];
	int argc = 0;
	memset(argv, 0, sizeof(argv));
	if (read_line(so, line, sizeof(line)) < 0 || strncmp(line, "RUN ", 4) != 0 ||
	    (argc = atoi(line + 4)) <= 0 || argc > MAX_ARGS) {
		trailer(so, "ERR", EINVAL);
		close(so);
		return NULL;
	}
	for (int i = 0; i < argc; i++) {
		if (read_line(so, line, sizeof(line)) < 0 || !(argv[i] = strdup(line))) {
			trailer(so, "ERR", EINVAL);
			goto out;
		}
	}
	printf("sweetcmd: run %s (%d args)\n", argv[0], argc);

	int out[2];
	if (pipe(out) != 0) {
		trailer(so, "ERR", errno);
		goto out;
	}
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_adddup2(&fa, out[1], 1);
	posix_spawn_file_actions_adddup2(&fa, out[1], 2);
	posix_spawn_file_actions_addclose(&fa, out[0]);
	posix_spawn_file_actions_addclose(&fa, out[1]);
	posix_spawn_file_actions_addclose(&fa, so);
	pid_t pid;
	int rc = strchr(argv[0], '/') ?
	    posix_spawn(&pid, argv[0], &fa, NULL, argv, environ) :
	    posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	close(out[1]);
	if (rc != 0) {
		close(out[0]);
		trailer(so, "ERR", rc);
		goto out;
	}

	/* Relay output; a closed or broken connection kills the program. */
	char buf[16384];
	struct pollfd pf[2] = {{.fd = out[0], .events = POLLIN}, {.fd = so, .events = POLLIN}};
	bool alive = true;
	for (;;) {
		if (poll(pf, 2, -1) < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if (pf[1].revents & (POLLIN | POLLHUP | POLLERR)) {
			char c;
			if (read(so, &c, 1) <= 0) {
				alive = false;
				kill(pid, SIGKILL);
				break;
			}
		}
		if (pf[0].revents & (POLLIN | POLLHUP | POLLERR)) {
			ssize_t r = read(out[0], buf, sizeof(buf));
			if (r <= 0) {
				break;  /* program closed its output (usually: exited) */
			}
			if (send_all(so, buf, (size_t)r) != 0) {
				alive = false;
				kill(pid, SIGKILL);
				break;
			}
		}
	}
	close(out[0]);
	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
	}
	if (alive) {
		if (WIFEXITED(status)) {
			trailer(so, "EXIT", WEXITSTATUS(status));
		} else if (WIFSIGNALED(status)) {
			trailer(so, "SIGNAL", WTERMSIG(status));
		}
	}
	printf("sweetcmd: %s done (status %#x%s)\n", argv[0], status, alive ? "" : ", client gone");
out:
	for (int i = 0; i < argc; i++) {
		free(argv[i]);
	}
	close(so);
	return NULL;
}

int
main(void)
{
	signal(SIGPIPE, SIG_IGN);
	setenv("PATH", "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin", 1);
	setvbuf(stdout, NULL, _IOLBF, 0);

	int ls = -1;
	struct sockaddr_in me = {.sin_len = sizeof(me), .sin_family = AF_INET,
	    .sin_port = htons(PORT)};
	me.sin_addr.s_addr = htonl(0xc0a80702);  /* 192.168.7.2: ue0 only */
	for (unsigned tries = 0;; tries++) {
		int one = 1;
		ls = socket(AF_INET, SOCK_STREAM, 0);
		if (ls >= 0 && setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0 &&
		    bind(ls, (struct sockaddr *)&me, sizeof(me)) == 0 && listen(ls, 8) == 0) {
			break;
		}
		if (tries % 30 == 0) {
			printf("sweetcmd: waiting for 192.168.7.2 (%s)\n", strerror(errno));
		}
		if (ls >= 0) {
			close(ls);
		}
		sleep(2);
	}
	printf("sweetcmd: listening on 192.168.7.2 tcp %d\n", PORT);
	for (;;) {
		int c = accept(ls, NULL, NULL);
		if (c < 0) {
			if (errno != EINTR) {
				sleep(1);
			}
			continue;
		}
		int one = 1;
		(void)setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		pthread_t t;
		if (pthread_create(&t, NULL, serve, (void *)(long)c) == 0) {
			pthread_detach(t);
		} else {
			close(c);
		}
	}
}
