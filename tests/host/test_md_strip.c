/* W9-A: host-targeted unit tests for main/md_strip.c.
 *
 * Pure-C module — runs on the dev/CI host with no ESP-IDF deps.
 * Plain assert.h driver to avoid a cmocka system dep on minimal CI
 * images.  Returns non-zero on first failure so `ctest --output-on-failure`
 * surfaces the assertion file:line. */

#include "md_strip.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int g_pass = 0;

#define CHECK_EQ_STR(actual, expected)                                                  \
   do {                                                                                 \
      if (strcmp((actual), (expected)) != 0) {                                          \
         fprintf(stderr, "FAIL %s:%d expected=\"%s\" actual=\"%s\"\n", __FILE__,        \
                 __LINE__, (expected), (actual));                                       \
         return 1;                                                                      \
      }                                                                                 \
      g_pass++;                                                                         \
   } while (0)

static int test_inline_bold_pair(void) {
   char out[64];
   md_strip_inline("hello **world**", out, sizeof out);
   CHECK_EQ_STR(out, "hello world");
   return 0;
}

static int test_inline_italic_pair(void) {
   char out[64];
   md_strip_inline("an _emphatic_ word", out, sizeof out);
   CHECK_EQ_STR(out, "an emphatic word");
   return 0;
}

static int test_inline_heading_stripped_at_line_start(void) {
   char out[64];
   md_strip_inline("# Title\nbody", out, sizeof out);
   CHECK_EQ_STR(out, "Title\nbody");
   return 0;
}

static int test_inline_bullet_becomes_unicode(void) {
   char out[64];
   md_strip_inline("- first\n- second", out, sizeof out);
   /* "•" is UTF-8 0xE2 0x80 0xA2 — assert via byte-exact match. */
   CHECK_EQ_STR(out, "\xE2\x80\xA2 first\n\xE2\x80\xA2 second");
   return 0;
}

static int test_inline_null_input_safe(void) {
   char out[16];
   out[0] = 'X';
   md_strip_inline(NULL, out, sizeof out);
   CHECK_EQ_STR(out, "");
   return 0;
}

static int test_inline_truncation_terminates(void) {
   char out[8];
   md_strip_inline("hello world this is long", out, sizeof out);
   /* Must always be NUL-terminated and never overflow. */
   assert(strlen(out) < sizeof out);
   g_pass++;
   return 0;
}

static int test_ellipsis_appended_on_truncation(void) {
   char out[16];
   /* "…" is UTF-8 0xE2 0x80 0xA6 = 3 bytes. */
   md_strip_inline_with_ellipsis("hello world this is too long for the buffer", out,
                                 sizeof out);
   size_t n = strlen(out);
   assert(n > 3);
   /* Last 3 bytes should be the ellipsis. */
   const char *tail = out + n - 3;
   if (memcmp(tail, "\xE2\x80\xA6", 3) != 0) {
      fprintf(stderr, "FAIL %s:%d expected trailing UTF-8 ellipsis, got bytes %02x %02x %02x\n",
              __FILE__, __LINE__, (unsigned char)tail[0], (unsigned char)tail[1],
              (unsigned char)tail[2]);
      return 1;
   }
   g_pass++;
   return 0;
}

static int test_tool_markers_stripped(void) {
   char out[128];
   md_strip_tool_markers("hi <tool>recall</tool><args>{\"q\":1}</args> world", out,
                         sizeof out);
   CHECK_EQ_STR(out, "hi world");
   return 0;
}

static int test_tool_markers_case_insensitive(void) {
   char out[128];
   /* Implementation emits a sentinel space when collapsing tag spans so
    * adjacent prose words don't get glued together; the whitespace pass
    * normalises that.  Pinned here so the behaviour can't regress
    * silently — losing the sentinel would re-glue real chat replies. */
   md_strip_tool_markers("a<TOOL>x</TOOL>b", out, sizeof out);
   CHECK_EQ_STR(out, "a b");
   return 0;
}

static int test_inline_in_place_safe(void) {
   char buf[64] = "**bold** then _it_";
   md_strip_inline(buf, buf, sizeof buf);
   CHECK_EQ_STR(buf, "bold then it");
   return 0;
}

int main(void) {
   struct {
      const char *name;
      int (*fn)(void);
   } tests[] = {
      {"inline_bold_pair", test_inline_bold_pair},
      {"inline_italic_pair", test_inline_italic_pair},
      {"inline_heading_stripped_at_line_start", test_inline_heading_stripped_at_line_start},
      {"inline_bullet_becomes_unicode", test_inline_bullet_becomes_unicode},
      {"inline_null_input_safe", test_inline_null_input_safe},
      {"inline_truncation_terminates", test_inline_truncation_terminates},
      {"ellipsis_appended_on_truncation", test_ellipsis_appended_on_truncation},
      {"tool_markers_stripped", test_tool_markers_stripped},
      {"tool_markers_case_insensitive", test_tool_markers_case_insensitive},
      {"inline_in_place_safe", test_inline_in_place_safe},
   };
   size_t n = sizeof(tests) / sizeof(tests[0]);
   for (size_t i = 0; i < n; i++) {
      int r = tests[i].fn();
      if (r != 0) {
         fprintf(stderr, "test_md_strip: FAILED at %s\n", tests[i].name);
         return r;
      }
      printf("test_md_strip: %s OK\n", tests[i].name);
   }
   printf("test_md_strip: %d assertions, %zu tests passed\n", g_pass, n);
   return 0;
}
