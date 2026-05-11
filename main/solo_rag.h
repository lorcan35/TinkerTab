/**
 * solo_rag — Tab5 on-device RAG store for vmode=5 SOLO_DIRECT.
 *
 * Flat float32 records appended to /sdcard/rag.bin.  Each record:
 *   uint32_t magic       = 'RAGv'
 *   uint32_t fact_id     monotonic, NVS-persisted in "rag_next_id"
 *   uint32_t ts_unix
 *   uint16_t vec_dim     (default 1536 for text-embedding-3-small)
 *   uint16_t text_len
 *   char     text[text_len]
 *   float    vec[vec_dim]
 *
 * Embeddings via openrouter_embed.  Recall is brute-force cosine
 * over every record — ≤10k records is the soft ceiling at SD-1.5
 * Mbps for sub-5s recall.  Most-recent-N PSRAM hot cache is a
 * Phase-2 optimisation.
 *
 * TT #370 — see docs/superpowers/specs/2026-05-08-tab5-solo-mode-design.md
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t solo_rag_init(void);

/** Embed `text` via openrouter_embed and append a record.  *out_fact_id
 *  is set to the monotonic id assigned to this fact. */
esp_err_t solo_rag_remember(const char *text, uint32_t *out_fact_id);

typedef struct {
   uint32_t fact_id;
   uint32_t ts;
   float score;
   char text[256];
} solo_rag_hit_t;

/** Embed `query` and return the top-K cosine matches in `hits`/`*n_hits`.
 *  hits is caller-owned; size must be ≥ k.  k <= 8 is the practical
 *  ceiling (linear top-K insertion). */
esp_err_t solo_rag_recall(const char *query, int k, solo_rag_hit_t *hits, int *n_hits);

/** Number of records currently stored. */
int solo_rag_count(void);

#ifdef __cplusplus
}
#endif
