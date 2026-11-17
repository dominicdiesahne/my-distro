/*
 * init.c - PID 1 for my-distro.
 *
 * The kernel starts this program first, and it must never exit
 * (if PID 1 exits, the kernel panics). Its jobs:
 *   1. mount the virtual filesystems (/proc, /sys, /dev, /dev/shm, /tmp, /run)
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
#include <linux/magic.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/vfs.h>
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

/*
 * The live system runs straight from the kernel's initramfs: the files sit
 * in "rootfs", the first mount of all, which has no mount above it. The
 * kernel never lets a process pivot_root() away from rootfs, and that is
 * how bubblewrap builds a sandbox (my-sandbox, the app permissions of
 * my-run): "bwrap: pivot_root: Invalid argument".
 *
 * So make / an ordinary mount first, the way switch_root changes roots,
 * but without copying anything: bind-mount rootfs onto a folder of itself,
 * move that mount onto / and make it our root. It is the same filesystem,
 * so the files stay where they are in RAM and no memory is used twice.
 * Every process started from here on has the bind mount as its root,
 * which has a mount above it and can be pivot_root()ed. Installed systems
 * start from a disk (ext4) and skip this.
 */
static void live_root_as_mount(void)
{
	const char *dir = "/.live-root";
	struct statfs fs;

	if (statfs("/", &fs) < 0 ||
	    (fs.f_type != RAMFS_MAGIC && fs.f_type != TMPFS_MAGIC))
		return;
	if (mkdir(dir, 0700) < 0 && errno != EEXIST)
		goto fail;
	if (mount("/", dir, NULL, MS_BIND | MS_REC, NULL) < 0)
		goto fail;
	if (chdir(dir) < 0 || mount(dir, "/", NULL, MS_MOVE, NULL) < 0 ||
	    chroot(".") < 0 || chdir("/") < 0)
		goto fail;
	rmdir(dir);                 /* the empty folder, seen from the new root */
	return;
fail:
	fprintf(stderr, "init: live root as a mount: %s (sandboxes won't work)\n",
		strerror(errno));
	chdir("/");
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
	 * clean at the next boot. (The live system's root is in RAM; making it
	 * read-only there changes nothing.) */
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

	live_root_as_mount();

	printf("init: mounting filesystems\n");
	mount_fs("proc", "/proc", "proc");
	mount_fs("sysfs", "/sys", "sysfs");
	/* Which security modules run (cat /sys/kernel/security/lsm) */
	mount_fs("securityfs", "/sys/kernel/security", "securityfs");
	/* UEFI variables (0.9.7): Secure Boot (mokutil) and firmware updates
	 * (fwupd) read and write them. Only on PCs started with UEFI. */
	if (access("/sys/firmware/efi/efivars", F_OK) == 0)
		mount_fs("efivarfs", "/sys/firmware/efi/efivars", "efivarfs");
	mount_fs("devtmpfs", "/dev", "devtmpfs");
	mount_fs("devpts", "/dev/pts", "devpts");
	/* /dev/shm (0.10.0): POSIX shared memory (shm_open, sem_open) lives
	 * here, and devtmpfs doesn't make the folder. The fstab entry for it is
	 * never used: nothing here runs "mount -a". Without it rootless podman
	 * can't make its locks ("failed to create 2048 locks in
	 * /libpod_rootless_lock_1000"), and browsers and Python's
	 * multiprocessing lose their shared memory too. */
	mkdir("/dev/shm", 01777);
	if (mount("tmpfs", "/dev/shm", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777") < 0 &&
	    errno != EBUSY)
		fprintf(stderr, "init: mount /dev/shm: %s\n", strerror(errno));
	mount_fs("tmpfs", "/tmp", "tmpfs");
	/* /run holds what is only true until the next boot: PID files, and the
	 * list of network interfaces that are up (ifup's /run/ifstate). On a
	 * disk it would survive a reboot, and ifup would then skip eth0 because
	 * the old list says it is up already. In RAM it starts empty. */
	mkdir("/run", 0755);
	if (mount("tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755") < 0 &&
	    errno != EBUSY)
		fprintf(stderr, "init: mount /run: %s\n", strerror(errno));
	mkdir("/run/lock", 0755);                 /* /var/lock points here */
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
