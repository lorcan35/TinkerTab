/*
 * TinkerTab — ui_memory
 *
 * Memory search overlay. Typed or spoken query → hit list with the relevant
 * excerpt from past conversations / notes. v5 minimal: static demo results
 * so the surface is navigable; real search binds to Dragon's Qwen3-embedding
 * endpoint in a later commit.
 */
#pragma once

#include "lvgl.h"
#include <stdbool.h>

void ui_memory_show(void);
void ui_memory_hide(void);
bool ui_memory_is_visible(void);
