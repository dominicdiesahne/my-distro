/*
 * my-term (1.4.0): lets the screen reader read terminals. xterm knows
 * nothing about the accessibility bus, so my-reader can't ask it for its
 * text; instead the xterm wrapper (/usr/bin/xterm) runs the program in it
 * through my-term while the screen reader is on:
 *
 *     my-term [PROGRAM [ARGS]]       (default: $SHELL, else /bin/sh)
 *
 * my-term runs PROGRAM on a pseudo-terminal of its own and copies
 * everything between it and the xterm unchanged (keys in, output out,
 * window size along). What the program writes is also sent to my-reader:
 * without the escape sequences (colours, cursor movement), in pieces of up
 * to about a second of output, through $XDG_RUNTIME_DIR/my-reader.say as
 * "WINDOWID<tab>TEXT". my-reader says it only while this xterm (WINDOWID,
 * set by xterm) is the active window. Typed letters come back as the
 * terminal's echo and are said too; a password prompt doesn't echo, so a
 * password is never said. A full-screen program (nano, htop, less: on the
 * terminal's alternate screen) isn't read, its screen changes all the time.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define SAY_MAX 600

static int master = -1;
static volatile sig_atomic_t winch;
static char say[SAY_MAX + 1];
static size_t say_len;
static enum { TEXT, ESC, CSI, OSC, OSC_ESC } state;
static char csi[16];              /* the parameters of the escape sequence */
static size_t csi_len;
static int alt;                   /* a full-screen program (alternate screen) */
static char fifo[256], window[32];

static void on_winch(int s)
{
	(void)s;
	winch = 1;
}

static void put(char c)
{
	if (say_len == SAY_MAX) {                 /* keep the newest */
		memmove(say, say + SAY_MAX / 2, SAY_MAX / 2);
		say_len = SAY_MAX / 2;
	}
	say[say_len++] = c;
}

/* the printable text of the output, without escape sequences */
static void collect(const char *buf, ssize_t n)
{
	for (ssize_t i = 0; i < n; i++) {
		unsigned char c = buf[i];

		switch (state) {
		case TEXT:
			if (c == 0x1b)
				state = ESC;
			else if (alt)
				;                        /* nano, htop, less: their screen isn't read */
			else if (c == '\n' || c == '\t')
				put(' ');
			else if (c == '\b' && say_len > 0)
				say_len--;               /* the shell's backspace */
			else if (c >= 0x20 && c != 0x7f)
				put(c);
			break;
		case ESC:
			state = c == '[' ? CSI : c == ']' ? OSC : TEXT;
			csi_len = 0;
			break;
		case CSI:
			if (c >= 0x40 && c <= 0x7e) {
				state = TEXT;
				csi[csi_len] = '\0';
				/* the alternate screen on (h) or off (l): ?1049, ?1047, ?47 */
				if ((c == 'h' || c == 'l') && (!strcmp(csi, "?1049") || !strcmp(csi, "?1047") ||
				                               !strcmp(csi, "?47"))) {
					alt = c == 'h';
					say_len = 0;
				}
			} else if (csi_len < sizeof csi - 1) {
				csi[csi_len++] = c;
			}
			break;
		case OSC:                            /* (window titles) */
			if (c == 7)
				state = TEXT;
			else if (c == 0x1b)
				state = OSC_ESC;
			break;
		case OSC_ESC:
			state = TEXT;
			break;
		}
	}
}

/* hand the collected text to my-reader, if it runs (it made the fifo) */
static void flush_say(void)
{
	size_t start = 0;
	int fd;

	while (start < say_len && say[start] == ' ')
		start++;
	if (start < say_len && fifo[0] && (fd = open(fifo, O_WRONLY | O_NONBLOCK | O_CLOEXEC)) >= 0) {
		char line[SAY_MAX + 40];
		int n = snprintf(line, sizeof line, "%s\t%.*s\n", window, (int)(say_len - start), say + start);

		if (n > 0 && write(fd, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1) < 0) {
			/* the reader is busy or gone: never mind */
		}
		close(fd);
	}
	say_len = 0;
}

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
	struct termios saved, raw;
	struct winsize ws;
	int have_tty = tcgetattr(0, &saved) == 0, status = 0;
	const char *rt = getenv("XDG_RUNTIME_DIR"), *wid = getenv("WINDOWID");
	long long first = 0;
	pid_t child;

	if (rt && *rt)
		snprintf(fifo, sizeof fifo, "%s/my-reader.say", rt);
	snprintf(window, sizeof window, "%s", wid ? wid : "0");
	if (ioctl(0, TIOCGWINSZ, &ws) != 0)
		memset(&ws, 0, sizeof ws);
	child = forkpty(&master, NULL, have_tty ? &saved : NULL, ws.ws_row ? &ws : NULL);
	if (child < 0) {
		perror("my-term: forkpty");
		return 1;
	}
	if (child == 0) {
		const char *sh = getenv("SHELL");

		if (argc > 1)
			execvp(argv[1], argv + 1);
		else
			execl(sh && *sh ? sh : "/bin/sh", sh && *sh ? sh : "/bin/sh", (char *)NULL);
		perror("my-term");
		_exit(127);
	}
	signal(SIGWINCH, on_winch);
	signal(SIGPIPE, SIG_IGN);
	if (have_tty) {
		raw = saved;
		cfmakeraw(&raw);
		tcsetattr(0, TCSANOW, &raw);
	}
	for (;;) {
		struct pollfd p[2] = { { 0, POLLIN, 0 }, { master, POLLIN, 0 } };
		char buf[4096];
		ssize_t n;
		int timeout = say_len ? 150 : -1;

		if (winch) {
			winch = 0;
			if (ioctl(0, TIOCGWINSZ, &ws) == 0)
				ioctl(master, TIOCSWINSZ, &ws);
		}
		n = poll(p, 2, timeout);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (n == 0 || (say_len && now_ms() - first > 1000)) {
			flush_say();             /* quiet for a moment, or a second of output */
			if (n == 0)
				continue;
		}
		if (p[0].revents & POLLIN) {
			n = read(0, buf, sizeof buf);
			if (n <= 0)
				break;
			for (ssize_t o = 0, w; o < n; o += w)
				if ((w = write(master, buf + o, n - o)) <= 0)
					break;
		} else if (p[0].revents & (POLLHUP | POLLERR)) {
			break;
		}
		if (p[1].revents & POLLIN) {
			n = read(master, buf, sizeof buf);
			if (n <= 0)
				break;
			for (ssize_t o = 0, w; o < n; o += w)
				if ((w = write(1, buf + o, n - o)) <= 0)
					break;
			if (!say_len)
				first = now_ms();
			collect(buf, n);
		} else if (p[1].revents & (POLLHUP | POLLERR)) {
			break;                   /* the program ended */
		}
	}
	flush_say();
	if (have_tty)
		tcsetattr(0, TCSANOW, &saved);
	close(master);
	if (waitpid(child, &status, 0) == child)
		return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
	return 0;
}
