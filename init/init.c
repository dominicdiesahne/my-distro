/*
 * init.c - PID 1 for my-distro.
 *
 * The kernel starts this program first, and it must never exit
 * (if PID 1 exits, the kernel panics). Its jobs:
 *   1. mount the virtual filesystems (/proc, /sys, /dev, /tmp)
 *   2. set the hostname from /etc/hostname
 *   3. run the startup scripts in /etc/init.d, if the system has them
 *      (on the desktop build they start udev and the graphical desktop)
 *   4. start a shell on the console, and start a new one when it exits
 *   5. reap orphaned processes: when a parent dies, the kernel hands its
 *      children to PID 1, and we must wait() on them or they stay zombies
 *   6. shut the machine down on `poweroff`, `reboot` or `halt`
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

enum shutdown_kind { RUNNING, HALT, POWEROFF, REBOOT };

static volatile sig_atomic_t shutdown_requested = RUNNING;

/* BusyBox's halt/poweroff/reboot commands tell init what to do by signal. */
static void on_signal(int sig)
{
	switch (sig) {
	case SIGUSR1: shutdown_requested = HALT;     break;
	case SIGUSR2: shutdown_requested = POWEROFF; break;
	case SIGTERM: shutdown_requested = REBOOT;   break;
	}
}

static void mount_fs(const char *source, const char *target, const char *type)
{
	mkdir(target, 0755);
	if (mount(source, target, type, 0, NULL) < 0 && errno != EBUSY)
		fprintf(stderr, "init: mount %s: %s\n", target, strerror(errno));
}

static void set_hostname(void)
{
	char name[64] = "my-distro";
	FILE *f = fopen("/etc/hostname", "r");
	if (f) {
		if (fgets(name, sizeof name, f))
			name[strcspn(name, "\n")] = '\0';
		fclose(f);
	}
	sethostname(name, strlen(name));
}

/*
 * /dev/console can't be a controlling terminal, so the shell would have no
 * job control (Ctrl-C, Ctrl-Z). Use the real device behind it instead:
 * /sys/class/tty/console/active lists the consoles, the last one is the main
 * one (e.g. "tty0 ttyS0" -> /dev/ttyS0).
 */
static void console_path(char *buf, size_t len)
{
	snprintf(buf, len, "/dev/console");
	FILE *f = fopen("/sys/class/tty/console/active", "r");
	if (!f)
		return;
	char line[128];
	if (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\n")] = '\0';
		char *last = strrchr(line, ' ');
		last = last ? last + 1 : line;
		if (*last)
			snprintf(buf, len, "/dev/%s", last);
	}
	fclose(f);
}

/*
 * /etc/init.d/rcS runs every /etc/init.d/S??* script with "start", in order.
 * The minimal initramfs has none, so this does nothing there.
 */
static void run_startup_scripts(void)
{
	const char *rcs = "/etc/init.d/rcS";
	if (access(rcs, X_OK) < 0)
		return;

	printf("init: running %s\n", rcs);
	pid_t pid = fork();
	if (pid == 0) {
		char *argv[] = { (char *)rcs, NULL };
		char *envp[] = { "PATH=/sbin:/bin:/usr/sbin:/usr/bin", NULL };
		execve(rcs, argv, envp);
		perror("init: exec rcS");
		_exit(127);
	}
	if (pid > 0)
		waitpid(pid, NULL, 0);
}

static pid_t spawn_shell(void)
{
	pid_t pid = fork();
	if (pid != 0)
		return pid; /* parent (or -1 if fork failed) */

	/* Child: new session, then take the console as controlling terminal. */
	setsid();
	char tty[160];
	console_path(tty, sizeof tty);
	int fd = open(tty, O_RDWR);
	if (fd >= 0) {
		ioctl(fd, TIOCSCTTY, 1);
		dup2(fd, 0);
		dup2(fd, 1);
		dup2(fd, 2);
		if (fd > 2)
			close(fd);
	}

	char *envp[] = { "HOME=/root", "PATH=/sbin:/bin:/usr/sbin:/usr/bin",
			 "TERM=vt100", NULL };

	/* Systems with passwords (/etc/shadow, the desktop version) ask for a
	 * user name and password first. login starts that user's shell. */
	if (access("/etc/shadow", F_OK) == 0 && access("/bin/login", X_OK) == 0) {
		char *largv[] = { "login", NULL };
		execve("/bin/login", largv, envp);
		perror("init: exec /bin/login");
	}

	chdir("/root");
	/* A leading '-' makes it a login shell, so it reads /etc/profile. */
	char *argv[] = { "-sh", NULL };
	execve("/bin/sh", argv, envp);
	perror("init: exec /bin/sh");
	_exit(127);
}

static void shut_down(enum shutdown_kind kind)
{
	printf("init: sending all processes the TERM signal\n");
	kill(-1, SIGTERM); /* -1 = every process except init itself */
	sleep(1);
	kill(-1, SIGKILL);
	while (waitpid(-1, NULL, WNOHANG) > 0)
		;
	sync();

	/* On a system installed to disk (my-install), make the root filesystem
	 * read-only now that nothing writes to it any more: the disk is then
	 * clean at the next boot. The live system's root is in RAM and can't be
	 * remounted, which is fine. */
	if (mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY, NULL) == 0)
		printf("init: root filesystem is read-only\n");
	sync();

	switch (kind) {
	case HALT:     printf("init: halting\n");     reboot(RB_HALT_SYSTEM); break;
	case POWEROFF: printf("init: powering off\n"); reboot(RB_POWER_OFF);  break;
	default:       printf("init: rebooting\n");   reboot(RB_AUTOBOOT);    break;
	}
	for (;;) /* only reached if reboot() failed; PID 1 must not exit */
		pause();
}

int main(void)
{
	if (getpid() != 1) {
		fprintf(stderr, "init: must be run as PID 1 by the kernel\n");
		return 1;
	}

	printf("init: mounting filesystems\n");
	mount_fs("proc", "/proc", "proc");
	mount_fs("sysfs", "/sys", "sysfs");
	mount_fs("devtmpfs", "/dev", "devtmpfs");
	mount_fs("devpts", "/dev/pts", "devpts");
	mount_fs("tmpfs", "/tmp", "tmpfs");
	set_hostname();

	/* No SA_RESTART: a signal interrupts wait() so we notice it right away. */
	struct sigaction sa = { .sa_handler = on_signal };
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGUSR2, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	run_startup_scripts();

	printf("init: starting shell\n");
	pid_t shell = spawn_shell();

	while (shutdown_requested == RUNNING) {
		if (shell < 0) {
			sleep(1);
			shell = spawn_shell();
			continue;
		}
		pid_t pid = wait(NULL);
		if (pid == shell) {
			printf("init: shell exited, starting a new one\n");
			shell = spawn_shell();
		}
		/* Any other pid was an orphan we just reaped. */
	}

	shut_down(shutdown_requested);
	return 0;
}
