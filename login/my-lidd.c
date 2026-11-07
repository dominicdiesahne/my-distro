/*
 * my-lidd: closing a laptop's lid locks the screen and puts it to sleep.
 *
 * The kernel reports the lid as a switch (SW_LID) on an input device
 * (/dev/input/eventN, "Lid Switch", from ACPI). We find that device, wait for
 * its events, and when the lid closes:
 *   1. lock the desktop of whoever is logged in (my-lock --background, run as
 *      them by /usr/lib/my-distro/in-session; it returns once it is locked),
 *   2. suspend (/sys/power/state, like my-power suspend).
 * Opening the lid wakes the computer up; the lock screen is waiting.
 *
 * What a closed lid does is set in /etc/my-distro/power.conf:
 *     lid = suspend    (lock + sleep; the default)
 *     lid = lock       (only lock)
 *     lid = nothing
 *
 * Started at boot by /etc/init.d/S46power, as root (only root may suspend).
 * Computers without a lid have no such device: then it looks again every
 * 30 seconds (a test can add one later) and does nothing else.
 */
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CONF        "/etc/my-distro/power.conf"
#define MAX_LIDS    4
#define BITS(n)     (((n) + 8 * sizeof(long) - 1) / (8 * sizeof(long)))
#define TEST_BIT(b, a) (((a)[(b) / (8 * sizeof(long))] >> ((b) % (8 * sizeof(long)))) & 1)

static void log_msg(const char *what)
{
	char t[32];
	time_t now = time(NULL);

	strftime(t, sizeof t, "%F %T", localtime(&now));
	printf("%s %s\n", t, what);
	fflush(stdout);
}

/* lid = ... from the settings file */
static void lid_action(char *out, size_t len)
{
	char line[128], v[32];
	FILE *f = fopen(CONF, "r");

	snprintf(out, len, "suspend");
	while (f && fgets(line, sizeof line, f))
		if (sscanf(line, " lid = %31s", v) == 1)
			snprintf(out, len, "%s", v);
	if (f)
		fclose(f);
}

/* Opens every input device that has a lid switch. Returns how many. */
static int find_lids(struct pollfd *fds)
{
	DIR *d = opendir("/dev/input");
	struct dirent *e;
	int n = 0;

	while (d && n < MAX_LIDS && (e = readdir(d))) {
		char path[300];
		unsigned long sw[BITS(SW_MAX + 1)] = { 0 };
		int fd;

		if (strncmp(e->d_name, "event", 5) != 0)
			continue;
		snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
		if ((fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC)) < 0)
			continue;
		if (ioctl(fd, EVIOCGBIT(EV_SW, sizeof sw), sw) >= 0 && TEST_BIT(SW_LID, sw)) {
			char name[128] = "";

			ioctl(fd, EVIOCGNAME(sizeof name), name);
			snprintf(path, sizeof path, "lid switch: /dev/input/%s (%s)", e->d_name, name);
			log_msg(path);
			fds[n].fd = fd;
			fds[n].events = POLLIN;
			n++;
		} else {
			close(fd);
		}
	}
	if (d)
		closedir(d);
	return n;
}

static int run(const char *const argv[], int timeout)
{
	pid_t pid = fork();
	int st = 0;

	if (pid == 0) {
		alarm(timeout);                  /* never hang here for long */
		execv(argv[0], (char *const *)argv);
		_exit(127);
	}
	if (pid < 0 || waitpid(pid, &st, 0) < 0)
		return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void suspend(void)
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
		log_msg("this computer can't sleep");
		return;
	}
	sync();
	if ((f = fopen("/sys/power/state", "w"))) {
		fputs(how, f);
		fclose(f);                       /* returns after waking up */
	}
}

static void lid_closed(void)
{
	static const char *const lock_cmd[] = { "/usr/lib/my-distro/in-session",
		"/usr/bin/my-lock", "--background", NULL };
	char action[32];

	lid_action(action, sizeof action);
	if (strcmp(action, "nothing") == 0) {
		log_msg("lid closed: nothing to do (power.conf)");
		return;
	}
	/* in-session fails when nobody is logged in: then there is nothing to lock */
	log_msg(run(lock_cmd, 10) == 0 ? "lid closed: desktop locked"
	                                : "lid closed: no desktop to lock");
	if (strcmp(action, "lock") == 0)
		return;
	log_msg("suspending");
	suspend();
	log_msg("woke up");
}

int main(void)
{
	struct pollfd fds[MAX_LIDS];
	int n = 0;
	time_t ignore_until = 0;

	for (;;) {
		if (n == 0) {
			n = find_lids(fds);
			if (n == 0) {
				sleep(30);
				continue;
			}
		}
		if (poll(fds, n, -1) <= 0)
			continue;
		for (int i = 0; i < n; i++) {
			struct input_event ev;

			if (fds[i].revents & (POLLERR | POLLHUP)) {   /* device gone */
				for (int j = 0; j < n; j++)
					close(fds[j].fd);
				n = 0;
				break;
			}
			while (read(fds[i].fd, &ev, sizeof ev) == sizeof ev)
				if (ev.type == EV_SW && ev.code == SW_LID && ev.value == 1 &&
				    time(NULL) >= ignore_until) {
					lid_closed();
					/* the lid's own events around waking up */
					ignore_until = time(NULL) + 2;
				}
		}
	}
}
