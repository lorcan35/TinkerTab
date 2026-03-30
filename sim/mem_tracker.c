/*
 * mem_tracker.c — Tracked PSRAM allocator for desktop simulator.
 *
 * Implements the ui_port_malloc / ui_port_free / ui_port_realloc API
 * declared in main/ui_port.h. Enforces the 32MB PSRAM cap that matches
 * the real Tab5 hardware. Warns at 28MB, aborts at 32MB.
 * Logs every individual allocation > 100KB.
 *
 * Stats are queried via ui_port_heap_used() and ui_port_heap_free().
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

/* ── PSRAM limits matching real Tab5 hardware ──────────────────────── */
#define PSRAM_TOTAL_BYTES      (32UL * 1024 * 1024)  /* 32 MB */
#define PSRAM_WARN_BYTES       (28UL * 1024 * 1024)  /* warn at 28 MB */
#define PSRAM_LOG_LARGE_BYTES  (100UL * 1024)         /* log allocs >100 KB */

/* ── Tracker state ─────────────────────────────────────────────────── */
static size_t s_heap_used = 0;
static size_t s_alloc_count = 0;
static size_t s_peak_used = 0;

/* ── Internal helpers ──────────────────────────────────────────────── */

/* We store the allocation size just before the returned pointer.
 * Layout: [size_t size][user bytes...]
 *                      ^ returned pointer
 */
#define HEADER_SIZE (sizeof(size_t))

static size_t *header_of(void *ptr) {
    return (size_t *)((char *)ptr - HEADER_SIZE);
}

/* ── Public API ────────────────────────────────────────────────────── */

void *ui_port_malloc(size_t size, const char *file, int line)
{
    if (size == 0) return NULL;

    if (s_heap_used + size > PSRAM_TOTAL_BYTES) {
        fprintf(stderr, "E [ui_port] PSRAM EXHAUSTED: tried to alloc %zu bytes "
                "(used %zu / %lu) at %s:%d — ABORTING\n",
                size, s_heap_used, PSRAM_TOTAL_BYTES, file, line);
        abort();
    }

    void *raw = malloc(HEADER_SIZE + size);
    if (!raw) {
        fprintf(stderr, "E [ui_port] malloc(%zu) failed at %s:%d\n",
                size, file, line);
        return NULL;
    }

    *(size_t *)raw = size;
    void *ptr = (char *)raw + HEADER_SIZE;

    s_heap_used += size;
    s_alloc_count++;
    if (s_heap_used > s_peak_used) s_peak_used = s_heap_used;

    if (size >= PSRAM_LOG_LARGE_BYTES) {
        printf("I [ui_port] large alloc %zu KB at %s:%d (used %.1f MB / 32 MB)\n",
               size / 1024, file, line,
               (double)s_heap_used / (1024 * 1024));
    }

    if (s_heap_used >= PSRAM_WARN_BYTES) {
        fprintf(stderr, "W [ui_port] PSRAM at %.1f MB — approaching 32 MB limit!\n",
                (double)s_heap_used / (1024 * 1024));
    }

    return ptr;
}

void ui_port_free(void *ptr)
{
    if (!ptr) return;

    size_t *hdr = header_of(ptr);
    size_t size = *hdr;

    if (size > s_heap_used) {
        fprintf(stderr, "E [ui_port] double-free or corruption detected "
                "(size=%zu, used=%zu)\n", size, s_heap_used);
        return;
    }

    s_heap_used -= size;
    free(hdr);
}

void *ui_port_realloc(void *ptr, size_t new_size, const char *file, int line)
{
    if (!ptr) return ui_port_malloc(new_size, file, line);
    if (new_size == 0) { ui_port_free(ptr); return NULL; }

    size_t *hdr = header_of(ptr);
    size_t old_size = *hdr;
    size_t delta = (new_size > old_size) ? (new_size - old_size) : 0;

    if (delta > 0 && s_heap_used + delta > PSRAM_TOTAL_BYTES) {
        fprintf(stderr, "E [ui_port] PSRAM EXHAUSTED on realloc: tried +%zu bytes "
                "(used %zu / %lu) at %s:%d — ABORTING\n",
                delta, s_heap_used, PSRAM_TOTAL_BYTES, file, line);
        abort();
    }

    void *new_raw = realloc(hdr, HEADER_SIZE + new_size);
    if (!new_raw) {
        fprintf(stderr, "E [ui_port] realloc(%zu) failed at %s:%d\n",
                new_size, file, line);
        return NULL;
    }

    *(size_t *)new_raw = new_size;
    void *new_ptr = (char *)new_raw + HEADER_SIZE;

    /* Update accounting */
    if (new_size > old_size) {
        s_heap_used += (new_size - old_size);
    } else {
        s_heap_used -= (old_size - new_size);
    }
    if (s_heap_used > s_peak_used) s_peak_used = s_heap_used;

    return new_ptr;
}

size_t ui_port_heap_used(void)
{
    return s_heap_used;
}

size_t ui_port_heap_free(void)
{
    return (s_heap_used < PSRAM_TOTAL_BYTES) ? (PSRAM_TOTAL_BYTES - s_heap_used) : 0;
}

void ui_port_heap_print(void)
{
    printf("I [ui_port] heap: used=%.1f MB  peak=%.1f MB  allocs=%zu  free=%.1f MB\n",
           (double)s_heap_used / (1024 * 1024),
           (double)s_peak_used / (1024 * 1024),
           s_alloc_count,
           (double)ui_port_heap_free() / (1024 * 1024));
}
