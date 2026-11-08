/*
 * Semantic file search for my-distro, shared by my-indexd (builds the
 * index), my-search (command line) and my-search-gui (window).
 *
 * How it works: an embedding model (all-MiniLM-L6-v2, run by llama.cpp)
 * turns a piece of text into 384 numbers, a vector that describes what the
 * text is about. Texts about similar things get vectors that point in a
 * similar direction, even when they use different words. We cut every file
 * into chunks of ~700 characters, store one vector per chunk, and for a
 * search compare the query's vector with all of them (cosine similarity).
 *
 * The index lives in ~/.cache/my-search/index, one per user, so you can
 * only ever search your own files.
 */
#ifndef SEARCH_H
#define SEARCH_H

#include <stdint.h>
#include <stddef.h>

#define MODEL_PATH "/usr/share/my-search/all-MiniLM-L6-v2-Q8_0.gguf"
#define CHUNK_CHARS     700        /* text per chunk */
#define MAX_CHUNKS_FILE 30         /* longer files: only the first 30 chunks */
#define MAX_FILE_SIZE   (1 << 20)  /* text files up to 1 MB are read */
#define MIN_SCORE       0.15f      /* weaker matches are just noise */

/* --- embed.c: the model ---------------------------------------------------- */

int embed_init(const char *model_path);   /* 0 = ok */
int embed_dim(void);
int embed_text(const char *text, float *out);   /* unit-length vector; 0 = ok */
void embed_free(void);

/* --- index.c: the index file and text helpers ------------------------------ */

struct chunk {
	char *path;
	int64_t mtime, size;
	char *text;          /* the chunk's text, shown as preview */
	float *vec;          /* dim floats */
};

struct index {
	int dim, n, cap;
	struct chunk *c;
};

/* ~/.cache/my-search/<name> */
void search_file(const char *name, char *out, size_t n);

int index_load(struct index *ix, const char *file, int dim);   /* 0 = ok */
int index_save(const struct index *ix, const char *file);
void index_add(struct index *ix, const char *path, int64_t mtime,
               int64_t size, const char *text, const float *vec);
void index_remove(struct index *ix, int i);
void index_free(struct index *ix);

/* Reads a text file (NULL for binary files). Caller frees. */
char *read_text(const char *path, size_t *len);

/* The text of a PDF or office file (.pdf, .odt, .docx, ...; NULL for other
 * files), at most MAX_FILE_SIZE bytes of it. Caller frees. */
char *read_document(const char *path, size_t *len);
#define MAX_DOC_SIZE    (64 << 20) /* documents up to 64 MB are read */

/* "my_gym-contract.txt" -> "my gym contract txt": words a model understands */
void name_words(const char *path, char *out, size_t n);

struct hit {
	int chunk;           /* best matching chunk of the file */
	float score;
};

/* Best files for a query (one hit per file), best first. Returns the count. */
int index_search(const struct index *ix, const float *qvec, const char *query,
                 struct hit *out, int max);

#endif
