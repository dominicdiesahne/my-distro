/* The index file, reading text files, and ranking search results. */
#define _GNU_SOURCE
#include "search.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAGIC "MYSEARCH1"

void search_file(const char *name, char *out, size_t n)
{
	const char *home = getenv("HOME");

	snprintf(out, n, "%s/.cache/my-search/%s", home ? home : "/tmp", name);
}

void index_add(struct index *ix, const char *path, int64_t mtime,
               int64_t size, const char *text, const float *vec)
{
	struct chunk *c;

	if (ix->n == ix->cap) {
		int cap = ix->cap ? ix->cap * 2 : 256;
		struct chunk *bigger = realloc(ix->c, cap * sizeof *bigger);

		if (!bigger)
			return;
		ix->c = bigger;
		ix->cap = cap;
	}
	c = &ix->c[ix->n];
	c->path = strdup(path);
	c->text = strdup(text);
	c->vec = malloc(ix->dim * sizeof(float));
	if (!c->path || !c->text || !c->vec)
		return;
	memcpy(c->vec, vec, ix->dim * sizeof(float));
	c->mtime = mtime;
	c->size = size;
	ix->n++;
}

void index_remove(struct index *ix, int i)
{
	free(ix->c[i].path);
	free(ix->c[i].text);
	free(ix->c[i].vec);
	ix->c[i] = ix->c[--ix->n];        /* order doesn't matter */
}

void index_free(struct index *ix)
{
	while (ix->n > 0)
		index_remove(ix, ix->n - 1);
	free(ix->c);
	ix->c = NULL;
	ix->cap = 0;
}

/*
 * File format: "MYSEARCH1", dim, count, then per chunk:
 * path length + path, mtime, size, text length + text, dim floats.
 */
static int put(FILE *f, const void *p, size_t n)
{
	return fwrite(p, 1, n, f) == n ? 0 : -1;
}

static int get(FILE *f, void *p, size_t n)
{
	return fread(p, 1, n, f) == n ? 0 : -1;
}

int index_save(const struct index *ix, const char *file)
{
	char tmp[4096];
	FILE *f;
	int err = 0;

	snprintf(tmp, sizeof tmp, "%s.tmp", file);
	f = fopen(tmp, "wb");
	if (!f)
		return -1;
	err |= put(f, MAGIC, 9);
	err |= put(f, &ix->dim, sizeof ix->dim);
	err |= put(f, &ix->n, sizeof ix->n);
	for (int i = 0; i < ix->n && !err; i++) {
		const struct chunk *c = &ix->c[i];
		uint32_t pl = strlen(c->path), tl = strlen(c->text);

		err |= put(f, &pl, 4) | put(f, c->path, pl);
		err |= put(f, &c->mtime, 8) | put(f, &c->size, 8);
		err |= put(f, &tl, 4) | put(f, c->text, tl);
		err |= put(f, c->vec, ix->dim * sizeof(float));
	}
	if (fclose(f) != 0 || err) {
		unlink(tmp);
		return -1;
	}
	return rename(tmp, file);       /* readers see the old or the new index */
}

int index_load(struct index *ix, const char *file, int dim)
{
	FILE *f = fopen(file, "rb");
	char magic[9];
	int fdim, n;
	char *path = NULL, *text = NULL;
	float *vec = NULL;

	ix->dim = dim;
	ix->n = 0;
	if (!f)
		return -1;
	if (get(f, magic, 9) || memcmp(magic, MAGIC, 9) || get(f, &fdim, 4) ||
	    get(f, &n, 4) || fdim != dim || n < 0) {
		fclose(f);
		return -1;
	}
	vec = malloc(dim * sizeof(float));
	for (int i = 0; i < n && vec; i++) {
		uint32_t pl, tl;
		int64_t mtime, size;

		if (get(f, &pl, 4) || pl > 4096 || !(path = malloc(pl + 1)) ||
		    get(f, path, pl) || get(f, &mtime, 8) || get(f, &size, 8) ||
		    get(f, &tl, 4) || tl > 65536 || !(text = malloc(tl + 1)) ||
		    get(f, text, tl) || get(f, vec, dim * sizeof(float)))
			break;
		path[pl] = text[tl] = '\0';
		index_add(ix, path, mtime, size, text, vec);
		free(path);
		free(text);
		path = text = NULL;
	}
	free(path);
	free(text);
	free(vec);
	fclose(f);
	return 0;
}

char *read_text(const char *path, size_t *len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	char *buf;
	ssize_t r, total = 0;

	if (fd < 0)
		return NULL;
	buf = malloc(MAX_FILE_SIZE + 1);
	while (buf && total < MAX_FILE_SIZE &&
	       (r = read(fd, buf + total, MAX_FILE_SIZE - total)) > 0)
		total += r;
	close(fd);
	if (!buf)
		return NULL;
	/* a zero byte means binary (images, programs, ...): not text */
	if (memchr(buf, '\0', total)) {
		free(buf);
		return NULL;
	}
	buf[total] = '\0';
	*len = total;
	return buf;
}

/* --- Documents: PDFs and office files -------------------------------------- */

/* Runs a program and returns what it prints, at most MAX_FILE_SIZE bytes
 * (NULL if it failed or printed nothing). No shell: the file name can't be
 * mistaken for a command. */
static char *capture(char *const argv[], size_t *len)
{
	int fd[2], st;
	pid_t pid;
	char *buf;
	ssize_t r;
	size_t total = 0;

	if (pipe(fd) != 0)
		return NULL;
	pid = fork();
	if (pid == 0) {
		int null = open("/dev/null", O_RDWR);

		dup2(null, 0);
		dup2(fd[1], 1);
		dup2(null, 2);
		close(fd[0]);
		close(fd[1]);
		execvp(argv[0], argv);
		_exit(127);
	}
	close(fd[1]);
	buf = pid > 0 ? malloc(MAX_FILE_SIZE + 1) : NULL;
	while (buf && total < MAX_FILE_SIZE && (r = read(fd[0], buf + total, MAX_FILE_SIZE - total)) > 0)
		total += r;
	close(fd[0]);                       /* more than that: the program stops */
	if (pid > 0)
		waitpid(pid, &st, 0);
	if (!buf || total == 0) {
		free(buf);
		return NULL;
	}
	buf[total] = '\0';
	*len = total;
	return buf;
}

/* The text of office XML: tags become spaces (a paragraph's end a line
 * break), &amp; and friends their characters. In place. */
static size_t strip_xml(char *s)
{
	char *in = s, *out = s;
	static const struct { const char *ent, *ch; } ents[] = {
		{ "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" },
		{ "&apos;", "'" }, { NULL, NULL } };

	while (*in) {
		if (*in == '<') {
			int para = strncmp(in, "</w:p>", 6) == 0 || strncmp(in, "</text:p", 8) == 0 ||
			           strncmp(in, "</text:h", 8) == 0 || strncmp(in, "</a:p>", 6) == 0;
			char *end = strchr(in, '>');

			if (!end)
				break;
			in = end + 1;
			if (out > s && out[-1] != ' ' && out[-1] != '\n')
				*out++ = para ? '\n' : ' ';
			else if (para && out > s)
				out[-1] = '\n';
			continue;
		}
		if (*in == '&') {
			int done = 0;

			for (int i = 0; ents[i].ent; i++)
				if (strncmp(in, ents[i].ent, strlen(ents[i].ent)) == 0) {
					*out++ = ents[i].ch[0];
					in += strlen(ents[i].ent);
					done = 1;
					break;
				}
			if (done)
				continue;
		}
		*out++ = *in++;
	}
	*out = '\0';
	return out - s;
}

/*
 * Text of documents the search understands besides plain text:
 *   .pdf                  pdftotext (poppler), the first 30 pages
 *   .odt .ods .odp        LibreOffice: content.xml in the zip file
 *   .docx .xlsx .pptx     Microsoft Office: the XML with the text in the zip
 * Others: NULL. Each program gets 20 seconds (timeout), so a broken
 * file can't stop the indexer.
 */
char *read_document(const char *path, size_t *len)
{
	const char *dot = strrchr(path, '.');
	char ext[8] = "";
	char *text;

	if (!dot || strchr(dot, '/') || strlen(dot) > 6)
		return NULL;
	for (int i = 0; dot[i + 1] && i < 7; i++)
		ext[i] = tolower((unsigned char)dot[i + 1]);

	if (strcmp(ext, "pdf") == 0) {
		char *const argv[] = { "timeout", "20", "pdftotext", "-l", "30", "-enc", "UTF-8",
		                       "-q", (char *)path, "-", NULL };

		return capture(argv, len);
	}
	const char *member =
		!strcmp(ext, "odt") || !strcmp(ext, "ods") || !strcmp(ext, "odp") ? "content.xml" :
		!strcmp(ext, "docx") ? "word/document.xml" :
		!strcmp(ext, "xlsx") ? "xl/sharedStrings.xml" :
		!strcmp(ext, "pptx") ? "ppt/slides/slide*.xml" : NULL;
	if (!member)
		return NULL;
	char *const argv[] = { "timeout", "20", "unzip", "-p", (char *)path, (char *)member, NULL };

	text = capture(argv, len);
	if (text)
		*len = strip_xml(text);
	return text;
}

void name_words(const char *path, char *out, size_t n)
{
	const char *base = strrchr(path, '/');
	size_t i = 0;

	base = base ? base + 1 : path;
	for (; *base && i + 1 < n; base++)
		out[i++] = (*base == '_' || *base == '-' || *base == '.') ? ' ' : *base;
	out[i] = '\0';
}

/* Share of the query's words (3+ letters) that appear in the chunk or path:
 * gives exact matches a small push on top of the meaning. */
static float keyword_share(const char *query, const struct chunk *c)
{
	char word[64];
	int words = 0, found = 0;

	while (*query) {
		size_t len = 0;

		while (*query && !isalnum((unsigned char)*query) && !(*query & 0x80))
			query++;
		while ((isalnum((unsigned char)*query) || (*query & 0x80)) &&
		       len + 1 < sizeof word)
			word[len++] = *query++;
		word[len] = '\0';
		if (len < 3)
			continue;
		words++;
		if (strcasestr(c->text, word) || strcasestr(c->path, word))
			found++;
	}
	return words ? (float)found / words : 0;
}

int index_search(const struct index *ix, const float *qvec, const char *query,
                 struct hit *out, int max)
{
	int n = 0;

	for (int i = 0; i < ix->n; i++) {
		const struct chunk *c = &ix->c[i];
		float score = 0;
		int j;

		for (int d = 0; d < ix->dim; d++)
			score += qvec[d] * c->vec[d];        /* cosine similarity */
		score += 0.15f * keyword_share(query, c);

		/* one hit per file: keep its best chunk */
		for (j = 0; j < n; j++)
			if (strcmp(ix->c[out[j].chunk].path, c->path) == 0)
				break;
		if (j < n) {
			if (score > out[j].score)
				out[j] = (struct hit){ i, score };
			continue;
		}
		if (n < max) {
			out[n++] = (struct hit){ i, score };
		} else {
			int worst = 0;

			for (int k = 1; k < n; k++)
				if (out[k].score < out[worst].score)
					worst = k;
			if (score > out[worst].score)
				out[worst] = (struct hit){ i, score };
		}
	}
	/* best first (insertion sort; max is small) */
	for (int i = 1; i < n; i++)
		for (int k = i; k > 0 && out[k].score > out[k - 1].score; k--) {
			struct hit t = out[k];

			out[k] = out[k - 1];
			out[k - 1] = t;
		}
	while (n > 0 && out[n - 1].score < MIN_SCORE)   /* drop the noise */
		n--;
	return n;
}
