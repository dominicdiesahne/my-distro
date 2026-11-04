/*
 * my-power: lets a normal user reboot or power off the machine.
 *
 *     my-power reboot
 *     my-power poweroff
 *
 * Only root may send signals to init (PID 1), so this program is installed
 * "setuid root" (mode 4755): it always runs with root rights, no matter who
 * starts it. That's why it does exactly one thing and accepts nothing else.
 * Our init (init/init.c) reboots on SIGTERM and powers off on SIGUSR2.
 */
#include <signal.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
	int sig;

	if (argc == 2 && strcmp(argv[1], "reboot") == 0)
		sig = SIGTERM;
	else if (argc == 2 && strcmp(argv[1], "poweroff") == 0)
		sig = SIGUSR2;
	else {
		fprintf(stderr, "usage: my-power reboot|poweroff\n");
		return 2;
	}
	if (kill(1, sig) != 0) {
		perror("my-power");
		return 1;
	}
	return 0;
}
