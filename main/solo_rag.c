/* solo_rag — flat-file vector store with brute-force cosine recall.
 *
 * Embeddings via openrouter_embed; records appended to /sdcard/rag.bin.
 * Brute-force cosine over every record on recall — fine up to ~10k
 * facts (SD-card seek + read at 1.5 Mbps).  No mmap; we stream the
 * file once, allocating per-record vec into PSRAM for the cosine
 * compare and freeing immediately.
 *
 * TT #370 — see solo_rag.h.
 */

#include "solo_rag.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "openrouter_client.h"

#define RAG_PATH      "/sdcard/rag.bin"
#define RAG_MAGIC     0x76474152u /* 'RAGv' little-endian */
#define NVS_NS        "settings"
#define NVS_NEXT_ID   "rag_next_id"

static const char *TAG = "solo_rag";

esp_err_t solo_rag_init(void) {
   /* Touch the file (creates if absent). */
   FILE *f = fopen(RAG_PATH, "ab");
   if (!f) {
      ESP_LOGW(TAG, "fopen(%s, ab) failed; SD card mounted?", RAG_PATH);
      return ESP_FAIL;
   }
   fclose(f);
   return ESP_OK;
}

static uint32_t next_fact_id(void) {
   nvs_handle_t h;
   uint32_t id = 1;
   if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
      nvs_get_u32(h, NVS_NEXT_ID, &id);
      nvs_set_u32(h, NVS_NEXT_ID, id + 1);
      nvs_commit(h);
      nvs_close(h);
   }
   return id;
}

esp_err_t solo_rag_remember(const char *text, uint32_t *out_fact_id) {
   if (!text || !*text) return ESP_ERR_INVALID_ARG;

   float *vec = NULL;
   size_t dim = 0;
   esp_err_t err = openrouter_embed(text, &vec, &dim);
   if (err != ESP_OK) return err;
   if (!vec || dim == 0) return ESP_FAIL;

   FILE *f = fopen(RAG_PATH, "ab");
   if (!f) {
      heap_caps_free(vec);
      return ESP_FAIL;
   }

   uint32_t magic = RAG_MAGIC;
   uint32_t fact_id = next_fact_id();
   uint32_t ts = (uint32_t)time(NULL);
   uint16_t vec_dim = (uint16_t)dim;
   size_t text_n = strlen(text);
   if (text_n > 4096) text_n = 4096;
   uint16_t text_len = (uint16_t)text_n;

   fwrite(&magic, sizeof magic, 1, f);
   fwrite(&fact_id, sizeof fact_id, 1, f);
   fwrite(&ts, sizeof ts, 1, f);
   fwrite(&vec_dim, sizeof vec_dim, 1, f);
   fwrite(&text_len, sizeof text_len, 1, f);
   fwrite(text, 1, text_len, f);
   fwrite(vec, sizeof(float), dim, f);
   fclose(f);
   heap_caps_free(vec);

   if (out_fact_id) *out_fact_id = fact_id;
   ESP_LOGI(TAG, "remember fact_id=%lu (vec_dim=%u, text_len=%u)",
            (unsigned long)fact_id, (unsigned)vec_dim, (unsigned)text_len);
   return ESP_OK;
}

static float cosine(const float *a, const float *b, size_t n) {
   double dot = 0, na = 0, nb = 0;
   for (size_t i = 0; i < n; i++) {
      dot += (double)a[i] * (double)b[i];
      na  += (double)a[i] * (double)a[i];
      nb  += (double)b[i] * (double)b[i];
   }
   if (na <= 0 || nb <= 0) return 0.0f;
   return (float)(dot / (sqrt(na) * sqrt(nb)));
}

esp_err_t solo_rag_recall(const char *query, int k,
                          solo_rag_hit_t *hits, int *n_hits) {
   if (!query || !hits || !n_hits || k <= 0) return ESP_ERR_INVALID_ARG;
   *n_hits = 0;

   float *qvec = NULL;
   size_t qdim = 0;
   esp_err_t err = openrouter_embed(query, &qvec, &qdim);
   if (err != ESP_OK) return err;
   if (!qvec || qdim == 0) return ESP_FAIL;

   FILE *f = fopen(RAG_PATH, "rb");
   if (!f) {
      heap_caps_free(qvec);
      return ESP_FAIL;
   }

   int got = 0;
   while (1) {
      uint32_t magic, fact_id, ts;
      uint16_t vd, tl;
      if (fread(&magic, sizeof magic, 1, f) != 1) break;
      if (magic != RAG_MAGIC) {
         ESP_LOGW(TAG, "bad magic mid-file — stopping scan");
         break;
      }
      fread(&fact_id, sizeof fact_id, 1, f);
      fread(&ts, sizeof ts, 1, f);
      fread(&vd, sizeof vd, 1, f);
      fread(&tl, sizeof tl, 1, f);
      char text_buf[4096];
      if (tl >= sizeof text_buf) tl = sizeof text_buf - 1;
      fread(text_buf, 1, tl, f);
      text_buf[tl] = '\0';
      float *vec = heap_caps_malloc((size_t)vd * sizeof(float),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!vec) break;
      fread(vec, sizeof(float), vd, f);

      if (vd != qdim) {
         heap_caps_free(vec);
         continue;
      }
      float score = cosine(qvec, vec, vd);
      heap_caps_free(vec);

      /* Top-K insertion: replace the lowest-scoring slot if score
       * beats it (or fill an empty slot until full). */
      int insert_at = -1;
      if (got < k) {
         insert_at = got;
      } else {
         int worst = 0;
         for (int i = 1; i < k; i++) {
            if (hits[i].score < hits[worst].score) worst = i;
         }
         if (score > hits[worst].score) insert_at = worst;
      }
      if (insert_at >= 0) {
         hits[insert_at].fact_id = fact_id;
         hits[insert_at].ts = ts;
         hits[insert_at].score = score;
         strncpy(hits[insert_at].text, text_buf, sizeof(hits[insert_at].text) - 1);
         hits[insert_at].text[sizeof(hits[insert_at].text) - 1] = '\0';
         if (got < k) got++;
      }
   }
   fclose(f);
   heap_caps_free(qvec);

   /* Sort hits descending by score (small-N selection sort). */
   for (int i = 0; i < got - 1; i++) {
      int best = i;
      for (int j = i + 1; j < got; j++) {
         if (hits[j].score > hits[best].score) best = j;
      }
      if (best != i) {
         solo_rag_hit_t tmp = hits[i];
         hits[i] = hits[best];
         hits[best] = tmp;
      }
   }
   *n_hits = got;
   return ESP_OK;
}

int solo_rag_count(void) {
   FILE *f = fopen(RAG_PATH, "rb");
   if (!f) return 0;
   int n = 0;
   while (1) {
      uint32_t magic;
      if (fread(&magic, sizeof magic, 1, f) != 1) break;
      if (magic != RAG_MAGIC) break;
      uint32_t fact_id, ts;
      uint16_t vd, tl;
      fread(&fact_id, sizeof fact_id, 1, f);
      fread(&ts, sizeof ts, 1, f);
      fread(&vd, sizeof vd, 1, f);
      fread(&tl, sizeof tl, 1, f);
      fseek(f, (long)tl + (long)vd * (long)sizeof(float), SEEK_CUR);
      n++;
   }
   fclose(f);
   return n;
}
