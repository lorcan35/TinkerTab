/*
 * TinkerTab — ui_agents
 *
 * Agent orchestration overlay. Narrative-first view of what Tinker has been
 * doing for you: each agent run shows a line ("I scanned your inbox — three
 * replies were waiting…") plus a task stream with done/wip/queued markers.
 *
 * v5 minimal: the overlay renders a hand-curated demo entry so the UX is
 * navigable and styleable. Real data comes from a future Dragon
 * agent_status protocol addition that aggregates tool_call / tool_result.
 */
#pragma once

#include "lvgl.h"
#include <stdbool.h>

/** Create and show the agents overlay. Idempotent — no-op if already shown. */
void ui_agents_show(void);

/** Hide the overlay. Safe to call when not shown. */
void ui_agents_hide(void);

/** True if the overlay is currently visible. */
bool ui_agents_is_visible(void);
