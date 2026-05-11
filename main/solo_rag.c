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
#include <unistd.h> /* ftruncate, fileno */

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "openrouter_client.h"

#define RAG_PATH "/sdcard/rag.bin"
#define RAG_MAGIC 0x76474152u /* 'RAGv' little-endian */
#define RAG_TEXT_MAX 4095u    /* W1-B: matches reader buffer cap */
#define NVS_NS "settings"
#define NVS_NEXT_ID "rag_next_id"

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

/* W3-D (TT #374): scan the on-disk file for the highest fact_id.  Used
 * as a fallback seed when NVS is unavailable so we never collide with
 * an existing record. */
static uint32_t scan_disk_max_fact_id(void) {
   FILE *f = fopen(RAG_PATH, "rb");
   if (!f) return 0;
   uint32_t max_id = 0;
   while (1) {
      uint32_t magic, fact_id, ts;
      uint16_t vd, tl;
      if (fread(&magic, sizeof magic, 1, f) != 1) break;
      if (magic != RAG_MAGIC) break;
      if (fread(&fact_id, sizeof fact_id, 1, f) != 1) break;
      if (fread(&ts, sizeof ts, 1, f) != 1) break;
      if (fread(&vd, sizeof vd, 1, f) != 1) break;
      if (fread(&tl, sizeof tl, 1, f) != 1) break;
      if (fseek(f, (long)tl + (long)vd * (long)sizeof(float), SEEK_CUR) != 0) break;
      if (fact_id > max_id) max_id = fact_id;
   }
   fclose(f);
   return max_id;
}

/* W3-D: monotonic id even if NVS open/get fails.  Old code defaulted
 * to id=1 on failure — two distinct facts could end up with the same
 * fact_id, breaking recall (returns wrong (fact_id, text) pair). */
static uint32_t next_fact_id(void) {
   nvs_handle_t h;
   uint32_t id = 0;
   bool from_nvs = false;
   if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
      if (nvs_get_u32(h, NVS_NEXT_ID, &id) == ESP_OK) {
         from_nvs = true;
      }
      if (!from_nvs) {
         /* First-boot OR NVS slot missing — seed from disk max so we
          * resume above the highest existing fact_id. */
         id = scan_disk_max_fact_id() + 1;
         ESP_LOGW(TAG, "next_fact_id: NVS slot missing — seeded from disk max → %lu", (unsigned long)id);
      }
      nvs_set_u32(h, NVS_NEXT_ID, id + 1);
      nvs_commit(h);
      nvs_close(h);
      return id;
   }
   /* NVS open itself failed — scan disk every call.  Slower but correct. */
   id = scan_disk_max_fact_id() + 1;
   ESP_LOGW(TAG, "next_fact_id: NVS unavailable — disk scan → %lu", (unsigned long)id);
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
   /* W1-B (TT #372): symmetric write/read cap.  Old code wrote up to
    * 4096 bytes but reader was capped at 4095, leaving 1 byte unread →
    * file misalignment from that record onward.  Write-side now caps at
    * 4095 so the reader's 4096-byte buffer fully consumes it. */
   if (text_n > RAG_TEXT_MAX) text_n = RAG_TEXT_MAX;
   uint16_t text_len = (uint16_t)text_n;

   /* W1-B: check every fwrite — partial write on the SD card means we
    * just appended a corrupt record.  Truncate on first failure to
    * keep the file readable for the next boot. */
   long start_off = ftell(f);
   bool ok = fwrite(&magic, sizeof magic, 1, f) == 1 && fwrite(&fact_id, sizeof fact_id, 1, f) == 1 &&
             fwrite(&ts, sizeof ts, 1, f) == 1 && fwrite(&vec_dim, sizeof vec_dim, 1, f) == 1 &&
             fwrite(&text_len, sizeof text_len, 1, f) == 1 && fwrite(text, 1, text_len, f) == text_len &&
             fwrite(vec, sizeof(float), dim, f) == dim;
   if (!ok) {
      ESP_LOGE(TAG, "remember: short write at offset %ld — truncating", start_off);
      fflush(f);
      int fd = fileno(f);
      if (fd >= 0 && start_off >= 0) {
         (void)ftruncate(fd, start_off);
      }
   }
   fclose(f);
   heap_caps_free(vec);
   if (!ok) return ESP_FAIL;

   if (out_fact_id) *out_fact_id = fact_id;
   ESP_LOGI(TAG, "remember fact_id=%lu (vec_dim=%u, text_len=%u)", (unsigned long)fact_id, (unsigned)vec_dim,
            (unsigned)text_len);
   return ESP_OK;
}

static float cosine(const float *a, const float *b, size_t n) {
   double dot = 0, na = 0, nb = 0;
   for (size_t i = 0; i < n; i++) {
      dot += (double)a[i] * (double)b[i];
      na += (double)a[i] * (double)a[i];
      nb += (double)b[i] * (double)b[i];
   }
   if (na <= 0 || nb <= 0) return 0.0f;
   return (float)(dot / (sqrt(na) * sqrt(nb)));
}

esp_err_t solo_rag_recall(const char *query, int k, solo_rag_hit_t *hits, int *n_hits) {
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

   /* W1-B (TT #372): every record read now checks fread return values.
    * On short read we abandon the whole scan — the file is corrupt past
    * this point and continuing would feed garbage to cosine.  text_buf
    * lives in PSRAM (was a 4 KB stack array — same class as the bug we
    * fixed in voice_solo.c). */
   const size_t text_cap = RAG_TEXT_MAX + 1;
   char *text_buf = heap_caps_malloc(text_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!text_buf) {
      fclose(f);
      heap_caps_free(qvec);
      return ESP_ERR_NO_MEM;
   }
   int got = 0;
   while (1) {
      uint32_t magic, fact_id, ts;
      uint16_t vd, tl;
      if (fread(&magic, sizeof magic, 1, f) != 1) break;
      if (magic != RAG_MAGIC) {
         ESP_LOGW(TAG, "bad magic at offset %ld — stopping scan", ftell(f) - 4);
         break;
      }
      if (fread(&fact_id, sizeof fact_id, 1, f) != 1 || fread(&ts, sizeof ts, 1, f) != 1 ||
          fread(&vd, sizeof vd, 1, f) != 1 || fread(&tl, sizeof tl, 1, f) != 1) {
         ESP_LOGW(TAG, "short header read — abandoning scan");
         break;
      }
      if (tl >= text_cap) tl = text_cap - 1;
      if (fread(text_buf, 1, tl, f) != tl) {
         ESP_LOGW(TAG, "short text read (wanted %u) — abandoning scan", (unsigned)tl);
         break;
      }
      text_buf[tl] = '\0';
      float *vec = heap_caps_malloc((size_t)vd * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!vec) break;
      if (fread(vec, sizeof(float), vd, f) != vd) {
         ESP_LOGW(TAG, "short vec read (wanted %u floats) — abandoning scan", (unsigned)vd);
         heap_caps_free(vec);
         break;
      }

      if (vd != qdim) {
         /* Embedding model changed underneath us; old facts have a
          * different dimension.  Skip + log once at INFO so it's
          * visible without spamming on every recall. */
         static bool warned = false;
         if (!warned) {
            ESP_LOGW(TAG, "skipping fact_id=%lu — dim %u != current %u (model changed?)", (unsigned long)fact_id,
                     (unsigned)vd, (unsigned)qdim);
            warned = true;
         }
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
   heap_caps_free(text_buf); /* W1-B */

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
      /* W1-B: check every fread; abandon on short read so a corrupt
       * record doesn't get counted. */
      if (fread(&fact_id, sizeof fact_id, 1, f) != 1) break;
      if (fread(&ts, sizeof ts, 1, f) != 1) break;
      if (fread(&vd, sizeof vd, 1, f) != 1) break;
      if (fread(&tl, sizeof tl, 1, f) != 1) break;
      if (fseek(f, (long)tl + (long)vd * (long)sizeof(float), SEEK_CUR) != 0) break;
      n++;
   }
   fclose(f);
   return n;
}
