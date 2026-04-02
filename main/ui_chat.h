#pragma once
#include "lvgl.h"
lv_obj_t *ui_chat_create(void);
void ui_chat_destroy(void);
bool ui_chat_is_active(void);
void ui_chat_add_message(const char *text, bool is_user);
