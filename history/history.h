/*
 * Shared by my-historyd (records versions), my-undo (command line) and
 * my-history (window): where versions are stored and how to read them.
 *
 * Layout on disk (everything lives in RAM, like the rest of the live system):
 *
 *   /var/lib/my-history/store/<encoded path>/<N>   version N of a file
 *   /var/lib/my-history/log/<uid>                  "time<TAB>N<TAB>path" lines
 *
 * <encoded path> is the absolute path with '%' -> "%25" and '/' -> "%2F",
 * e.g. /home/dominic/notes.txt -> %2Fhome%2Fdominic%2Fnotes.txt
 * Version 0 is the oldest one: the state at boot (for /etc, /root, /home)
 * or the first save of a new file. 1, 2, ... are the changes after that.
 * Each version directory, version file and log belongs to the file's owner
 * (mode 0700/0600), so users can read their own history and nobody else's.
 */
#ifndef HISTORY_H
#define HISTORY_H

#include <limits.h>
#include <stddef.h>
#include <sys/types.h>
#include <time.h>

#define HIST_DIR   "/var/lib/my-history"
#define HIST_STORE HIST_DIR "/store"
#define HIST_LOG   HIST_DIR "/log"

struct change {
	time_t when;
	int version;              /* the version this change created */
	char path[PATH_MAX];
};

/* Directory holding the versions of path. Returns 0, or -1 if too long. */
int hist_dir(const char *path, char *out, size_t n);

/* File holding version v of path. */
int hist_version_file(const char *path, int v, char *out, size_t n);

/* Version numbers of path, ascending. Returns how many (max entries). */
int hist_versions(const char *path, int *out, int max);

/* Changes, newest first. uid 0 reads everyone's log, others only their own.
 * Returns the count; *out is malloc'd (free it). */
int hist_changes(uid_t uid, struct change **out);

/* Writes version v back to path. Returns 0, or -1 with a message in err. */
int hist_restore(const char *path, int v, char *err, size_t n);

/* Makes a path absolute (relative to the current directory). */
void hist_abspath(const char *in, char *out, size_t n);

#endif
