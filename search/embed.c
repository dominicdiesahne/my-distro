/* Turns text into a vector with llama.cpp and the all-MiniLM-L6-v2 model. */
#include "search.h"

#include <llama.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_TOKENS 256      /* MiniLM was trained on texts up to 256 tokens */

static struct llama_model *model;
static struct llama_context *ctx;
static const struct llama_vocab *vocab;
static int dim;

/* llama.cpp is chatty; only pass on real errors. */
static void quiet_log(enum ggml_log_level level, const char *text, void *data)
{
	(void)data;
	if (level == GGML_LOG_LEVEL_ERROR)
		fputs(text, stderr);
}

int embed_init(const char *model_path)
{
	struct llama_model_params mp;
	struct llama_context_params cp;
	long cpus = sysconf(_SC_NPROCESSORS_ONLN);
	const char *env = getenv("MY_SEARCH_MODEL");   /* other model, for testing */

	if (env && *env)
		model_path = env;
	llama_log_set(quiet_log, NULL);
	llama_backend_init();

	mp = llama_model_default_params();
	model = llama_model_load_from_file(model_path, mp);
	if (!model) {
		fprintf(stderr, "can't load the model %s\n", model_path);
		return -1;
	}
	cp = llama_context_default_params();
	cp.embeddings = true;                       /* we want vectors, not text */
	cp.pooling_type = LLAMA_POOLING_TYPE_MEAN;  /* average over all tokens */
	cp.n_ctx = 512;
	cp.n_batch = 512;
	cp.n_ubatch = 512;
	cp.n_threads = cp.n_threads_batch = cpus > 4 ? 4 : cpus < 1 ? 1 : cpus;
	ctx = llama_init_from_model(model, cp);
	if (!ctx) {
		fprintf(stderr, "can't create the model context\n");
		return -1;
	}
	vocab = llama_model_get_vocab(model);
	dim = llama_model_n_embd(model);
	return 0;
}

int embed_dim(void)
{
	return dim;
}

int embed_text(const char *text, float *out)
{
	int len = strlen(text);
	llama_token *tok = malloc((len + 16) * sizeof *tok);
	int n, rc;
	const float *e;
	double norm = 0;
	llama_memory_t mem;

	if (!tok)
		return -1;
	/* text -> token numbers, with the model's [CLS] ... [SEP] around it */
	n = llama_tokenize(vocab, text, len, tok, len + 16, true, false);
	if (n <= 0) {
		free(tok);
		return -1;
	}
	if (n > MAX_TOKENS) {                 /* too long: cut, keep the end mark */
		tok[MAX_TOKENS - 1] = tok[n - 1];
		n = MAX_TOKENS;
	}

	mem = llama_get_memory(ctx);
	if (mem)
		llama_memory_clear(mem, true);
	struct llama_batch batch = llama_batch_get_one(tok, n);
	if (llama_model_has_encoder(model) && !llama_model_has_decoder(model))
		rc = llama_encode(ctx, batch);
	else
		rc = llama_decode(ctx, batch);
	free(tok);
	if (rc != 0)
		return -1;

	e = llama_get_embeddings_seq(ctx, 0);
	if (!e)
		return -1;
	/* scale to length 1, so a dot product is the cosine similarity */
	for (int i = 0; i < dim; i++)
		norm += (double)e[i] * e[i];
	norm = sqrt(norm);
	for (int i = 0; i < dim; i++)
		out[i] = norm > 0 ? e[i] / norm : 0;
	return 0;
}

void embed_free(void)
{
	if (ctx)
		llama_free(ctx);
	if (model)
		llama_model_free(model);
	llama_backend_free();
}
