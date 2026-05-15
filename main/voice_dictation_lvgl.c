#include "voice_dictation_lvgl.h"

#include <stdlib.h>

#include "ui_core.h" /* tab5_lv_async_call */

typedef struct {
   dict_subscriber_t cb;
   void *user_data;
   dict_event_t event;
} dict_marshal_t;

static void dict_marshal_lvgl_cb(void *arg) {
   dict_marshal_t *m = (dict_marshal_t *)arg;
   m->cb(&m->event, m->user_data);
   free(m);
}

static void dict_marshal_dispatch(const dict_event_t *e, void *user_data) {
   dict_subscriber_t real_cb = ((void **)user_data)[0];
   void *real_user_data = ((void **)user_data)[1];
   dict_marshal_t *m = malloc(sizeof(*m));
   if (!m) return;
   m->cb = real_cb;
   m->user_data = real_user_data;
   m->event = *e;
   tab5_lv_async_call(dict_marshal_lvgl_cb, m);
}

int voice_dictation_subscribe_lvgl(dict_subscriber_t cb, void *user_data) {
   if (!cb) return -1;
   void **tuple = malloc(sizeof(void *) * 2);
   if (!tuple) return -1;
   tuple[0] = (void *)cb;
   tuple[1] = user_data;
   int h = voice_dictation_subscribe(dict_marshal_dispatch, tuple);
   if (h < 0) {
      free(tuple);
      return -1;
   }
   return h;
}
