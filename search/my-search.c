/*
 * my-search: search your files by meaning, from the command line.
 *
 *   my-search what you are looking for
 *   my-search --status
 *
 * Shows the best matching files with a score (higher = closer in meaning)
 * and the part of the file that matched.
 */
#include "search.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RESULTS 10

static void print_preview(const char *text)
{
	int col = 0;

	printf("      ");
	for (const char *p = text; *p && col < 150; p++, col++)
		putchar(*p == '\n' || *p == '\t' ? ' ' : *p);
	printf("%s\n", strlen(text) > 150 ? "..." : "");
}

int main(int argc, char **argv)
{
	char query[2048] = "", file[4096], line[256];
	struct index ix = { 0 };
	struct hit hits[RESULTS];
	float *q;
	int n;
	FILE *f;

	if (argc < 2) {
		fprintf(stderr, "usage: my-search WORDS...   (or --status)\n");
		return 2;
	}
	search_file("status", file, sizeof file);
	if (strcmp(argv[1], "--status") == 0) {
		f = fopen(file, "r");
		if (f && fgets(line, sizeof line, f))
			fputs(line, stdout);
		else
			printf("The index isn't ready yet (my-indexd builds it).\n");
		if (f)
			fclose(f);
		return 0;
	}
	for (int i = 1; i < argc; i++) {
		strncat(query, argv[i], sizeof query - strlen(query) - 2);
		strcat(query, " ");
	}

	if (embed_init(MODEL_PATH) != 0)
		return 1;
	search_file("index", file, sizeof file);
	if (index_load(&ix, file, embed_dim()) != 0 || ix.n == 0) {
		printf("The index is empty or not built yet. Is my-indexd running?\n");
		return 1;
	}
	q = malloc(embed_dim() * sizeof(float));
	if (!q || embed_text(query, q) != 0) {
		fprintf(stderr, "my-search: couldn't process the query\n");
		return 1;
	}
	n = index_search(&ix, q, query, hits, RESULTS);
	for (int i = 0; i < n; i++) {
		const struct chunk *c = &ix.c[hits[i].chunk];

		printf("%3d%%  %s\n", (int)(hits[i].score * 100 + 0.5), c->path);
		if (c->text[0])
			print_preview(c->text);
	}
	if (n == 0)
		printf("Nothing found.\n");
	return 0;
}
