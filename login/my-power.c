/*
 * my-power: lets a normal user reboot, power off or suspend the machine, and
 * safely remove USB sticks.
 *
 *     my-power reboot
 *     my-power poweroff
 *     my-power suspend          sleep until a key, the mouse or the lid wakes it
 *     my-power eject /media/X   write everything to the drive, then unmount it
 *
 * Only root may send signals to init (PID 1), suspend the machine or unmount
 * drives, so this program is installed "setuid root" (mode 4755): it always
 * runs with root rights, no matter who starts it. That's why it does exactly
 * these things and accepts nothing else. Our init (init/init.c) reboots on
 * SIGTERM and powers off on SIGUSR2.
 */
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
	else {
		fprintf(stderr, "usage: my-power reboot|poweroff|suspend|eject /media/NAME\n");
		return 2;
	}
	if (kill(1, sig) != 0) {
		perror("my-power");
		return 1;
	}
	return 0;
}
