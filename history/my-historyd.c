/*
 * my-historyd: saves a version of every file that is changed, system-wide.
 *
 * It asks the kernel's fanotify for FAN_CLOSE_WRITE on the whole root
 * filesystem: whenever any program closes a file it had open for writing,
 * we get an event with an open file descriptor to that file, copy its
 * content into the version store and note the change in the owner's log.
 *
 * At start it also saves "version 0" (the state at boot) of every file in
 * /etc, /root and /home, so even the first change there can be undone.
 * Paths listed in /etc/my-history/exclude are skipped (logs, caches, ...).
 *
 * Runs as root (fanotify needs it), started by /etc/init.d/S50history.
 */
#define _GNU_SOURCE
#include "history.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fanotify.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define EXCLUDE_FILE "/etc/my-history/exclude"
#define MAX_SIZE     (16 * 1024 * 1024)  /* bigger files are not versioned */
#define MAX_VERSIONS 50                  /* older versions are deleted */
#define MAX_EXCLUDES 64

static char excludes[MAX_EXCLUDES][256];
static int nexcludes;
static pid_t self;

/* chown can only fail here if the store is broken; ignore the result */
static void ignore(int r)
{
	(void)r;
}

static void load_excludes(void)
{
	FILE *f = fopen(EXCLUDE_FILE, "r");
	char line[256];

	if (!f)
		return;
	while (fgets(line, sizeof line, f) && nexcludes < MAX_EXCLUDES) {
		line[strcspn(line, "\r\n")] = '\0';
		if (line[0] && line[0] != '#')
			snprintf(excludes[nexcludes++], 256, "%s", line);
	}
	fclose(f);
}

/* Lines starting with '/' match a path prefix, other lines any part of it. */
static int excluded(const char *path)
{
	if (strncmp(path, HIST_DIR, strlen(HIST_DIR)) == 0)
		return 1;                         /* never version our own store */
	for (int i = 0; i < nexcludes; i++) {
		const char *x = excludes[i];

		if (x[0] == '/' ? strncmp(path, x, strlen(x)) == 0
		                : strstr(path, x) != NULL)
			return 1;
	}
	return 0;
}

/* Same content as file "a"? Used to skip saves that change nothing. */
static int same_content(const char *a, int fd, off_t size)
{
	char x[16384], y[16384];
	struct stat st;
	int fa = open(a, O_RDONLY | O_CLOEXEC);
	ssize_t r;
	off_t pos = 0;
	int same = 0;

	if (fa < 0)
		return 0;
	if (fstat(fa, &st) == 0 && st.st_size == size) {
		same = 1;
		while (same && (r = read(fa, x, sizeof x)) > 0) {
			if (pread(fd, y, r, pos) != r || memcmp(x, y, r) != 0)
				same = 0;
			pos += r;
		}
	}
	close(fa);
	return same;
}

static void write_log(const char *path, int version, uid_t uid)
{
	char file[64], line[PATH_MAX + 64];
	int fd, len;

	snprintf(file, sizeof file, "%s/%u", HIST_LOG, (unsigned)uid);
	fd = open(file, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
	if (fd < 0)
		return;
	ignore(fchown(fd, uid, (gid_t)-1));          /* the owner may read their log */
	len = snprintf(line, sizeof line, "%ld\t%d\t%s\n", (long)time(NULL),
	               version, path);
	if (write(fd, line, len) != len)
		perror("my-historyd: log");
	close(fd);
}

/* Copies the open file fd into the store as the next version of path.
 * baseline = 1: only if the file has no versions yet, and don't log it. */
static void save_version(const char *path, int fd, const struct stat *st,
                         int baseline)
{
	char dir[PATH_MAX], vfile[PATH_MAX + 16], tmp[PATH_MAX + 24];
	char buf[65536];
	int versions[MAX_VERSIONS * 2 + 8];
	int n, next, out;
	ssize_t r;
	off_t pos = 0;

	if (!S_ISREG(st->st_mode) || st->st_size > MAX_SIZE || excluded(path) ||
	    hist_dir(path, dir, sizeof dir) != 0)
		return;

	n = hist_versions(path, versions, sizeof versions / sizeof *versions);
	if (baseline && n > 0)
		return;
	if (n > 0) {
		hist_version_file(path, versions[n - 1], vfile, sizeof vfile);
		if (same_content(vfile, fd, st->st_size))
			return;                       /* nothing really changed */
	}
	next = n > 0 ? versions[n - 1] + 1 : 0;

	if (mkdir(dir, 0700) == 0)
		ignore(chown(dir, st->st_uid, st->st_gid));
	snprintf(vfile, sizeof vfile, "%s/%d", dir, next);
	snprintf(tmp, sizeof tmp, "%s/.%d.tmp", dir, next);

	out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (out < 0) {
		fprintf(stderr, "my-historyd: %s: %s\n", tmp, strerror(errno));
		return;
	}
	while ((r = pread(fd, buf, sizeof buf, pos)) > 0) {
		if (write(out, buf, r) != r)
			break;
		pos += r;
	}
	ignore(fchown(out, st->st_uid, st->st_gid));
	close(out);
	rename(tmp, vfile);                   /* appears complete or not at all */

	/* keep the newest MAX_VERSIONS */
	for (int i = 0; n + 1 - i > MAX_VERSIONS; i++) {
		hist_version_file(path, versions[i], vfile, sizeof vfile);
		unlink(vfile);
	}
	if (!baseline)
		write_log(path, next, st->st_uid);
}

/* nftw callback for the boot-time snapshot */
static int snapshot(const char *path, const struct stat *st, int type,
                    struct FTW *ftw)
{
	(void)ftw;
	if (type == FTW_F && S_ISREG(st->st_mode) && !excluded(path)) {
		int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

		if (fd >= 0) {
			save_version(path, fd, st, 1);
			close(fd);
		}
	}
	return 0;
}

static void handle(struct fanotify_event_metadata *ev)
{
	char link[64], path[PATH_MAX];
	ssize_t len;
	struct stat st;

	if (ev->fd < 0)
		return;
	if (ev->pid != self && (ev->mask & FAN_CLOSE_WRITE)) {
		/* the kernel gave us an fd; /proc tells us which path it is */
		snprintf(link, sizeof link, "/proc/self/fd/%d", ev->fd);
		len = readlink(link, path, sizeof path - 1);
		if (len > 0) {
			path[len] = '\0';
			if (!strstr(path, " (deleted)") && fstat(ev->fd, &st) == 0)
				save_version(path, ev->fd, &st, 0);
		}
	}
	close(ev->fd);
}

int main(void)
{
	char buf[8192] __attribute__((aligned(__alignof__(struct fanotify_event_metadata))));
	int fan;

	self = getpid();
	load_excludes();
	mkdir(HIST_DIR, 0711);
	mkdir(HIST_STORE, 0711);              /* users can enter, not list */
	mkdir(HIST_LOG, 0711);

	fan = fanotify_init(FAN_CLASS_NOTIF | FAN_CLOEXEC, O_RDONLY | O_LARGEFILE);
	if (fan < 0) {
		perror("my-historyd: fanotify_init (kernel needs CONFIG_FANOTIFY)");
		return 1;
	}
	/* The whole filesystem "/" is on. /proc, /sys, /dev, /tmp are separate
	 * filesystems and therefore not included. */
	if (fanotify_mark(fan, FAN_MARK_ADD | FAN_MARK_FILESYSTEM, FAN_CLOSE_WRITE,
	                  AT_FDCWD, "/") != 0 &&
	    fanotify_mark(fan, FAN_MARK_ADD | FAN_MARK_MOUNT, FAN_CLOSE_WRITE,
	                  AT_FDCWD, "/") != 0) {
		perror("my-historyd: fanotify_mark");
		return 1;
	}

	/* Version 0 of the files people usually edit. Events that happen
	 * meanwhile wait in the kernel queue. */
	nftw("/etc", snapshot, 32, FTW_PHYS | FTW_MOUNT);
	nftw("/root", snapshot, 32, FTW_PHYS | FTW_MOUNT);
	nftw("/home", snapshot, 32, FTW_PHYS | FTW_MOUNT);
	fprintf(stderr, "my-historyd: watching all file changes\n");

	for (;;) {
		ssize_t len = read(fan, buf, sizeof buf);
		struct fanotify_event_metadata *ev = (void *)buf;

		if (len < 0) {
			if (errno == EINTR)
				continue;
			perror("my-historyd: read");
			return 1;
		}
		for (; FAN_EVENT_OK(ev, len); ev = FAN_EVENT_NEXT(ev, len))
			handle(ev);
	}
}
