# Mode Picker Rebuild — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the leaky 3-dial mode-sheet with a mode-primary reason-row picker (Layout B): pick a mode directly, see why you'd pick it, set Smartness within it, and tuck plumbing under Advanced.

**Architecture:** `ui_mode_sheet.c` keeps its overlay/sheet/header/Done shell but its body changes from `Intelligence×Voice×Autonomy` dials + preset chips to **6 mode rows** that write `vmode` directly (routing already keys off `vmode`, so the resolver core is untouched). A per-mode metadata table drives the copy; the 3.5 availability check drives dimming. Smartness (Fast/Balanced/Smart) persists to the existing `int_tier` NVS key as a within-mode model selector. An Advanced drawer absorbs the model picker, engine pins, privacy lock, and spend cap (relocated out of Settings).

**Tech Stack:** ESP-IDF 5.5.2, LVGL 9.x, ESP32-P4, C. Spec: `docs/superpowers/specs/2026-05-26-mode-picker-rebuild-design.md`.

**Verification model (firmware — read this):** There is no LVGL unit-test harness. Each task's "test" is the project's real loop: `idf.py build` clean → `git-clang-format --diff origin/main` clean → `idf.py -p /dev/ttyACM0 flash` → on-device checks via the debug server (`TOK=05eed3b13bf62d92cfd8ac424438b9f2`, Tab5 `192.168.1.90:8080`): screenshot, `GET /voice`, `GET /m5`, `GET /events?since=N` (`eng.route`). Screenshots: pace + retry (busy-guard 429s mid-render). A clean reboot clears any stuck screenshot busy-flag (#734).

**Branch:** `feat/mode-picker-rebuild` (off main). Commit after each task with `refs #724`.

---

### Task 1: Mode metadata table + availability helper

Pure data + one helper. No UI change yet — just compiles and is callable.

**Files:**
- Modify: `main/ui_mode_sheet.c` (add near top, after the includes / `static const char *TAG`)

- [ ] **Step 1: Add the metadata struct + table**

Add after the `#include`s (the file already includes `voice_onboard.h`, `settings.h`, `ui_theme.h` from slice 3.5):

```c
/* TT #724 (3.2): per-mode display + availability metadata. Names come from
 * th_mode_names (single source). `requires` drives the 3.5 dim/refuse gate. */
typedef enum { REQ_NONE = 0, REQ_ADDON, REQ_OR_KEY } mode_req_t;
typedef struct {
    const char *reason;  /* one-line why-you'd-pick-it (selected row) */
    const char *leaves;  /* what leaves the device + speed + cost (selected row) */
    const char *oneline; /* compact meta for unselected rows */
    mode_req_t  req;
    bool        fixed_brain; /* true => Smartness has no range (grey it) */
} mode_meta_t;

/* Indexed by vmode (0..5). Order matches th_mode_names / VOICE_MODE_*. */
static const mode_meta_t s_mode_meta[VOICE_MODE_COUNT] = {
    /* 0 Local       */ {"Private brain, on the Dragon box", "Nothing leaves \xc2\xb7 free \xc2\xb7 ~60s",          "Nothing leaves \xc2\xb7 free \xc2\xb7 ~60s",        REQ_NONE,   true},
    /* 1 Hybrid      */ {"Private brain, fast voice",        "leaves: your voice (STT) \xc2\xb7 ~5s \xc2\xb7 ~2\xc2\xa2", "Private brain \xc2\xb7 fast voice \xc2\xb7 ~2\xc2\xa2", REQ_NONE,   true},
    /* 2 Cloud       */ {"Smartest, everything cloud",       "leaves: voice + text \xc2\xb7 ~5s \xc2\xb7 needs key", "Voice+text \xc2\xb7 smartest \xc2\xb7 needs key",   REQ_NONE,   false},
    /* 3 TinkerAgent */ {"Tools + memory, agentic",          "leaves: voice + text + tools \xc2\xb7 via gateway",   "Tools + memory \xc2\xb7 agentic",                REQ_NONE,   true},
    /* 4 TinkerON    */ {"Works offline, on-device addon",   "Nothing leaves \xc2\xb7 works offline",               "Nothing leaves \xc2\xb7 works offline",          REQ_ADDON,  true},
    /* 5 Solo        */ {"Cloud quality, no Dragon needed",  "leaves: voice + text \xc2\xb7 direct to OpenRouter",  "No Dragon \xc2\xb7 voice+text leave",            REQ_OR_KEY, false},
};
```

- [ ] **Step 2: Add the availability helper**

Add below the table (reuses the exact 3.5 checks):

```c
/* TT #724 (3.5/3.2): can this mode run right now? */
static bool mode_is_available(uint8_t vmode)
{
    if (vmode >= VOICE_MODE_COUNT) return false;
    switch (s_mode_meta[vmode].req) {
        case REQ_ADDON:  return voice_onboard_failover_state() == 2 /* M5_FAIL_READY */;
        case REQ_OR_KEY: {
            char k[128] = {0};
            tab5_settings_get_or_key(k, sizeof k);
            return k[0] != '\0';
        }
        default: return true;
    }
}
```

- [ ] **Step 3: Build**

Run: `. /home/rebelforce/esp/esp-idf/export.sh && idf.py build 2>&1 | grep -E "Project build complete|error:"`
Expected: `Project build complete` (the table/helper are unused for now — `-Wno-error=unused-function` allows it; if `unused-variable` on the table errors, add `__attribute__((unused))` to `s_mode_meta`).

- [ ] **Step 4: Format check**

Run: `git-clang-format --binary clang-format-18 --diff origin/main main/ui_mode_sheet.c`
Expected: empty / "did not modify". If not: `git add main/ui_mode_sheet.c && git-clang-format --binary clang-format-18 origin/main`.

- [ ] **Step 5: Commit**

```bash
git add main/ui_mode_sheet.c
git commit -m "feat(ui): mode metadata table + availability helper (refs #724)"
```

---

### Task 2: Reason-row picker body (replaces dials + preset chips)

Replace the dial/preset rendering with 6 mode rows; selected row expands; tap commits `vmode` directly. This is the core of 3.2 + 3.3.

**Files:**
- Modify: `main/ui_mode_sheet.c` — the body-render section of `ui_mode_sheet_show()` (the `INTELLIGENCE/VOICE/AUTONOMY` dial blocks + the `PRESETS` chip loop, roughly the run between the headline and the composite card). Replace those with the row list. Keep: overlay/scrim, sheet container, grip, kicker+headline ("How should she think?"), Done button, and the composite "RESOLVES TO" card MAY be removed (the selected row now shows the same info) — remove it in this task.

- [ ] **Step 1: Add the commit + row-tap callbacks**

Add above `ui_mode_sheet_show()` (near `preset_click_cb`):

```c
static uint8_t s_sel_vmode = 0;          /* row currently shown expanded */
static void rebuild_rows(void);          /* fwd: re-render rows on selection */

/* Write the chosen mode + notify Dragon. Same calls the old preset-4/5 path
 * made — proven. Does NOT touch tab5_mode_resolve / the dial tiers. */
static void commit_mode(uint8_t vmode)
{
    tab5_settings_set_voice_mode(vmode);
    char model[64] = {0};
    tab5_settings_get_llm_model(model, sizeof(model));
    voice_send_config_update((int)vmode, model);
    ui_audio_cue_play(UI_CUE_MODE_SWITCH);
    extern void ui_agents_on_mode_change(void);
    ui_agents_on_mode_change();
    ESP_LOGI(TAG, "picker: mode -> %u (%s)", vmode, th_mode_names[vmode]);
}

static void row_click_cb(lv_event_t *e)
{
    uint8_t vmode = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if (vmode >= VOICE_MODE_COUNT) return;
    if (!mode_is_available(vmode)) {
        if (tab5_ui_try_lock(150)) {
            ui_home_show_toast(s_mode_meta[vmode].req == REQ_ADDON
                                   ? "TinkerON not ready \xe2\x80\x94 check the module"
                                   : "Add an OpenRouter key in Settings first");
            tab5_ui_unlock();
        }
        return;
    }
    s_sel_vmode = vmode;
    rebuild_rows();      /* expand the newly-selected row */
    commit_mode(vmode);
}
```

- [ ] **Step 2: Add the row-list builder**

Add the `rebuild_rows()` definition. It owns a container `s_rows_root` (file-scope `static lv_obj_t *s_rows_root = NULL;` — add with the other statics). It clears + rebuilds 6 rows:

```c
static lv_obj_t *s_rows_root = NULL;

static void rebuild_rows(void)
{
    if (!s_rows_root) return;
    lv_obj_clean(s_rows_root);
    int y = 0;
    for (uint8_t m = 0; m < VOICE_MODE_COUNT; m++) {
        bool avail = mode_is_available(m);
        bool sel   = (m == s_sel_vmode);
        int  rh    = sel ? 74 : 46;

        lv_obj_t *row = lv_obj_create(s_rows_root);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, SIDE_PAD, y);
        lv_obj_set_size(row, MS_W - 2 * SIDE_PAD, rh);
        lv_obj_set_style_bg_color(row, lv_color_hex(sel ? 0x1A1509 : 0x13131C), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(sel ? TH_AMBER : 0x20202C), 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)m);
        if (!avail) lv_obj_set_style_opa(row, LV_OPA_40, 0);

        /* mode dot (shared widget, colored by mode) */
        lv_obj_t *dot = widget_mode_dot_create(row, 8, m);
        if (dot) lv_obj_set_pos(dot, 12, sel ? 14 : 18);

        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, th_mode_names[m]);
        lv_obj_set_style_text_font(nm, FONT_BODY, 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(TH_TEXT_PRIMARY), 0);
        lv_obj_set_pos(nm, 30, sel ? 10 : 13);

        if (m == 1) { /* Hybrid "recommended" tag */
            lv_obj_t *rec = lv_label_create(row);
            lv_label_set_text(rec, "RECOMMENDED");
            lv_obj_set_style_text_font(rec, FONT_TINY, 0);
            lv_obj_set_style_text_color(rec, lv_color_hex(0x7A7A88), 0);
            lv_obj_align(rec, LV_ALIGN_TOP_RIGHT, -14, sel ? 14 : 16);
        }
        if (m == 4 && !avail) { /* TinkerON addon hint */
            lv_obj_t *w = lv_label_create(row);
            lv_label_set_text(w, "\xe2\x9a\xa0 attach addon");
            lv_obj_set_style_text_font(w, FONT_TINY, 0);
            lv_obj_set_style_text_color(w, lv_color_hex(TH_AMBER), 0);
            lv_obj_align(w, LV_ALIGN_TOP_RIGHT, -14, sel ? 14 : 16);
        }

        if (sel) {
            lv_obj_t *reason = lv_label_create(row);
            lv_label_set_text(reason, s_mode_meta[m].reason);
            lv_obj_set_style_text_font(reason, FONT_SMALL, 0);
            lv_obj_set_style_text_color(reason, lv_color_hex(0xC2C2CC), 0);
            lv_obj_set_pos(reason, 30, 34);
            lv_obj_t *meta = lv_label_create(row);
            lv_label_set_text(meta, s_mode_meta[m].leaves);
            lv_obj_set_style_text_font(meta, FONT_TINY, 0);
            lv_obj_set_style_text_color(meta, lv_color_hex(0x6A6A78), 0);
            lv_obj_set_pos(meta, 30, 53);
        } else {
            lv_obj_t *one = lv_label_create(row);
            lv_label_set_text(one, s_mode_meta[m].oneline);
            lv_obj_set_style_text_font(one, FONT_TINY, 0);
            lv_obj_set_style_text_color(one, lv_color_hex(0x6A6A78), 0);
            lv_obj_align(one, LV_ALIGN_RIGHT_MID, -14, 0);
        }
        y += rh + 8;
    }
}
```

- [ ] **Step 2b: In `ui_mode_sheet_show()`, set `s_sel_vmode` + create `s_rows_root` + call `rebuild_rows()`**

After the headline/kicker block, where the dial blocks used to start, insert:

```c
    s_sel_vmode = tab5_settings_get_voice_mode();
    if (s_sel_vmode >= VOICE_MODE_COUNT) s_sel_vmode = 0;
    s_rows_root = lv_obj_create(s_sheet);
    lv_obj_remove_style_all(s_rows_root);
    lv_obj_set_pos(s_rows_root, 0, 150);             /* below SMARTNESS (Task 3) */
    lv_obj_set_size(s_rows_root, MS_W, 470);
    lv_obj_clear_flag(s_rows_root, LV_OBJ_FLAG_SCROLLABLE);
    rebuild_rows();
```

- [ ] **Step 2c: Delete the dial blocks + preset loop + composite card**

Remove from `ui_mode_sheet_show()`: the three `INTELLIGENCE/VOICE/AUTONOMY` segment-render blocks, the `PRESETS` chip loop (the `for (c = 0; c < chip_count...)` added in 3.0/3.5), and the composite "RESOLVES TO" card creation + its `refresh_composite()` call. Also delete now-dead helpers: `refresh_segments`, `refresh_composite`, `seg_click_cb`, `preset_click_cb`, the agent-consent modal (`show_agent_consent`/`hide_agent_consent`/`consent_*_cb`) IF the new picker drops the in-sheet agent consent — **KEEP** the agent-consent flow: instead, call it from `commit_mode` when `vmode==3` and `aut_tier` was 0 (preserves the memory-bypass warning). Simplest: in `row_click_cb`, before `commit_mode(3)`, if `tab5_settings_get_aut_tier()==0` call `show_agent_consent`-equivalent. (If this balloons, file a follow-up to restore consent and land the picker first — note in commit.)

- [ ] **Step 3: Build + format** (as Task 1 steps 3-4).

- [ ] **Step 4: Flash + verify on device**

```bash
. /home/rebelforce/esp/esp-idf/export.sh && idf.py -p /dev/ttyACM0 flash
# wait for boot, then:
TOK=05eed3b13bf62d92cfd8ac424438b9f2; H="Authorization: Bearer $TOK"; B=192.168.1.90:8080
curl -s -m6 -H "$H" -X POST "http://$B/navigate?screen=home"; sleep 1
curl -s -m6 -H "$H" -X POST "http://$B/touch" -d '{"x":360,"y":340,"action":"long_press","duration_ms":900}'; sleep 2
# screenshot (retry on 429):
for t in 1 2 3 4; do c=$(curl -s -m8 -H "$H" -o /tmp/t2.jpg -w "%{http_code}" "http://$B/screenshot.jpg"); [ "$c" = 200 ] && break; sleep 0.8; done
```
Expected (read /tmp/t2.jpg): 6 mode rows, the current mode expanded (reason + leaves line), TinkerON dimmed if cold. Tap an available row at its y → `GET /voice` reflects the new mode; tap TinkerON when cold → toast, no change.

- [ ] **Step 5: Commit**

```bash
git add main/ui_mode_sheet.c
git commit -m "feat(ui): reason-row mode picker — mode-primary, sets vmode directly (refs #724)"
```

---

### Task 3: Smartness segment

Fast/Balanced/Smart row above the mode list, persisted to `int_tier`; greyed for fixed-brain modes.

**Files:**
- Modify: `main/ui_mode_sheet.c`

- [ ] **Step 1: Add the segment + callback**

Add a `static lv_obj_t *s_smart_seg[3] = {0};` with the other statics, and:

**VALIDATED CORRECTION:** `int_tier` is read by *nothing* in the routing path
(only the soon-retired `tab5_mode_resolve`), and `config_update` only forwards
`llm_model` for Cloud. So setting `int_tier` alone is **cosmetic**. Smartness must
set the **actual model** for the only two modes where Tab5 picks it — **Cloud**
(`llm_model`) and **Solo** (`or_mdl_llm`) — and send `config_update`. Local/Hybrid
(Dragon picks), TinkerON, TinkerAgent are fixed-brain (greyed). Default tier→model
map (overridable via Advanced exact-pick), models taken from the existing
`s_cloud_models[]`:

```c
/* Fast/Balanced/Smart -> a concrete model. Setting int_tier alone changes
 * nothing (routing keys off vmode + llm_model, never int_tier — validated). */
static const char *const s_smart_cloud[3] = {
    "~anthropic/claude-haiku-latest",  /* Fast     */
    "anthropic/claude-sonnet-4.6",     /* Balanced */
    "anthropic/claude-opus-4.7",       /* Smart    */
};
static void smart_click_cb(lv_event_t *e)
{
    uint8_t tier = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if (tier > 2 || s_mode_meta[s_sel_vmode].fixed_brain) return;
    tab5_settings_set_int_tier(tier); /* persisted for the segment's on-state */
    const char *model = s_smart_cloud[tier];
    if (s_sel_vmode == VOICE_MODE_SOLO) tab5_settings_set_or_mdl_llm(model);
    else                                tab5_settings_set_llm_model(model); /* Cloud */
    voice_send_config_update((int)s_sel_vmode, (char *)model);
    for (int i = 0; i < 3; i++) {
        if (s_smart_seg[i]) lv_obj_set_style_bg_color(
            s_smart_seg[i], lv_color_hex(i == tier ? TH_AMBER : 0x15151F), 0);
    }
    ESP_LOGI(TAG, "smartness tier=%u -> model=%s (vmode=%u)", tier, model, s_sel_vmode);
}

static void build_smartness(void)   /* called from ui_mode_sheet_show after the headline */
{
    lv_obj_t *lab = lv_label_create(s_sheet);
    lv_label_set_text(lab, "SMARTNESS");
    lv_obj_set_style_text_font(lab, FONT_TINY, 0);
    lv_obj_set_style_text_color(lab, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_text_letter_space(lab, 2, 0);
    lv_obj_set_pos(lab, SIDE_PAD, 108);

    bool fixed = s_mode_meta[s_sel_vmode].fixed_brain;
    uint8_t tier = tab5_settings_get_int_tier();
    const char *names[3] = {"Fast", "Balanced", "Smart"};
    int seg_w = (MS_W - 2 * SIDE_PAD - 2 * 6) / 3;
    for (int i = 0; i < 3; i++) {
        lv_obj_t *s = lv_obj_create(s_sheet);
        lv_obj_remove_style_all(s);
        lv_obj_set_pos(s, SIDE_PAD + i * (seg_w + 6), 126);
        lv_obj_set_size(s, seg_w, 30);
        lv_obj_set_style_radius(s, 10, 0);
        lv_obj_set_style_bg_color(s, lv_color_hex((i == tier && !fixed) ? TH_AMBER : 0x15151F), 0);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
        lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
        if (!fixed) {
            lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(s, smart_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        } else {
            lv_obj_set_style_opa(s, LV_OPA_40, 0);
        }
        lv_obj_t *t = lv_label_create(s);
        lv_label_set_text(t, names[i]);
        lv_obj_set_style_text_font(t, FONT_SMALL, 0);
        lv_obj_center(t);
        s_smart_seg[i] = s;
    }
}
```

- [ ] **Step 2: Call `build_smartness()` in `ui_mode_sheet_show()`** right before the `s_rows_root` block from Task 2. Reset `s_smart_seg[*]=NULL` in `ui_mode_sheet_hide()` alongside the other statics.

- [ ] **Step 3: When a row is selected, refresh smartness enabled/disabled** — in `row_click_cb`, after `rebuild_rows()`, also rebuild smartness (a fixed-brain mode greys it). Simplest: clear + rebuild via a `refresh_smartness()` that re-runs the per-segment styling using `s_mode_meta[s_sel_vmode].fixed_brain`. (Keep `build_smartness` idempotent by deleting prior segs first, or guard.)

- [ ] **Step 4: Build + format + flash + verify** — open picker: SMARTNESS segment shows; select TinkerON → segment greys; select Cloud → segment active, tapping Smart persists (`GET /settings` → `int_tier`). Commit `feat(ui): Smartness knob (within-mode model tier) (refs #724)`.

---

### Task 4: Advanced drawer

Collapsed "Advanced ▸" row → expands to model pick + engine pins + privacy lock + spend cap, relocated from Settings.

**Files:**
- Modify: `main/ui_mode_sheet.c` (drawer UI + handlers)
- Reference (source the controls): `main/ui_settings.c` ENGINES block (`mk_section "ENGINES"` near `:421` + `cb_engine_pick` at `:948`), CLOUD LLM chips (tracked from `:1858`), privacy `On-device lock` + `cap_mils`.

- [ ] **Step 1:** Add a collapsed drawer row below `s_rows_root` (a 1-line "Advanced ▸ model · engine · privacy · cap" button). On tap, toggle `s_adv_open` and rebuild the drawer contents into a `s_adv_root` container (created in `ui_mode_sheet_show`, sized 0 when collapsed).

- [ ] **Step 2: Engine pins** — port the AUTO/K144/OPENROUTER segmented from `ui_settings.c` (`cb_engine_pick`, `tab5_settings_get/set_llm_engine`). Reuse the same setter; render 3 chips in the drawer. Code mirrors the Task-3 segment pattern but calls `tab5_settings_set_llm_engine(idx)`.

- [ ] **Step 3: Exact-model pick** — port the CLOUD LLM horizontal chip row from `ui_settings.c` (the model chips that call the `/mode?model=` / `tab5_settings_set_llm_model` path). Render as a horizontally-scrollable chip row in the drawer.

- [ ] **Step 4: Privacy lock + spend cap** — a toggle (`tab5_settings_get/set_*` for on-device-lock) and a read/edit of `cap_mils`. Toggle reuses the Settings privacy setter.

- [ ] **Step 5:** When any Advanced control is touched, set a flag so the home/chat chip can show "Mode · modified" (expose via a `bool ui_mode_sheet_is_modified(void)` or reuse engine-pin != AUTO as the "modified" signal). Minimal: treat `llm_engine != AUTO || on-device-lock || custom model` as modified; surface in `chat_header_set_mode` / `ui_home update_mode_ui` as a " · modified" suffix.

- [ ] **Step 6: Build + format + flash + verify** — Advanced ▸ expands; engine pin persists (`GET /settings` llm_engine); model chip persists; privacy toggle persists. Commit `feat(ui): Advanced drawer (model/engine/privacy/cap) (refs #724)`.

---

### Task 5: Settings — collapse the duplicate VOICE-MODE picker

The picker is now the single mode authority; Settings should not host a second one.

**Files:**
- Modify: `main/ui_settings.c` — VOICE MODE block (`:1862`+) and the ENGINES (`:421`/section) + CLOUD LLM blocks now relocated to the drawer.

- [ ] **Step 1:** Replace the VOICE MODE radio rows (the `for (i<5)` loop that renders `th_mode_names[i]` rows) with a single **read-only "current mode" row**: shows `th_mode_names[get_voice_mode()]` + its tagline; on tap → `ui_mode_sheet_show()`.
- [ ] **Step 2:** Remove the ENGINES section + CLOUD LLM chip block from Settings (now in the Advanced drawer). Keep STT/TTS *status* labels if desired (read-only).
- [ ] **Step 3: Build + format + flash + verify** — Settings shows one "current mode → opens picker" row; no duplicate radio; no ENGINES/CLOUD-LLM blocks. Tapping the row opens the picker. Commit `refactor(settings): replace VOICE-MODE radio with read-only row → opens picker (refs #724)`.

---

### Task 6: Cleanup + routing verification

Retire the dial machinery from the picker path and prove routing is unaffected.

**Files:**
- Modify: `main/ui_mode_sheet.c` (remove the tier-resync block in `ui_mode_sheet_show` lines ~107-143 that reverse-derives tiers; the picker no longer uses dials). Leave `tab5_mode_resolve` (settings.c) + the debug `/mode`-from-tiers path untouched.

- [ ] **Step 1:** Delete the dial-tier resync block + any remaining `tab5_mode_resolve` calls inside `ui_mode_sheet.c`. Confirm `s_int_tier/s_voi_tier/s_aut_tier` statics are only read by Smartness now (int_tier) — remove voi/aut if dead.
- [ ] **Step 2: Build + format.**
- [ ] **Step 3: Flash + routing verification** — for each mode, select it in the picker then send a turn and confirm it routes to the expected backend:

```bash
TOK=05eed3b13bf62d92cfd8ac424438b9f2; H="Authorization: Bearer $TOK"; B=192.168.1.90:8080
for m in 0 1 2 4 5; do
  curl -s -H "$H" -X POST "http://$B/mode?m=$m" >/dev/null; sleep 1
  curl -s -H "$H" -X POST "http://$B/chat" -d '{"text":"hi"}' >/dev/null
  sleep 3; curl -s -H "$H" "http://$B/events?since=0" | grep -o 'eng.route[^}]*' | tail -1
done
```
Expected: each mode's `eng.route` matches (priv/k144/solo/local). No regression vs pre-change.

- [ ] **Step 4: `story_smoke` regression**

Run: `cd ~/projects/TinkerTab && TAB5_TOKEN=$TOK python3 tests/e2e/runner.py story_smoke`
Expected: nav/camera/settings/chat-send pass (the Local-LLM-done timeout is the known flake).

- [ ] **Step 5: Commit** `refactor(ui): retire dial machinery from picker; routing verified (refs #724)`.

---

## Self-Review

- **Spec coverage:** 3.2 picker → Tasks 2/3; mode-primary (3.3) → Task 2 (commit_mode sets vmode) + Task 6 (retire dials); 3.4 Advanced → Task 4; Settings dedup → Task 5; capability gating → Task 1/2 (reused). 3.6 effective-route → out of scope (spec-flagged). ✅
- **Placeholder scan:** Task 4 steps 2-4 describe *relocating* existing Settings controls with exact file:line sources rather than re-pasting their full code — this is a "modify, sourcing from file:lines" instruction, not a placeholder; the implementer reads the cited Settings code. Task 2c flags the agent-consent decision explicitly with a fallback. No "TBD/handle edge cases" left.
- **Type consistency:** `mode_meta_t`/`s_mode_meta`/`mode_is_available`/`commit_mode`/`rebuild_rows`/`s_sel_vmode`/`s_rows_root`/`s_smart_seg` are defined in Task 1-3 and used consistently. `th_mode_names`, `voice_onboard_failover_state`, `tab5_settings_get_or_key`, `ui_audio_cue_play`, `tab5_ui_try_lock` all already in scope (slice 3.5 includes).

## Risk notes for the executor

- **Agent-consent (vmode 3):** the current sheet shows a memory-bypass consent modal before committing TinkerAgent. Task 2c preserves it via `commit_mode`; if wiring it cleanly balloons, land the picker without it and file a follow-up to restore — do NOT silently drop the consent.
- **Screenshot busy-flag (#734):** if screenshots 429 forever, reboot to clear; don't fight it.
- **No `LV_USE_ASSERT`:** allocs return NULL silently. Keep the picker lightweight; it builds on `lv_layer_top` while home is underneath.
