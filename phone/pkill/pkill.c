/*
 * pkill: signal processes by exact name (iOS ships no killall/pkill).
 *   pkill NAME [SIGNAL]      default SIGTERM; prints each pid it signalled; exit 1 if none matched
 * Used to restart a launchd KeepAlive daemon (e.g. configd after its preferences changed): launchd
 * starts it again by itself.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: pkill NAME [SIGNAL]\n");
		return 2;
	}
	int sig = argc > 2 ? atoi(argv[2]) : SIGTERM;
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0 };
	size_t len = 0;
	if (sysctl(mib, 3, NULL, &len, NULL, 0) != 0) {
		perror("sysctl");
		return 1;
	}
	len += len / 4;
	struct kinfo_proc *kp = malloc(len);
	if (!kp || sysctl(mib, 3, kp, &len, NULL, 0) != 0) {
		perror("sysctl");
		return 1;
	}
	int n = (int)(len / sizeof(*kp)), hits = 0;
	for (int i = 0; i < n; i++) {
		pid_t pid = kp[i].kp_proc.p_pid;
		if (pid <= 1 || pid == getpid() || strncmp(kp[i].kp_proc.p_comm, argv[1], MAXCOMLEN) != 0)
			continue;
		if (kill(pid, sig) == 0) {
			printf("pkill: %s[%d] <- signal %d\n", argv[1], pid, sig);
			hits++;
		} else {
			printf("pkill: %s[%d]: %s\n", argv[1], pid, strerror(errno));
		}
	}
	if (!hits)
		printf("pkill: no process named %s\n", argv[1]);
	return hits ? 0 : 1;
}
