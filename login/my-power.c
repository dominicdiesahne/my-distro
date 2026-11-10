/*
 * my-power: lets a normal user reboot, power off or suspend the machine, and
 * safely remove USB sticks.
 *
 *     my-power reboot
 *     my-power poweroff
 *     my-power suspend          sleep until a key, the mouse or the lid wakes it
 *     my-power eject /media/X   write everything to the drive, then unmount it
 *     my-power brightness up|down   the screen's backlight, in steps of 10 %
 *     my-power hibernate [--check]  save the memory to the swap file, switch off
 *     my-power mount-share //SERVER/SHARE [USER]   a Windows share in /media
 *                               (password on standard input; my-share)
 *     my-power switch-user      a second login screen on another console,
 *                               for another user (Start menu, Switch user)
 *
 * Only root may send signals to init (PID 1), suspend the machine or unmount
 * drives, so this program is installed "setuid root" (mode 4755): it always
 * runs with root rights, no matter who starts it. That's why it does exactly
 * these things and accepts nothing else. Our init (init/init.c) reboots on
 * SIGTERM and powers off on SIGUSR2.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int is_mounted(const char *dir);

/* Puts the machine to sleep: "mem" (suspend to RAM) if it can, else
 * "freeze" (the kernel's own lighter sleep). Returns after waking up. */
static int suspend(void)
{
	char states[128] = "";
	const char *how;
	FILE *f = fopen("/sys/power/state", "r");

	if (f) {
		if (!fgets(states, sizeof states, f))
			states[0] = '\0';
		fclose(f);
	}
	how = strstr(states, "mem") ? "mem" : strstr(states, "freeze") ? "freeze" : NULL;
	if (!how) {
		fprintf(stderr, "my-power: this computer can't sleep\n");
		return 1;
	}
	sync();                          /* in case it never wakes up again */
	f = fopen("/sys/power/state", "w");
	if (!f || fputs(how, f) < 0 || fclose(f) != 0) {
		perror("my-power: suspend");
		return 1;
	}
	return 0;
}

/* First line of a file in buf ("" if it can't be read) */
static void read_line(const char *path, char *buf, size_t len)
{
	FILE *f = fopen(path, "r");

	buf[0] = '\0';
	if (f) {
		if (!fgets(buf, len, f))
			buf[0] = '\0';
		fclose(f);
	}
	buf[strcspn(buf, "\n")] = '\0';
}

/*
 * Hibernate: the kernel writes the memory into the swap file (/swapfile,
 * made by my-install) and switches off; the next start continues from there.
 * It needs the kernel to know where the swap file is (/sys/power/resume,
 * set from the kernel line or by the initramfs) and the file to be on.
 * Returns after waking up again. check_only: only say whether it would work.
 */
static int hibernate(int check_only)
{
	char states[128], dev[32], swaps[4096] = "";
	FILE *f = fopen("/proc/swaps", "r");

	if (f) {
		size_t n = fread(swaps, 1, sizeof swaps - 1, f);

		swaps[n] = '\0';
		fclose(f);
	}
	read_line("/sys/power/state", states, sizeof states);
	read_line("/sys/power/resume", dev, sizeof dev);
	/* 0.9.7: started with Secure Boot, the kernel locks itself down, and
	 * that switches off hibernation (a memory image on the disk could be
	 * changed while the computer is off, and Linux can't check it) */
	{
		char lock[128];

		read_line("/sys/kernel/security/lockdown", lock, sizeof lock);
		if (!strstr(states, "disk") && (strstr(lock, "[integrity]") ||
		                                 strstr(lock, "[confidentiality]"))) {
			fprintf(stderr, "Hibernate is not possible while Secure Boot is on "
			        "(the kernel's lockdown forbids it). Suspend still works.\n");
			return 1;
		}
	}
	if (!strstr(states, "disk")) {
		fprintf(stderr, "This computer can't hibernate (the kernel doesn't offer it).\n");
		return 1;
	}
	if (!strstr(swaps, "/swapfile") || !dev[0] || strcmp(dev, "0:0") == 0) {
		fprintf(stderr, "Hibernating needs the swap file of an installed system "
		        "(my-install makes it).\n");
		return 1;
	}
	if (check_only) {
		printf("Hibernate is possible.\n");
		return 0;
	}
	sync();
	f = fopen("/sys/power/state", "w");
	if (!f || fputs("disk", f) < 0 || fclose(f) != 0) {
		perror("my-power: hibernate (not enough room in the swap file?)");
		return 1;
	}
	return 0;
}

/*
 * A Windows (SMB) share, mounted for the user who runs this: the files
 * belong to them, nobody else can open the folder. The password comes on
 * standard input (one line), never on the command line, and goes to
 * mount.cifs in its PASSWD variable. The share appears as
 * /media/SERVER-SHARE; "my-power eject" unmounts it again.
 *
 *     my-power mount-share //SERVER/SHARE [USER]
 */
static int safe_name(const char *s, const char *extra)
{
	if (!*s || strlen(s) > 64 || *s == '-' || *s == '.')
		return 0;
	for (; *s; s++)
		if (!(*s >= 'a' && *s <= 'z') && !(*s >= 'A' && *s <= 'Z') &&
		    !(*s >= '0' && *s <= '9') && !strchr(extra, *s))
			return 0;
	return 1;
}

static int mount_share(const char *unc, const char *user)
{
	char server[80], share[80], dir[PATH_MAX], opts[256], pass[256] = "";
	const char *slash;
	uid_t uid = getuid();
	gid_t gid = getgid();
	pid_t pid;
	int st;

	/* //server/share: names with letters, digits and . _ - $ only */
	if (strncmp(unc, "//", 2) != 0 || !(slash = strchr(unc + 2, '/')) ||
	    slash - unc - 2 >= (int)sizeof server) {
		fprintf(stderr, "my-power: the share is written //SERVER/SHARE\n");
		return 2;
	}
	snprintf(server, sizeof server, "%.*s", (int)(slash - unc - 2), unc + 2);
	snprintf(share, sizeof share, "%s", slash + 1);
	if (!safe_name(server, ".-_") || !safe_name(share, ".-_$ ") ||
	    (user && !safe_name(user, ".-_@\\"))) {
		fprintf(stderr, "my-power: unusual characters in the share's or user's name\n");
		return 2;
	}
	if (!fgets(pass, sizeof pass, stdin))
		pass[0] = '\0';
	pass[strcspn(pass, "\n")] = '\0';

	snprintf(dir, sizeof dir, "/media/%s-%s", server, share);
	for (char *c = dir + 7; *c; c++)
		if (*c == ' ' || *c == '$')
			*c = '_';
	if (is_mounted(dir)) {
		printf("%s\n", dir);                 /* already there: just open it */
		return 0;
	}
	mkdir("/media", 0755);
	if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
		perror("my-power: mount-share");
		return 1;
	}
	if (chown(dir, uid, gid) != 0)
		perror("my-power: mount-share");
	/* the files belong to the user; "guest" when there is no user name */
	snprintf(opts, sizeof opts, "uid=%u,gid=%u,file_mode=0600,dir_mode=0700,"
	         "nosuid,nodev,noexec,%s%s", (unsigned)uid, (unsigned)gid,
	         user && *user ? "username=" : "guest", user && *user ? user : "");
	pid = fork();
	if (pid == 0) {
		char *envp[] = { NULL, "PATH=/usr/sbin:/usr/bin:/sbin:/bin", NULL };
		char pw_env[300];

		snprintf(pw_env, sizeof pw_env, "PASSWD=%s", pass);
		envp[0] = pw_env;
		if (setuid(0) != 0)
			_exit(1);
		execle("/usr/sbin/mount.cifs", "mount.cifs", unc, dir, "-o", opts,
		       (char *)NULL, envp);
		_exit(127);
	}
	memset(pass, 0, sizeof pass);
	if (pid < 0 || waitpid(pid, &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
		rmdir(dir);
		fprintf(stderr, "The share could not be opened (wrong name or password, "
		        "or the server can't be reached).\n");
		return 1;
	}
	printf("%s\n", dir);
	return 0;
}

/*
 * Fast user switching: the first desktop runs on display :0, console 7.
 * "Switch user" shows a second login screen on display :1 (console 8), up
 * to :3 (console 10); the first session stays as it is, locked. my-login
 * writes /run/my-login/N while display :N shows its login screen
 * ("waiting") or a session (the user's name). A display that waits is
 * reused; otherwise /usr/lib/my-distro/second-login starts a new X server
 * with my-login on the next free console. Ctrl+Alt+F7 goes back to the
 * first desktop, F8 to the second, ...
 */
static int show_console(int vt)
{
	char cmd[16];
	pid_t pid = fork();
	int st;

	snprintf(cmd, sizeof cmd, "%d", vt);
	if (pid == 0) {
		char *envp[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", NULL };

		if (setuid(0) != 0)
			_exit(1);
		execle("/usr/bin/chvt", "chvt", cmd, (char *)NULL, envp);
		_exit(127);
	}
	return pid > 0 && waitpid(pid, &st, 0) > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : 1;
}

static int switch_user(void)
{
	char path[64], state[64], lock[64];

	for (int n = 1; n <= 3; n++) {
		snprintf(path, sizeof path, "/run/my-login/%d", n);
		read_line(path, state, sizeof state);
		if (strcmp(state, "waiting") == 0)
			return show_console(7 + n);          /* a login screen is there */
	}
	for (int n = 1; n <= 3; n++) {
		snprintf(lock, sizeof lock, "/tmp/.X%d-lock", n);
		snprintf(path, sizeof path, "/run/my-login/%d", n);
		if (access(lock, F_OK) == 0 || access(path, F_OK) == 0)
			continue;                             /* in use */
		pid_t pid = fork();

		if (pid == 0) {
			char d[8], vt[8];
			char *envp[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LANG=C.UTF-8", NULL };
			int fd = open("/dev/null", O_RDWR);

			snprintf(d, sizeof d, ":%d", n);
			snprintf(vt, sizeof vt, "vt%d", 7 + n);
			if (setgid(0) != 0 || setuid(0) != 0)
				_exit(1);
			setsid();                             /* not part of the user's session */
			if (fd >= 0) {
				dup2(fd, 0);
				dup2(fd, 1);
				dup2(fd, 2);
			}
			execle("/usr/lib/my-distro/second-login", "second-login", d, vt,
			       (char *)NULL, envp);
			_exit(127);
		}
		return pid > 0 ? 0 : 1;
	}
	fprintf(stderr, "my-power: there are already 4 desktops\n");
	return 1;
}

/* Is dir a mount point in /proc/mounts? (spaces there are written \040) */
static int is_mounted(const char *dir)
{
	char line[PATH_MAX * 2 + 64], dev[PATH_MAX], mnt[PATH_MAX];
	FILE *f = fopen("/proc/mounts", "r");
	int found = 0;

	while (f && !found && fgets(line, sizeof line, f))
		if (sscanf(line, "%4095s %4095s", dev, mnt) == 2 && strcmp(mnt, dir) == 0)
			found = 1;
	if (f)
		fclose(f);
	return found;
}

/* Unmounts a drive in /media: only a folder directly in /media, nothing else
 * (not the system disk). sync first, so nothing is lost when it's pulled out. */
static int eject(const char *path)
{
	char dir[PATH_MAX];

	if (!realpath(path, dir) || strncmp(dir, "/media/", 7) != 0 ||
	    dir[7] == '\0' || strchr(dir + 7, '/')) {
		fprintf(stderr, "my-power: only drives in /media can be ejected\n");
		return 1;
	}
	if (!is_mounted(dir)) {
		fprintf(stderr, "my-power: %s is not a drive\n", dir);
		return 1;
	}
	sync();
	if (umount(dir) != 0) {
		if (errno == EBUSY)
			fprintf(stderr, "A program still uses a file on it: close it and try again.\n");
		else
			perror("my-power: eject");
		return 1;
	}
	rmdir(dir);
	return 0;
}

static long read_number(const char *dir, const char *file)
{
	char path[PATH_MAX];
	long v = -1;
	FILE *f;

	snprintf(path, sizeof path, "%s/%s", dir, file);
	if ((f = fopen(path, "r"))) {
		if (fscanf(f, "%ld", &v) != 1)
			v = -1;
		fclose(f);
	}
	return v;
}

/*
 * The screen's backlight: /sys/class/backlight has one folder per way to set
 * it. A laptop can have two (e.g. acpi_video0 from the firmware and
 * intel_backlight from the graphics driver); the kernel's own advice is to
 * prefer "firmware", then "platform", then "raw". Prints the new value in %.
 */
static int brightness(const char *how)
{
	static const char *types[] = { "firmware", "platform", "raw" };
	char best[PATH_MAX] = "", path[PATH_MAX], type[32];
	int best_rank = 99;
	long max, now, step;
	DIR *d = opendir("/sys/class/backlight");
	struct dirent *e;
	FILE *f;

	if (strcmp(how, "up") != 0 && strcmp(how, "down") != 0) {
		fprintf(stderr, "usage: my-power brightness up|down\n");
		return 2;
	}
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof path, "/sys/class/backlight/%s/type", e->d_name);
		type[0] = '\0';
		if ((f = fopen(path, "r"))) {
			if (!fgets(type, sizeof type, f))
				type[0] = '\0';
			fclose(f);
		}
		type[strcspn(type, "\n")] = '\0';
		for (int r = 0; r < 3; r++)
			if (strcmp(type, types[r]) == 0 && r < best_rank) {
				best_rank = r;
				snprintf(best, sizeof best, "/sys/class/backlight/%s", e->d_name);
			}
	}
	if (d)
		closedir(d);
	if (!best[0]) {
		fprintf(stderr, "my-power: this screen's brightness can't be changed\n");
		return 1;
	}
	max = read_number(best, "max_brightness");
	now = read_number(best, "brightness");
	if (max <= 0 || now < 0)
		return 1;
	step = max / 10 > 0 ? max / 10 : 1;
	now += strcmp(how, "up") == 0 ? step : -step;
	now = now > max ? max : now < step / 2 + 1 ? step / 2 + 1 : now;   /* never black */
	snprintf(path, sizeof path, "%s/brightness", best);
	if (!(f = fopen(path, "w")) || fprintf(f, "%ld\n", now) < 0 || fclose(f) != 0) {
		perror("my-power: brightness");
		return 1;
	}
	printf("%ld\n", now * 100 / max);
	return 0;
}

int main(int argc, char **argv)
{
	int sig;

	if (argc == 2 && strcmp(argv[1], "reboot") == 0)
		sig = SIGTERM;
	else if (argc == 2 && strcmp(argv[1], "poweroff") == 0)
		sig = SIGUSR2;
	else if (argc == 2 && strcmp(argv[1], "suspend") == 0)
		return suspend();
	else if (argc == 3 && strcmp(argv[1], "eject") == 0)
		return eject(argv[2]);
	else if (argc == 3 && strcmp(argv[1], "brightness") == 0)
		return brightness(argv[2]);
	else if (argc >= 2 && argc <= 3 && strcmp(argv[1], "hibernate") == 0)
		return hibernate(argc == 3 && strcmp(argv[2], "--check") == 0);
	else if ((argc == 3 || argc == 4) && strcmp(argv[1], "mount-share") == 0)
		return mount_share(argv[2], argc == 4 ? argv[3] : NULL);
	else if (argc == 2 && strcmp(argv[1], "switch-user") == 0)
		return switch_user();
	else {
		fprintf(stderr, "usage: my-power reboot|poweroff|suspend|hibernate [--check]|"
		        "eject /media/NAME|brightness up|down|mount-share //SERVER/SHARE [USER]|"
		        "switch-user\n");
		return 2;
	}
	if (kill(1, sig) != 0) {
		perror("my-power");
		return 1;
	}
	return 0;
}
