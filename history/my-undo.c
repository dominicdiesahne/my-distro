/*
 * my-undo: undo file changes from the command line.
 *
 *   my-undo                  undo the most recent change to one of your files
 *   my-undo list [N]         the N most recent changes (default 20)
 *   my-undo history FILE     all saved versions of FILE
 *   my-undo restore FILE N   put version N of FILE back
 *   my-undo show FILE N      print version N of FILE
 *
 * Restoring is itself a change, so it gets saved as a new version too:
 * running my-undo twice undoes the undo.
 */
#include "history.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAXV 256

static void when(time_t t, char *out, size_t n)
{
	strftime(out, n, "%a %d.%m. %H:%M:%S", localtime(&t));
}

static int usage(void)
{
	fprintf(stderr,
	        "usage: my-undo                  undo your last file change\n"
	        "       my-undo list [N]         show recent changes\n"
	        "       my-undo history FILE     show the versions of FILE\n"
	        "       my-undo restore FILE N   restore version N of FILE\n"
	        "       my-undo show FILE N      print version N of FILE\n");
	return 2;
}

static int restore(const char *path, int v)
{
	char err[PATH_MAX + 128];

	if (hist_restore(path, v, err, sizeof err) != 0) {
		fprintf(stderr, "my-undo: %s\n", err);
		return 1;
	}
	printf("Restored %s to version %d.\n", path, v);
	return 0;
}

static int undo_last(void)
{
	struct change *c;
	int n = hist_changes(getuid(), &c), rc = 1;

	for (int i = 0; i < n; i++) {
		char t[64];

		if (c[i].version <= 0)
			continue;
		when(c[i].when, t, sizeof t);
		printf("Last change: %s (version %d, %s)\n", c[i].path, c[i].version, t);
		rc = restore(c[i].path, c[i].version - 1);
		break;
	}
	if (rc && n == 0)
		fprintf(stderr, "my-undo: no changes recorded yet\n");
	free(c);
	return rc;
}

static int list(int max)
{
	struct change *c;
	int n = hist_changes(getuid(), &c);

	if (n == 0)
		printf("No changes recorded yet.\n");
	for (int i = 0; i < n && i < max; i++) {
		char t[64];

		when(c[i].when, t, sizeof t);
		printf("%s  v%-3d %s\n", t, c[i].version, c[i].path);
	}
	free(c);
	return 0;
}

static int history(const char *path)
{
	int v[MAXV], n = hist_versions(path, v, MAXV);

	if (n == 0) {
		fprintf(stderr, "my-undo: no history for %s (or it isn't yours)\n", path);
		return 1;
	}
	printf("Versions of %s:\n", path);
	for (int i = n - 1; i >= 0; i--) {
		char vf[PATH_MAX], t[64];
		struct stat st;

		hist_version_file(path, v[i], vf, sizeof vf);
		if (stat(vf, &st) != 0)
			continue;
		when(st.st_mtime, t, sizeof t);
		printf("  v%-3d %s  %8lld bytes%s\n", v[i], t, (long long)st.st_size,
		       v[i] == 0 ? "  (oldest: at boot or when first saved)" : i == n - 1 ? "  (current)" : "");
	}
	return 0;
}

static int show(const char *path, int v)
{
	char vf[PATH_MAX], buf[65536];
	FILE *f;
	size_t r;

	if (hist_version_file(path, v, vf, sizeof vf) != 0 || !(f = fopen(vf, "r"))) {
		fprintf(stderr, "my-undo: no version %d of %s\n", v, path);
		return 1;
	}
	while ((r = fread(buf, 1, sizeof buf, f)) > 0)
		fwrite(buf, 1, r, stdout);
	fclose(f);
	return 0;
}

int main(int argc, char **argv)
{
	char path[PATH_MAX];

	if (argc == 1)
		return undo_last();
	if (strcmp(argv[1], "list") == 0)
		return list(argc > 2 ? atoi(argv[2]) : 20);
	if (argc < 3)
		return usage();
	hist_abspath(argv[2], path, sizeof path);
	if (strcmp(argv[1], "history") == 0)
		return history(path);
	if (argc == 4 && strcmp(argv[1], "restore") == 0)
		return restore(path, atoi(argv[3]));
	if (argc == 4 && strcmp(argv[1], "show") == 0)
		return show(path, atoi(argv[3]));
	return usage();
}
