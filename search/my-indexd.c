/*
 * my-indexd: keeps the search index of your home folder up to date.
 *
 * Started for every desktop session (Openbox autostart), runs as you.
 * Every 5 seconds it walks $HOME (skipping hidden files and folders) and
 * compares each file's modification time and size with the index:
 *   - new or changed file: cut into chunks, one vector per chunk
 *   - deleted file: its chunks are removed
 * PDFs (pdftotext) and office files (.odt, .docx, ...: unzip) by their text,
 * like text files; other binary files (pictures, programs) by name only.
 * Progress goes to ~/.cache/my-search/status for the search window.
 */
#define _GNU_SOURCE
#include "search.h"

#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SCAN_SECONDS 5
#define MAX_FILES    20000

struct file {
	char *path;
	int64_t mtime, size;
};

static struct index ix;
static struct file *files;      /* what this scan found */
static int nfiles;
static char index_path[4096], status_path[4096];
static float *vec;

static int collect(const char *path, const struct stat *st, int type,
                   struct FTW *ftw)
{
	const char *name = path + ftw->base;

	if (ftw->level > 0 && name[0] == '.')          /* hidden: skip */
		return type == FTW_D ? FTW_SKIP_SUBTREE : FTW_CONTINUE;
	if (type == FTW_F && S_ISREG(st->st_mode) && nfiles < MAX_FILES) {
		files[nfiles].path = strdup(path);
		files[nfiles].mtime = st->st_mtime;
		files[nfiles].size = st->st_size;
		nfiles++;
	}
	return FTW_CONTINUE;
}

static void status(const char *fmt, int a, int b)
{
	FILE *f = fopen(status_path, "w");

	if (f) {
		fprintf(f, fmt, a, b);
		fputc('\n', f);
		fclose(f);
	}
}

/* Splits a file into chunks and adds one vector per chunk to the index. */
static void index_file(const struct file *fl)
{
	char words[256], piece[CHUNK_CHARS + 1], *input;
	size_t len = 0, pos = 0;
	/* PDFs and office files through their converters, then plain text */
	char *text = fl->size <= MAX_DOC_SIZE ? read_document(fl->path, &len) : NULL;
	int chunks = 0;

	if (!text && fl->size <= MAX_FILE_SIZE)
		text = read_text(fl->path, &len);

	name_words(fl->path, words, sizeof words);
	input = malloc(sizeof words + sizeof piece + 16);
	if (!input) {
		free(text);
		return;
	}
	if (!text || len == 0) {
		/* no text: the name alone */
		snprintf(input, sizeof words + 16, "%s", words);
		if (embed_text(input, vec) == 0)
			index_add(&ix, fl->path, fl->mtime, fl->size, "", vec);
		free(text);
		free(input);
		return;
	}
	while (pos < len && chunks < MAX_CHUNKS_FILE) {
		size_t end = pos + CHUNK_CHARS;

		if (end >= len) {
			end = len;
		} else {
			/* end at a space or line break, not inside a word */
			size_t cut = end;

			while (cut > pos + CHUNK_CHARS / 2 && text[cut] != ' ' &&
			       text[cut] != '\n')
				cut--;
			if (cut > pos + CHUNK_CHARS / 2)
				end = cut;
			while (end > pos && ((unsigned char)text[end] & 0xC0) == 0x80)
				end--;                   /* not inside a UTF-8 letter */
			if (end <= pos)              /* never get stuck */
				end = pos + CHUNK_CHARS;
		}
		memcpy(piece, text + pos, end - pos);
		piece[end - pos] = '\0';
		/* the file name goes with every chunk, so it counts for the meaning */
		snprintf(input, sizeof words + sizeof piece + 16, "%s\n%s", words, piece);
		if (embed_text(input, vec) == 0)
			index_add(&ix, fl->path, fl->mtime, fl->size, piece, vec);
		chunks++;
		pos = end;
		while (pos < len && (text[pos] == ' ' || text[pos] == '\n'))
			pos++;
	}
	free(text);
	free(input);
}

static int cmp_path(const void *a, const void *b)
{
	return strcmp(((const struct file *)a)->path, ((const struct file *)b)->path);
}

static void scan(const char *home)
{
	int changed = 0, todo = 0, done = 0;
	char *seen;

	nfiles = 0;
	nftw(home, collect, 32, FTW_PHYS | FTW_MOUNT | FTW_ACTIONRETVAL);
	qsort(files, nfiles, sizeof *files, cmp_path);
	seen = calloc(nfiles + 1, 1);       /* seen[i]: file i is in the index as is */

	/* Drop chunks of files that are gone or changed. */
	for (int i = ix.n - 1; i >= 0 && seen; i--) {
		struct file key = { ix.c[i].path, 0, 0 };
		struct file *f = bsearch(&key, files, nfiles, sizeof *files, cmp_path);

		if (f && f->mtime == ix.c[i].mtime && f->size == ix.c[i].size) {
			seen[f - files] = 1;
		} else {
			index_remove(&ix, i);
			changed = 1;
		}
	}
	for (int i = 0; i < nfiles && seen; i++)
		todo += !seen[i];

	/* (Re)index the new and changed files. */
	for (int i = 0; i < nfiles && seen; i++) {
		if (seen[i])
			continue;
		status("Indexing: %d of %d files", ++done, todo);
		index_file(&files[i]);
		changed = 1;
	}
	if (changed) {
		index_save(&ix, index_path);
		status("Indexed %d files (%d sections)", nfiles, ix.n);
	} else if (todo == 0 && access(status_path, F_OK) != 0) {
		status("Indexed %d files (%d sections)", nfiles, ix.n);
	}
	for (int i = 0; i < nfiles; i++)
		free(files[i].path);
	free(seen);
}

int main(void)
{
	const char *home = getenv("HOME");
	char dir[4096], lock[4096];
	int lockfd;

	if (!home) {
		fprintf(stderr, "my-indexd: HOME is not set\n");
		return 1;
	}
	if (nice(10) == -1)                   /* lower priority than your apps */
		perror("my-indexd: nice");
	/* ~/.cache/my-search, creating ~/.cache too if needed */
	snprintf(dir, sizeof dir, "%s/.cache", home);
	mkdir(dir, 0700);
	search_file("", dir, sizeof dir);
	mkdir(dir, 0700);
	search_file("index", index_path, sizeof index_path);
	search_file("status", status_path, sizeof status_path);

	/* only one indexer per user */
	search_file("lock", lock, sizeof lock);
	lockfd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (lockfd < 0) {
		perror(lock);
		return 1;
	}
	if (flock(lockfd, LOCK_EX | LOCK_NB) != 0) {
		fprintf(stderr, "my-indexd: already running\n");
		return 0;
	}

	if (embed_init(MODEL_PATH) != 0)
		return 1;
	vec = malloc(embed_dim() * sizeof(float));
	files = malloc(MAX_FILES * sizeof *files);
	if (!vec || !files)
		return 1;
	index_load(&ix, index_path, embed_dim());

	for (;;) {
		scan(home);
		sleep(SCAN_SECONDS);
	}
}
