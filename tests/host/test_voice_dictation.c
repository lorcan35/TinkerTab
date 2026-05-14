/* Host-targeted unit tests for main/voice_dictation.c.
 *
 * Pure-C state machine — zero ESP-IDF deps in the module under test;
 * this file uses plain assert.h / stdio just like test_md_strip.c. */

#include "voice_dictation.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int g_pass = 0;

#define CHECK(cond)                                                       \
   do {                                                                   \
      if (!(cond)) {                                                      \
         fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
         return 1;                                                        \
      }                                                                   \
      g_pass++;                                                           \
   } while (0)

#define CHECK_EQ(a, b)                                                              \
   do {                                                                             \
      if ((a) != (b)) {                                                             \
         fprintf(stderr, "FAIL %s:%d  %s != %s  (%d vs %d)\n", __FILE__, __LINE__,  \
                 #a, #b, (int)(a), (int)(b));                                       \
         return 1;                                                                  \
      }                                                                             \
      g_pass++;                                                                     \
   } while (0)

static int test_init_state_is_idle(void) {
   voice_dictation_init();
   dict_event_t e = voice_dictation_get();
   CHECK_EQ(e.state, DICT_IDLE);
   CHECK_EQ(e.fail_reason, DICT_FAIL_NONE);
   CHECK_EQ(e.note_slot, -1);
   return 0;
}

int main(void) {
   if (test_init_state_is_idle()) return 1;
   fprintf(stderr, "ok  %d checks passed\n", g_pass);
   return 0;
}
