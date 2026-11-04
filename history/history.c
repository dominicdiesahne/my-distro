/* Reading and restoring versions; see history.h for the layout. */
#include "history.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int hist_dir(const char *path, char *out, size_t n)
{
	size_t len = snprintf(out, n, "%s/", HIST_STORE);

	for (const char *p = path; *p; p++) {
		const char *rep = *p == '/' ? "%2F" : *p == '%' ? "%25" : NULL;

		if (len + 4 >= n)
			return -1;
		if (rep) {
			memcpy(out + len, rep, 3);
			len += 3;
		} else {
			out[len++] = *p;
		}
	}
	out[len] = '\0';
	/* one directory name may be at most 255 bytes */
	return len - strlen(HIST_STORE) - 1 > 255 ? -1 : 0;
}

int hist_version_file(const char *path, int v, char *out, size_t n)
{
	char dir[PATH_MAX];

	if (hist_dir(path, dir, sizeof dir) != 0)
		return -1;
	return snprintf(out, n, "%s/%d", dir, v) < (int)n ? 0 : -1;
}

static int cmp_int(const void *a, const void *b)
{
	return *(const int *)a - *(const int *)b;
}

int hist_versions(const char *path, int *out, int max)
{
	char dir[PATH_MAX];
	DIR *d;
	struct dirent *e;
	int n = 0;

	if (hist_dir(path, dir, sizeof dir) != 0 || !(d = opendir(dir)))
		return 0;
	while ((e = readdir(d)) && n < max) {
		char *end;
		long v = strtol(e->d_name, &end, 10);

		if (e->d_name[0] >= '0' && e->d_name[0] <= '9' && *end == '\0')
			out[n++] = (int)v;
	}
	closedir(d);
	qsort(out, n, sizeof *out, cmp_int);
	return n;
}

static int read_log(const char *file, struct change **list, int *n, int *cap)
{
	FILE *f = fopen(file, "r");
	char line[PATH_MAX + 64];

	if (!f)
		return -1;
	while (fgets(line, sizeof line, f)) {
		struct change c;
		char *tab1 = strchr(line, '\t'), *tab2;

		if (!tab1 || !(tab2 = strchr(tab1 + 1, '\t')))
			continue;
		line[strcspn(line, "\n")] = '\0';
		c.when = strtol(line, NULL, 10);
		c.version = atoi(tab1 + 1);
		snprintf(c.path, sizeof c.path, "%s", tab2 + 1);
		if (*n == *cap) {
			struct change *bigger;

			*cap = *cap ? *cap * 2 : 256;
			bigger = realloc(*list, *cap * sizeof **list);
			if (!bigger)
				break;
			*list = bigger;
		}
		(*list)[(*n)++] = c;
	}
	fclose(f);
	return 0;
}

static int newest_first(const void *a, const void *b)
{
	const struct change *x = a, *y = b;

	if (x->when != y->when)
		return x->when < y->when ? 1 : -1;
	/* same second: the higher version of a file is newer */
	return y->version - x->version;
}

int hist_changes(uid_t uid, struct change **out)
{
	struct change *list = NULL;
	int n = 0, cap = 0;
	char file[PATH_MAX];

	if (uid == 0) {
		DIR *d = opendir(HIST_LOG);
		struct dirent *e;

		while (d && (e = readdir(d))) {
			if (e->d_name[0] == '.')
				continue;
			snprintf(file, sizeof file, "%s/%s", HIST_LOG, e->d_name);
			read_log(file, &list, &n, &cap);
		}
		if (d)
			closedir(d);
	} else {
		snprintf(file, sizeof file, "%s/%u", HIST_LOG, (unsigned)uid);
		read_log(file, &list, &n, &cap);
	}
	if (n > 1)
		qsort(list, n, sizeof *list, newest_first);
	*out = list;
	return n;
}

int hist_restore(const char *path, int v, char *err, size_t n)
{
	char vfile[PATH_MAX], buf[65536];
	int in, out;
	ssize_t r;
	struct stat st;
	mode_t mode = 0644;

	if (hist_version_file(path, v, vfile, sizeof vfile) != 0) {
		snprintf(err, n, "path too long");
		return -1;
	}
	in = open(vfile, O_RDONLY | O_CLOEXEC);
	if (in < 0) {
		snprintf(err, n, "version %d of %s: %s", v, path, strerror(errno));
		return -1;
	}
	if (stat(path, &st) == 0) {
		if (!S_ISREG(st.st_mode)) {
			snprintf(err, n, "%s is not a normal file", path);
			close(in);
			return -1;
		}
		mode = st.st_mode & 07777;
	}
	out = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
	if (out < 0) {
		snprintf(err, n, "%s: %s", path, strerror(errno));
		close(in);
		return -1;
	}
	while ((r = read(in, buf, sizeof buf)) > 0)
		if (write(out, buf, r) != r) {
			snprintf(err, n, "writing %s: %s", path, strerror(errno));
			r = -1;
			break;
		}
	close(in);
	if (close(out) != 0 || r < 0) {
		if (r == 0)
			snprintf(err, n, "writing %s: %s", path, strerror(errno));
		return -1;
	}
	return 0;
}

void hist_abspath(const char *in, char *out, size_t n)
{
	char cwd[PATH_MAX];

	if (in[0] == '/' || !getcwd(cwd, sizeof cwd))
		snprintf(out, n, "%s", in);
	else
		snprintf(out, n, "%s/%s", strcmp(cwd, "/") ? cwd : "", in);
}
