/*
 * my-power: lets a normal user reboot, power off or suspend the machine, and
 * safely remove USB sticks.
 *
 *     my-power reboot
 *     my-power poweroff
 *     my-power suspend          sleep until a key, the mouse or the lid wakes it
 *     my-power eject /media/X   write everything to the drive, then unmount it
 *     my-power brightness up|down   the screen's backlight, in steps of 10 %
 *
 * Only root may send signals to init (PID 1), suspend the machine or unmount
 * drives, so this program is installed "setuid root" (mode 4755): it always
 * runs with root rights, no matter who starts it. That's why it does exactly
 * these things and accepts nothing else. Our init (init/init.c) reboots on
 * SIGTERM and powers off on SIGUSR2.
 */
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <unistd.h>

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
	else {
		fprintf(stderr, "usage: my-power reboot|poweroff|suspend|eject /media/NAME|"
		        "brightness up|down\n");
		return 2;
	}
	if (kill(1, sig) != 0) {
		perror("my-power");
		return 1;
	}
	return 0;
}
