/**
 * TinkerTab Design System — Shared colors, spacing, and styles.
 *
 * Every screen imports this header for visual consistency.
 * Styles are initialized once via ui_theme_init() at boot.
 * All sizes use DPI_SCALE for portability.
 */
#pragma once

#include "lvgl.h"
#include "config.h"  /* DPI_SCALE, TOUCH_MIN, FONT_* */

/* ── Colors ────────────────────────────────────────────────── */
#define TH_BG            0x08080E   /* Background (subtle blue tint) */
#define TH_CARD          0x111119   /* Card surface */
#define TH_CARD_ELEVATED 0x13131F   /* Elevated card (agent, modal) */
#define TH_CARD_BORDER   0x0B0B12   /* Card border */

#define TH_TEXT_PRIMARY   0xE8E8EF  /* Titles, headings */
#define TH_TEXT_BODY      0xAAAAAA  /* Body text */
/* TT #328 Wave 1 a11y lift: TH_TEXT_DIM was 0x444444 (2.11:1 — fails WCAG
 * 1.4.3 minimum 4.5:1) and TH_TEXT_SECONDARY was 0x666666 (3.42:1 — same
 * fail).  Bumped DIM → 0x808088 (5.05:1) and SECONDARY → 0x9A9AA3 (7.20:1)
 * so mode-chip subtitles, ghost text, OFFLINE-hero body, Settings + Notes
 * list metadata stay readable in low light. */
#define TH_TEXT_SECONDARY 0x9A9AA3 /* Metadata, dates */
#define TH_TEXT_DIM 0x808088       /* Timestamps, placeholders */

#define TH_AMBER          0xF59E0B  /* Primary accent */
#define TH_AMBER_DARK     0xD97706  /* Pressed / gradient end */
#define TH_AMBER_GLOW     0xF5A623  /* Shadow glow */

#define TH_MODE_LOCAL     0x22C55E  /* Green */
#define TH_MODE_HYBRID    0xEAB308  /* Yellow */
#define TH_MODE_CLOUD     0x3B82F6  /* Blue */
#define TH_MODE_CLAW      0xF43F5E  /* Rose */
#define TH_MODE_ONBOARD 0x8E5BFF    /* Violet — vmode=4 K144 onboard */
#define TH_MODE_SOLO    0x06B6D4    /* Cyan — vmode=5 SOLO_DIRECT (TT #370) */

#define TH_STATUS_GREEN   0x22C55E
#define TH_STATUS_RED     0xEF4444

/* ── Spacing (DPI-scaled, real px on Tab5 shown in comments) ─ */
#define TH_MARGIN       DPI_SCALE(18)   /* 24px side margins */
#define TH_CARD_GAP     DPI_SCALE(9)    /* 12px between cards */
#define TH_CARD_PAD     DPI_SCALE(15)   /* 20px inside cards */
#define TH_CARD_RADIUS  DPI_SCALE(15)   /* 20px for elevated cards */
#define TH_INFO_RADIUS  DPI_SCALE(12)   /* 16px for info cards */
#define TH_SECTION_GAP  DPI_SCALE(15)   /* 20px between sections */
#define TH_INPUT_H      DPI_SCALE(38)   /* 52px input pill height */
#define TH_INPUT_R      DPI_SCALE(20)   /* 26px input pill radius */
#define TH_NAV_H        DPI_SCALE(54)   /* 72px nav bar height */
#define TH_STATUS_H     DPI_SCALE(30)   /* 40px status bar height */

/* ── Mode arrays ───────────────────────────────────────────── */
/* TT #328 Wave 1: grew from [4] to [VOICE_MODE_COUNT] to include Onboard.
 * Indexing with vm == 4 was previously UB (read past array end) on
 * Sessions/Drawer call-sites. */
extern const char *th_mode_names[VOICE_MODE_COUNT];
extern const uint32_t th_mode_colors[VOICE_MODE_COUNT];

/* ── Shared styles (initialized once by ui_theme_init) ─────── */
extern lv_style_t s_style_card;
extern lv_style_t s_style_card_elevated;

/* ── Init — call once before any screen creation ───────────── */
void ui_theme_init(void);
