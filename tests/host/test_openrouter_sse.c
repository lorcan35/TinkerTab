/* W9-A slice 2: host-targeted tests for openrouter_sse line-buffered
 * SSE parser.  Compiled with host shims for esp_heap_caps / esp_log /
 * esp_err so the module's actual code (main/openrouter_sse.c) runs
 * verbatim — no behaviour deviation from the firmware binary.
 *
 * The bugs the regression net is sized to catch:
 *   - W3-C (TT #374): after a >64 KB SSE line overflows, the OLD code
 *     reset line_len=0 and started accumulating mid-line content as a
 *     fresh "new line", emitting garbage JSON deltas to chat.  The fix
 *     sets a `dropping` flag instead, so we MUST observe zero callbacks
 *     between overflow and the next \n.
 *   - The 4 KB → 64 KB bump for multimodal audio events.  We verify the
 *     parser can ingest a ~50 KB single line successfully.
 *   - `data: [DONE]` sentinel must flip the cumulative `done` flag.
 *   - CRLF lines must trim the trailing \r before comparison.
 *   - Comment lines (`:` prefix) per SSE spec must be silently dropped.
 *   - Multi-event single feed must dispatch each event in order. */

#include "openrouter_sse.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
   int n;
   char captured[8][512];
} captures_t;

static void cb_capture(const char *json, size_t len, void *ctx) {
   captures_t *c = (captures_t *)ctx;
   if (c->n >= 8) return;
   size_t copy = len < sizeof(c->captured[0]) - 1 ? len : sizeof(c->captured[0]) - 1;
   memcpy(c->captured[c->n], json, copy);
   c->captured[c->n][copy] = 0;
   c->n++;
}

static int g_pass = 0;
#define EXPECT_EQ_STR(actual, expected)                                                       \
   do {                                                                                       \
      if (strcmp((actual), (expected)) != 0) {                                                \
         fprintf(stderr, "FAIL %s:%d expected=\"%s\" actual=\"%s\"\n", __FILE__, __LINE__,    \
                 (expected), (actual));                                                       \
         return 1;                                                                            \
      }                                                                                       \
      g_pass++;                                                                               \
   } while (0)
#define EXPECT_INT_EQ(actual, expected)                                                       \
   do {                                                                                       \
      if ((actual) != (expected)) {                                                           \
         fprintf(stderr, "FAIL %s:%d expected=%d actual=%d\n", __FILE__, __LINE__,            \
                 (int)(expected), (int)(actual));                                             \
         return 1;                                                                            \
      }                                                                                       \
      g_pass++;                                                                               \
   } while (0)

static int test_single_event(void) {
   captures_t c = {0};
   openrouter_sse_state_t *s = NULL;
   assert(openrouter_sse_init(&s, cb_capture, &c) == ESP_OK);
   const char *buf = "data: {\"hello\":1}\n\n";
   bool done = openrouter_sse_feed(s, buf, strlen(buf));
   EXPECT_INT_EQ(done, 0);
   EXPECT_INT_EQ(c.n, 1);
   EXPECT_EQ_STR(c.captured[0], "{\"hello\":1}");
   openrouter_sse_free(s);
   return 0;
}

static int test_multi_event_single_feed(void) {
   captures_t c = {0};
   openrouter_sse_state_t *s = NULL;
   openrouter_sse_init(&s, cb_capture, &c);
   const char *buf = "data: {\"a\":1}\n\ndata: {\"b\":2}\n\ndata: {\"c\":3}\n\n";
   openrouter_sse_feed(s, buf, strlen(buf));
   EXPECT_INT_EQ(c.n, 3);
   EXPECT_EQ_STR(c.captured[0], "{\"a\":1}");
   EXPECT_EQ_STR(c.captured[1], "{\"b\":2}");
   EXPECT_EQ_STR(c.captured[2], "{\"c\":3}");
   openrouter_sse_free(s);
   return 0;
}

static int test_split_across_feeds(void) {
   captures_t c = {0};
   openrouter_sse_state_t *s = NULL;
   openrouter_sse_init(&s, cb_capture, &c);
   openrouter_sse_feed(s, "data: {\"hel", 11);
   openrouter_sse_feed(s, "lo\":1}\n\n", 8);
   EXPECT_INT_EQ(c.n, 1);
   EXPECT_EQ_STR(c.captured[0], "{\"hello\":1}");
   openrouter_sse_free(s);
   return 0;
}

static int test_done_sentinel(void) {
   captures_t c = {0};
   openrouter_sse_state_t *s = NULL;
   openrouter_sse_init(&s, cb_capture, &c);
   bool done = openrouter_sse_feed(s, "data: [DONE]\n\n", 14);
   EXPECT_INT_EQ(done, 1);
   EXPECT_INT_EQ(c.n, 0); /* [DONE] is internal — no callback */
   openrouter_sse_free(s);
   return 0;
}

static int test_crlf_lines(void) {
   captures_t c = {0};
   openrouter_sse_state_t *s = NULL;
   openrouter_sse_init(&s, cb_capture, &c);
   const char *buf = "data: {\"x\":7}\r\n\r\n";
   openrouter_sse_feed(s, buf, strlen(buf));
   EXPECT_INT_EQ(c.n, 1);
   EXPECT_EQ_STR(c.captured[0], "{\"x\":7}");
   openrouter_sse_free(s);
   return 0;
}

static int test_comment_lines_dropped(void) {
   captures_t c = {0};
   openrouter_sse_state_t *s = NULL;
   openrouter_sse_init(&s, cb_capture, &c);
   const char *buf = ": keep-alive\n\ndata: {\"y\":3}\n\n";
   openrouter_sse_feed(s, buf, strlen(buf));
   EXPECT_INT_EQ(c.n, 1);
   EXPECT_EQ_STR(c.captured[0], "{\"y\":3}");
   openrouter_sse_free(s);
   return 0;
}

static int test_overflow_drops_then_resumes(void) {
   /* Build a line larger than the 64 KB buffer.  After the overflow,
    * the parser must NOT emit a callback for the partial — and must
    * correctly emit the NEXT well-formed event after the offending \n. */
   captures_t c = {0};
   openrouter_sse_state_t *s = NULL;
   openrouter_sse_init(&s, cb_capture, &c);
   /* 70 KB of 'A' then \n\n — overflows the 64 KB line buf. */
   size_t bigsz = 70 * 1024;
   char *big = malloc(bigsz + 64);
   memcpy(big, "data: ", 6);
   memset(big + 6, 'A', bigsz);
   memcpy(big + 6 + bigsz, "\n\ndata: {\"after\":1}\n\n", 21);
   openrouter_sse_feed(s, big, 6 + bigsz + 21);
   free(big);
   /* W3-C regression net: must observe exactly 1 callback (the second
    * well-formed event), NOT a garbage partial from the overflowed line. */
   EXPECT_INT_EQ(c.n, 1);
   EXPECT_EQ_STR(c.captured[0], "{\"after\":1}");
   openrouter_sse_free(s);
   return 0;
}

static int test_large_50kb_line_fits(void) {
   /* 50 KB single line — within the 64 KB cap, must be delivered intact. */
   captures_t c = {0};
   openrouter_sse_state_t *s = NULL;
   openrouter_sse_init(&s, cb_capture, &c);
   size_t bigsz = 50 * 1024;
   char *big = malloc(bigsz + 64);
   memcpy(big, "data: \"", 7);
   memset(big + 7, 'B', bigsz);
   memcpy(big + 7 + bigsz, "\"\n\n", 3);
   openrouter_sse_feed(s, big, 7 + bigsz + 3);
   free(big);
   EXPECT_INT_EQ(c.n, 1);
   /* Verify byte at offset 1 is 'B' (the opening quote is index 0). */
   if (c.captured[0][1] != 'B') {
      fprintf(stderr, "FAIL large line truncated, got first body byte 0x%02x\n",
              (unsigned char)c.captured[0][1]);
      return 1;
   }
   g_pass++;
   openrouter_sse_free(s);
   return 0;
}

static int test_optional_space_after_colon(void) {
   captures_t c = {0};
   openrouter_sse_state_t *s = NULL;
   openrouter_sse_init(&s, cb_capture, &c);
   /* SSE spec allows `data:VALUE` (no space) — must be supported. */
   openrouter_sse_feed(s, "data:{\"z\":9}\n\n", 14);
   EXPECT_INT_EQ(c.n, 1);
   EXPECT_EQ_STR(c.captured[0], "{\"z\":9}");
   openrouter_sse_free(s);
   return 0;
}

int main(void) {
   struct {
      const char *name;
      int (*fn)(void);
   } tests[] = {
      {"single_event", test_single_event},
      {"multi_event_single_feed", test_multi_event_single_feed},
      {"split_across_feeds", test_split_across_feeds},
      {"done_sentinel", test_done_sentinel},
      {"crlf_lines", test_crlf_lines},
      {"comment_lines_dropped", test_comment_lines_dropped},
      {"overflow_drops_then_resumes", test_overflow_drops_then_resumes},
      {"large_50kb_line_fits", test_large_50kb_line_fits},
      {"optional_space_after_colon", test_optional_space_after_colon},
   };
   size_t n = sizeof(tests) / sizeof(tests[0]);
   for (size_t i = 0; i < n; i++) {
      int r = tests[i].fn();
      if (r != 0) {
         fprintf(stderr, "test_openrouter_sse: FAILED at %s\n", tests[i].name);
         return r;
      }
      printf("test_openrouter_sse: %s OK\n", tests[i].name);
   }
   printf("test_openrouter_sse: %d assertions, %zu tests passed\n", g_pass, n);
   return 0;
}
