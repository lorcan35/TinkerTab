/*
 * ui_keyboard.c — Glyph OS on-screen keyboard overlay
 * 720x1280 portrait, LVGL v9
 *
 * Full custom keyboard (no lv_keyboard widget) for complete design control.
 * Dark glass aesthetic with cyan accents, smooth slide-up animation.
 *
 * Sits on lv_layer_top() so it overlays any screen. A floating trigger
 * button in the bottom-right corner toggles it open/closed.
 *
 * Two layers: QWERTY (letters) and 123 (numbers/symbols).
 */

#include "ui_keyboard.h"
#include "config.h"
#include "esp_log.h"
#include <string.h>
#include <ctype.h>

static const char *TAG = "ui_kbd";

/* ── Palette — Glyph OS dark glass ─────────────────────────────── */
#define KB_BG           0x0A0A0A
#define KB_KEY_BG       0x1A1A1A   /* rgba(255,255,255,0.06) on black */
#define KB_KEY_PRESS    0x2A2A2A   /* rgba(255,255,255,0.12) on black */
#define KB_KEY_SPECIAL  0x222228   /* special keys — slight blue tint */
#define KB_KEY_ENTER    0x003844   /* cyan tint for enter */
#define KB_SPACE_BG     0x0A1214   /* rgba(0,229,255,0.04) on black */
#define KB_CYAN         0x00E5FF   /* primary accent */
#define KB_TEXT         0xB3B3B3   /* rgba(255,255,255,0.7) */
#define KB_TEXT_DIM     0x666666   /* dimmed text */
#define KB_SEP          0x101010   /* rgba(255,255,255,0.06) separator */
#define KB_HANDLE       0x262626   /* drag handle */
#define KB_TRIGGER_BG   0x0A1A1E   /* trigger button bg */
#define KB_TRIGGER_BRD  0x0D3840   /* trigger button border */

/* ── Layout constants ──────────────────────────────────────────── */
#define SW              720
#define SH              1280
#define KB_HEIGHT       420
#define KB_ANIM_SHOW_MS 300
#define KB_ANIM_HIDE_MS 250

#define KEY_H           48
#define KEY_GAP         4
#define KEY_RAD         8
#define ROW_PAD_X       6      /* horizontal padding inside keyboard */
#define ROW_GAP_Y       4

#define TRIGGER_SZ      44
#define TRIGGER_MARGIN  20

/* ── Key definitions ───────────────────────────────────────────── */

/* QWERTY rows — NULL-terminated */
static const char *s_row0_lower[] = {"q","w","e","r","t","y","u","i","o","p", NULL};
static const char *s_row1_lower[] = {"a","s","d","f","g","h","j","k","l", NULL};
static const char *s_row2_lower[] = {"z","x","c","v","b","n","m", NULL};

static const char *s_row0_upper[] = {"Q","W","E","R","T","Y","U","I","O","P", NULL};
static const char *s_row1_upper[] = {"A","S","D","F","G","H","J","K","L", NULL};
static const char *s_row2_upper[] = {"Z","X","C","V","B","N","M", NULL};

/* Number/symbol layer */
static const char *s_num_row0[] = {"1","2","3","4","5","6","7","8","9","0", NULL};
static const char *s_num_row1[] = {"@","#","$","&","*","(",")","-","'","\"", NULL};
static const char *s_num_row2[] = {"+","=","%","_","\\","|","~","<",">", NULL};

/* ── Key type enum ─────────────────────────────────────────────── */
typedef enum {
    KEY_CHAR,       /* regular character key */
    KEY_BACKSPACE,
    KEY_SHIFT,
    KEY_ENTER,
    KEY_SPACE,
    KEY_LAYER,      /* "123" / "ABC" toggle */
} key_type_t;

/* ── State ─────────────────────────────────────────────────────── */
static lv_obj_t  *s_kb_panel      = NULL;   /* main keyboard panel */
static lv_obj_t  *s_trigger_btn   = NULL;   /* floating trigger */
static lv_obj_t  *s_target_ta     = NULL;   /* textarea receiving input */
static bool       s_visible       = false;
static bool       s_shifted       = false;  /* shift state */
static bool       s_caps_lock     = false;  /* double-tap shift */
static bool       s_num_layer     = false;  /* numbers/symbols layer */
static bool       s_num_built     = false;  /* lazy: number rows created? */

/* Key rows — containers for easy show/hide on layer switch */
static lv_obj_t  *s_letter_rows[4] = {NULL}; /* row0..row3 (letters + bottom) */
static lv_obj_t  *s_num_rows[4]    = {NULL}; /* row0..row3 (numbers + bottom) */
static lv_obj_t  *s_key_area       = NULL;   /* saved for lazy num row creation */

/* Labels on letter keys that need updating on shift */
static lv_obj_t  *s_letter_labels[26]; /* a-z labels for shift toggle */
static int        s_letter_label_count = 0;

/* Shift key label (to show active state) */
static lv_obj_t  *s_shift_lbl     = NULL;
static lv_obj_t  *s_shift_key     = NULL;

/* Layer toggle labels */
static lv_obj_t  *s_layer_lbl_letters = NULL; /* "123" on letter layer */
static lv_obj_t  *s_layer_lbl_nums    = NULL; /* "ABC" on num layer */

/* ── Forward declarations ──────────────────────────────────────── */
static void build_keyboard_panel(void);
static void build_trigger_button(void);
static void build_letter_rows(lv_obj_t *parent);
static void build_number_rows(lv_obj_t *parent);
static lv_obj_t *make_key(lv_obj_t *row, const char *label, int w, int h,
                           uint32_t bg_col, uint32_t txt_col, const lv_font_t *font,
                           key_type_t type);
static void key_press_cb(lv_event_t *e);
static void trigger_click_cb(lv_event_t *e);
static void slide_anim_cb(void *obj, int32_t val);
static void hide_anim_ready_cb(lv_anim_t *a);
static void apply_shift_state(void);
static void switch_layer(bool to_numbers);

/* ========================================================================= */
/*  Public API                                                                */
/* ========================================================================= */

void ui_keyboard_init(lv_obj_t *parent)
{
    (void)parent; /* We use lv_layer_top() directly */

    ESP_LOGI(TAG, "Initializing Glyph OS keyboard overlay");

    build_keyboard_panel();
    build_trigger_button();

    /* Start hidden — move below screen */
    lv_obj_set_y(s_kb_panel, SH);
    lv_obj_add_flag(s_kb_panel, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;

    ESP_LOGI(TAG, "Keyboard initialized (%dx%d), trigger at bottom-right", SW, KB_HEIGHT);
}

void ui_keyboard_show(lv_obj_t *target_textarea)
{
    if (s_visible) {
        /* Already visible — just update target */
        s_target_ta = target_textarea;
        return;
    }

    s_target_ta = target_textarea;
    s_visible = true;

    /* Unhide and animate slide-up */
    lv_obj_clear_flag(s_kb_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(s_kb_panel, SH); /* start below screen */

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_kb_panel);
    lv_anim_set_values(&a, SH, SH - KB_HEIGHT);
    lv_anim_set_duration(&a, KB_ANIM_SHOW_MS);
    lv_anim_set_exec_cb(&a, slide_anim_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    /* Hide trigger while keyboard is open */
    lv_obj_add_flag(s_trigger_btn, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "Keyboard shown");
}

void ui_keyboard_hide(void)
{
    if (!s_visible) return;

    s_visible = false;

    /* Animate slide-down */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_kb_panel);
    lv_anim_set_values(&a, SH - KB_HEIGHT, SH);
    lv_anim_set_duration(&a, KB_ANIM_HIDE_MS);
    lv_anim_set_exec_cb(&a, slide_anim_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&a, hide_anim_ready_cb);
    lv_anim_start(&a);

    /* Reset shift/caps on hide */
    s_shifted = false;
    s_caps_lock = false;
    apply_shift_state();

    /* Reset to letter layer on hide */
    if (s_num_layer) {
        switch_layer(false);
    }

    ESP_LOGI(TAG, "Keyboard hiding");
}

void ui_keyboard_toggle(lv_obj_t *target_textarea)
{
    if (s_visible) {
        ui_keyboard_hide();
    } else {
        ui_keyboard_show(target_textarea);
    }
}

bool ui_keyboard_is_visible(void)
{
    return s_visible;
}

void ui_keyboard_set_target(lv_obj_t *target_textarea)
{
    s_target_ta = target_textarea;
}

lv_obj_t *ui_keyboard_get_trigger_btn(void)
{
    return s_trigger_btn;
}

/* ========================================================================= */
/*  Build keyboard panel                                                      */
/* ========================================================================= */

static void build_keyboard_panel(void)
{
    s_kb_panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_kb_panel, SW, KB_HEIGHT);
    lv_obj_set_pos(s_kb_panel, 0, SH); /* start offscreen */
    lv_obj_set_style_bg_color(s_kb_panel, lv_color_hex(KB_BG), 0);
    lv_obj_set_style_bg_opa(s_kb_panel, LV_OPA_90 + 5, 0); /* ~92% opacity */
    lv_obj_set_style_border_width(s_kb_panel, 0, 0);
    lv_obj_set_style_radius(s_kb_panel, 0, 0);
    lv_obj_set_style_pad_all(s_kb_panel, 0, 0);
    lv_obj_clear_flag(s_kb_panel, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Top separator line ────────────────────────────────────── */
    lv_obj_t *sep = lv_obj_create(s_kb_panel);
    lv_obj_set_size(sep, SW, 1);
    lv_obj_set_pos(sep, 0, 0);
    lv_obj_set_style_bg_color(sep, lv_color_hex(KB_SEP), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_set_style_radius(sep, 0, 0);
    lv_obj_clear_flag(sep, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* ── Drag handle ───────────────────────────────────────────── */
    lv_obj_t *handle = lv_obj_create(s_kb_panel);
    lv_obj_set_size(handle, 40, 3);
    lv_obj_set_pos(handle, (SW - 40) / 2, 6);
    lv_obj_set_style_bg_color(handle, lv_color_hex(KB_HANDLE), 0);
    lv_obj_set_style_bg_opa(handle, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(handle, 0, 0);
    lv_obj_set_style_radius(handle, 2, 0);
    lv_obj_clear_flag(handle, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* ── Key container ─────────────────────────────────────────── */
    lv_obj_t *key_area = lv_obj_create(s_kb_panel);
    lv_obj_set_size(key_area, SW, KB_HEIGHT - 16);
    lv_obj_set_pos(key_area, 0, 16);
    lv_obj_set_style_bg_opa(key_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(key_area, 0, 0);
    lv_obj_set_style_pad_all(key_area, 0, 0);
    lv_obj_set_style_radius(key_area, 0, 0);
    lv_obj_clear_flag(key_area, LV_OBJ_FLAG_SCROLLABLE);

    /* Build both layers inside key_area */
    build_letter_rows(key_area);

    /* Number rows built lazily on first switch_layer(true) to avoid
       LVGL crash from too many objects created in one batch on layer_top */
    s_key_area = key_area;
    s_num_built = false;
    s_num_layer = false;
}

/* ========================================================================= */
/*  Build letter (QWERTY) rows                                                */
/* ========================================================================= */

static void build_letter_rows(lv_obj_t *parent)
{
    int y = 8;
    int avail_w = SW - 2 * ROW_PAD_X;

    s_letter_label_count = 0;

    /* ── Row 0: QWERTYUIOP (10 keys) ─────────────────────────── */
    {
        int num_keys = 10;
        int total_gap = (num_keys - 1) * KEY_GAP;
        int key_w = (avail_w - total_gap) / num_keys;

        lv_obj_t *row = lv_obj_create(parent);
        lv_obj_set_size(row, SW, KEY_H);
        lv_obj_set_pos(row, 0, y);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        s_letter_rows[0] = row;

        for (int i = 0; s_row0_lower[i] != NULL; i++) {
            int x = ROW_PAD_X + i * (key_w + KEY_GAP);
            lv_obj_t *key = make_key(row, s_row0_lower[i], key_w, KEY_H,
                                      KB_KEY_BG, KB_TEXT, &lv_font_montserrat_18, KEY_CHAR);
            lv_obj_set_pos(key, x, 0);
            /* Store label for shift toggling */
            lv_obj_t *lbl = lv_obj_get_child(key, 0);
            if (lbl && s_letter_label_count < 26) {
                s_letter_labels[s_letter_label_count++] = lbl;
            }
        }
    }
    y += KEY_H + ROW_GAP_Y;

    /* ── Row 1: ASDFGHJKL (9 keys, centered) ─────────────────── */
    {
        int num_keys = 9;
        int total_gap = (num_keys - 1) * KEY_GAP;
        int key_w = (avail_w - total_gap) / 10; /* same key width as row 0 */
        int row_w = num_keys * key_w + (num_keys - 1) * KEY_GAP;
        int row_x = (SW - row_w) / 2;

        lv_obj_t *row = lv_obj_create(parent);
        lv_obj_set_size(row, SW, KEY_H);
        lv_obj_set_pos(row, 0, y);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        s_letter_rows[1] = row;

        for (int i = 0; s_row1_lower[i] != NULL; i++) {
            int x = row_x + i * (key_w + KEY_GAP);
            lv_obj_t *key = make_key(row, s_row1_lower[i], key_w, KEY_H,
                                      KB_KEY_BG, KB_TEXT, &lv_font_montserrat_18, KEY_CHAR);
            lv_obj_set_pos(key, x, 0);
            lv_obj_t *lbl = lv_obj_get_child(key, 0);
            if (lbl && s_letter_label_count < 26) {
                s_letter_labels[s_letter_label_count++] = lbl;
            }
        }
    }
    y += KEY_H + ROW_GAP_Y;

    /* ── Row 2: shift + ZXCVBNM + backspace ───────────────────── */
    {
        int num_char_keys = 7;
        int special_w = 56; /* shift and backspace width */
        int total_gap = (num_char_keys + 1) * KEY_GAP; /* gaps between all keys */
        int char_area = avail_w - 2 * special_w - total_gap;
        int key_w = char_area / num_char_keys;

        lv_obj_t *row = lv_obj_create(parent);
        lv_obj_set_size(row, SW, KEY_H);
        lv_obj_set_pos(row, 0, y);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        s_letter_rows[2] = row;

        int x = ROW_PAD_X;

        /* Shift key */
        s_shift_key = make_key(row, LV_SYMBOL_UP, special_w, KEY_H,
                                KB_KEY_SPECIAL, KB_TEXT, &lv_font_montserrat_18, KEY_SHIFT);
        lv_obj_set_pos(s_shift_key, x, 0);
        s_shift_lbl = lv_obj_get_child(s_shift_key, 0);
        x += special_w + KEY_GAP;

        /* Letter keys */
        for (int i = 0; s_row2_lower[i] != NULL; i++) {
            lv_obj_t *key = make_key(row, s_row2_lower[i], key_w, KEY_H,
                                      KB_KEY_BG, KB_TEXT, &lv_font_montserrat_18, KEY_CHAR);
            lv_obj_set_pos(key, x, 0);
            lv_obj_t *lbl = lv_obj_get_child(key, 0);
            if (lbl && s_letter_label_count < 26) {
                s_letter_labels[s_letter_label_count++] = lbl;
            }
            x += key_w + KEY_GAP;
        }

        /* Backspace key */
        lv_obj_t *bksp = make_key(row, LV_SYMBOL_BACKSPACE, special_w, KEY_H,
                                   KB_KEY_SPECIAL, KB_TEXT, &lv_font_montserrat_18, KEY_BACKSPACE);
        lv_obj_set_pos(bksp, SW - ROW_PAD_X - special_w, 0);
    }
    y += KEY_H + ROW_GAP_Y;

    /* ── Row 3: 123 + "," + space + "." + enter ───────────────── */
    {
        int layer_w = 64;
        int comma_w = 44;
        int dot_w   = 44;
        int enter_w = 80;
        int fixed_w = layer_w + comma_w + dot_w + enter_w + 4 * KEY_GAP;
        int space_w = avail_w - fixed_w;

        lv_obj_t *row = lv_obj_create(parent);
        lv_obj_set_size(row, SW, KEY_H);
        lv_obj_set_pos(row, 0, y);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        s_letter_rows[3] = row;

        int x = ROW_PAD_X;

        /* "123" layer toggle */
        lv_obj_t *layer_key = make_key(row, "123", layer_w, KEY_H,
                                        KB_KEY_SPECIAL, KB_TEXT, &lv_font_montserrat_16, KEY_LAYER);
        lv_obj_set_pos(layer_key, x, 0);
        s_layer_lbl_letters = lv_obj_get_child(layer_key, 0);
        x += layer_w + KEY_GAP;

        /* comma */
        lv_obj_t *comma = make_key(row, ",", comma_w, KEY_H,
                                    KB_KEY_BG, KB_TEXT_DIM, &lv_font_montserrat_18, KEY_CHAR);
        lv_obj_set_pos(comma, x, 0);
        x += comma_w + KEY_GAP;

        /* space bar */
        lv_obj_t *space = make_key(row, " ", space_w, KEY_H,
                                    KB_SPACE_BG, KB_TEXT, &lv_font_montserrat_18, KEY_SPACE);
        lv_obj_set_pos(space, x, 0);
        x += space_w + KEY_GAP;

        /* period */
        lv_obj_t *dot = make_key(row, ".", dot_w, KEY_H,
                                  KB_KEY_BG, KB_TEXT_DIM, &lv_font_montserrat_18, KEY_CHAR);
        lv_obj_set_pos(dot, x, 0);
        x += dot_w + KEY_GAP;

        /* enter/done */
        lv_obj_t *enter = make_key(row, "Done", enter_w, KEY_H,
                                    KB_KEY_ENTER, KB_CYAN, &lv_font_montserrat_16, KEY_ENTER);
        lv_obj_set_pos(enter, x, 0);
    }
}

/* ========================================================================= */
/*  Build number/symbol rows                                                  */
/* ========================================================================= */

static void build_number_rows(lv_obj_t *parent)
{
    int y = 8;
    int avail_w = SW - 2 * ROW_PAD_X;

    /* ── Row 0: 1234567890 (10 keys) ─────────────────────────── */
    {
        int num_keys = 10;
        int total_gap = (num_keys - 1) * KEY_GAP;
        int key_w = (avail_w - total_gap) / num_keys;

        lv_obj_t *row = lv_obj_create(parent);
        lv_obj_set_size(row, SW, KEY_H);
        lv_obj_set_pos(row, 0, y);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        s_num_rows[0] = row;

        for (int i = 0; s_num_row0[i] != NULL; i++) {
            int x = ROW_PAD_X + i * (key_w + KEY_GAP);
            lv_obj_t *key = make_key(row, s_num_row0[i], key_w, KEY_H,
                                      KB_KEY_BG, KB_TEXT, &lv_font_montserrat_18, KEY_CHAR);
            lv_obj_set_pos(key, x, 0);
        }
    }

    y += KEY_H + ROW_GAP_Y;

    /* ── Row 1: @#$&*()-'" (10 keys) ─────────────────────────── */
    {
        int num_keys = 10;
        int total_gap = (num_keys - 1) * KEY_GAP;
        int key_w = (avail_w - total_gap) / num_keys;

        lv_obj_t *row = lv_obj_create(parent);
        lv_obj_set_size(row, SW, KEY_H);
        lv_obj_set_pos(row, 0, y);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        s_num_rows[1] = row;

        for (int i = 0; s_num_row1[i] != NULL; i++) {
            int x = ROW_PAD_X + i * (key_w + KEY_GAP);
            lv_obj_t *key = make_key(row, s_num_row1[i], key_w, KEY_H,
                                      KB_KEY_BG, KB_TEXT, &lv_font_montserrat_18, KEY_CHAR);
            lv_obj_set_pos(key, x, 0);
        }
    }

    y += KEY_H + ROW_GAP_Y;

    /* ── Row 2: #+=%_\|~<> (9 keys, centered) + backspace ───── */
    {
        int num_char_keys = 9;
        int special_w = 56;
        int total_gap = num_char_keys * KEY_GAP;
        int char_area = avail_w - special_w - total_gap;
        int key_w = char_area / num_char_keys;

        lv_obj_t *row = lv_obj_create(parent);
        lv_obj_set_size(row, SW, KEY_H);
        lv_obj_set_pos(row, 0, y);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        s_num_rows[2] = row;

        int x = ROW_PAD_X;
        for (int i = 0; s_num_row2[i] != NULL; i++) {
            lv_obj_t *key = make_key(row, s_num_row2[i], key_w, KEY_H,
                                      KB_KEY_BG, KB_TEXT, &lv_font_montserrat_18, KEY_CHAR);
            lv_obj_set_pos(key, x, 0);
            x += key_w + KEY_GAP;
        }

        /* Backspace */
        lv_obj_t *bksp = make_key(row, LV_SYMBOL_BACKSPACE, special_w, KEY_H,
                                   KB_KEY_SPECIAL, KB_TEXT, &lv_font_montserrat_18, KEY_BACKSPACE);
        lv_obj_set_pos(bksp, SW - ROW_PAD_X - special_w, 0);
    }

    y += KEY_H + ROW_GAP_Y;

    /* ── Row 3: ABC + "," + space + "." + enter ───────────────── */
    {
        int layer_w = 64;
        int comma_w = 44;
        int dot_w   = 44;
        int enter_w = 80;
        int fixed_w = layer_w + comma_w + dot_w + enter_w + 4 * KEY_GAP;
        int space_w = avail_w - fixed_w;

        lv_obj_t *row = lv_obj_create(parent);
        lv_obj_set_size(row, SW, KEY_H);
        lv_obj_set_pos(row, 0, y);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        s_num_rows[3] = row;

        int x = ROW_PAD_X;

        /* "ABC" layer toggle */
        lv_obj_t *layer_key = make_key(row, "ABC", layer_w, KEY_H,
                                        KB_KEY_SPECIAL, KB_TEXT, &lv_font_montserrat_16, KEY_LAYER);
        lv_obj_set_pos(layer_key, x, 0);
        s_layer_lbl_nums = lv_obj_get_child(layer_key, 0);
        x += layer_w + KEY_GAP;

        /* comma */
        lv_obj_t *comma = make_key(row, ",", comma_w, KEY_H,
                                    KB_KEY_BG, KB_TEXT_DIM, &lv_font_montserrat_18, KEY_CHAR);
        lv_obj_set_pos(comma, x, 0);
        x += comma_w + KEY_GAP;

        /* space bar */
        lv_obj_t *space = make_key(row, " ", space_w, KEY_H,
                                    KB_SPACE_BG, KB_TEXT, &lv_font_montserrat_18, KEY_SPACE);
        lv_obj_set_pos(space, x, 0);
        x += space_w + KEY_GAP;

        /* period */
        lv_obj_t *dot = make_key(row, ".", dot_w, KEY_H,
                                  KB_KEY_BG, KB_TEXT_DIM, &lv_font_montserrat_18, KEY_CHAR);
        lv_obj_set_pos(dot, x, 0);
        x += dot_w + KEY_GAP;

        /* enter/done */
        lv_obj_t *enter = make_key(row, "Done", enter_w, KEY_H,
                                    KB_KEY_ENTER, KB_CYAN, &lv_font_montserrat_16, KEY_ENTER);
        lv_obj_set_pos(enter, x, 0);
    }
}

/* ========================================================================= */
/*  Build a single key                                                        */
/* ========================================================================= */

static lv_obj_t *make_key(lv_obj_t *row, const char *label, int w, int h,
                           uint32_t bg_col, uint32_t txt_col, const lv_font_t *font,
                           key_type_t type)
{
    lv_obj_t *key = lv_obj_create(row);
    lv_obj_set_size(key, w, h);
    lv_obj_set_style_bg_color(key, lv_color_hex(bg_col), 0);
    lv_obj_set_style_bg_opa(key, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(key, KEY_RAD, 0);
    lv_obj_set_style_border_width(key, 0, 0);
    lv_obj_set_style_pad_all(key, 0, 0);
    lv_obj_clear_flag(key, LV_OBJ_FLAG_SCROLLABLE);

    /* Press feedback style */
    lv_obj_set_style_bg_color(key, lv_color_hex(KB_KEY_PRESS), LV_STATE_PRESSED);

    /* Label */
    lv_obj_t *lbl = lv_label_create(key);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(txt_col), 0);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_center(lbl);

    /* Store the key type as user data for the event callback */
    lv_obj_set_user_data(key, (void *)(uintptr_t)type);

    /* Event handlers */
    lv_obj_add_event_cb(key, key_press_cb, LV_EVENT_CLICKED, NULL);

    return key;
}

/* ========================================================================= */
/*  Build floating trigger button                                             */
/* ========================================================================= */

static void build_trigger_button(void)
{
    s_trigger_btn = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_trigger_btn, TRIGGER_SZ, TRIGGER_SZ);
    lv_obj_set_pos(s_trigger_btn, SW - TRIGGER_SZ - TRIGGER_MARGIN,
                    SH - TRIGGER_SZ - TRIGGER_MARGIN - 64); /* above nav bar */
    lv_obj_set_style_bg_color(s_trigger_btn, lv_color_hex(KB_TRIGGER_BG), 0);
    lv_obj_set_style_bg_opa(s_trigger_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_trigger_btn, TRIGGER_SZ / 2, 0);
    lv_obj_set_style_border_color(s_trigger_btn, lv_color_hex(KB_TRIGGER_BRD), 0);
    lv_obj_set_style_border_width(s_trigger_btn, 1, 0);
    lv_obj_set_style_border_opa(s_trigger_btn, LV_OPA_40, 0);
    lv_obj_set_style_pad_all(s_trigger_btn, 0, 0);
    lv_obj_clear_flag(s_trigger_btn, LV_OBJ_FLAG_SCROLLABLE);

    /* Press feedback */
    lv_obj_set_style_bg_color(s_trigger_btn, lv_color_hex(KB_TRIGGER_BRD), LV_STATE_PRESSED);

    /* Keyboard icon — use LVGL symbol or text glyph */
    lv_obj_t *icon = lv_label_create(s_trigger_btn);
    lv_label_set_text(icon, LV_SYMBOL_KEYBOARD);
    lv_obj_set_style_text_color(icon, lv_color_hex(KB_CYAN), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_18, 0);
    lv_obj_center(icon);

    lv_obj_add_event_cb(s_trigger_btn, trigger_click_cb, LV_EVENT_CLICKED, NULL);
}

/* ========================================================================= */
/*  Event callbacks                                                           */
/* ========================================================================= */

static void key_press_cb(lv_event_t *e)
{
    lv_obj_t *key = lv_event_get_target(e);
    key_type_t type = (key_type_t)(uintptr_t)lv_obj_get_user_data(key);

    switch (type) {
    case KEY_CHAR:
    case KEY_SPACE: {
        /* Get the label text from the key's child label */
        lv_obj_t *lbl = lv_obj_get_child(key, 0);
        if (lbl == NULL) break;
        const char *txt = lv_label_get_text(lbl);
        if (txt == NULL || txt[0] == '\0') {
            /* Space key — label is empty/space */
            if (s_target_ta) lv_textarea_add_char(s_target_ta, ' ');
        } else {
            if (s_target_ta) {
                /* If shifted and it's a single letter, send uppercase */
                if (s_shifted && strlen(txt) == 1 && isalpha((unsigned char)txt[0])) {
                    char upper = (char)toupper((unsigned char)txt[0]);
                    lv_textarea_add_char(s_target_ta, upper);
                } else {
                    lv_textarea_add_text(s_target_ta, txt);
                }
            }
        }
        /* Auto-unshift after typing (unless caps lock) */
        if (s_shifted && !s_caps_lock && !s_num_layer) {
            s_shifted = false;
            apply_shift_state();
        }
        break;
    }

    case KEY_BACKSPACE:
        if (s_target_ta) lv_textarea_delete_char(s_target_ta);
        break;

    case KEY_ENTER:
        if (s_target_ta) {
            /* Send newline — screens can also hook into textarea events */
            lv_textarea_add_char(s_target_ta, '\n');
        }
        /* Optionally hide keyboard on Done */
        ui_keyboard_hide();
        break;

    case KEY_SHIFT: {
        if (s_shifted && !s_caps_lock) {
            /* Second tap while shifted — enable caps lock */
            s_caps_lock = true;
            s_shifted = true;
        } else if (s_caps_lock) {
            /* Third tap — disable everything */
            s_caps_lock = false;
            s_shifted = false;
        } else {
            /* First tap — enable shift */
            s_shifted = true;
        }
        apply_shift_state();
        break;
    }

    case KEY_LAYER:
        switch_layer(!s_num_layer);
        break;
    }
}

static void trigger_click_cb(lv_event_t *e)
{
    (void)e;
    ui_keyboard_toggle(s_target_ta);
}

/* ========================================================================= */
/*  Animation helpers                                                         */
/* ========================================================================= */

static void slide_anim_cb(void *obj, int32_t val)
{
    lv_obj_set_y((lv_obj_t *)obj, val);
}

static void hide_anim_ready_cb(lv_anim_t *a)
{
    (void)a;
    lv_obj_add_flag(s_kb_panel, LV_OBJ_FLAG_HIDDEN);
    /* Re-show trigger button */
    lv_obj_clear_flag(s_trigger_btn, LV_OBJ_FLAG_HIDDEN);
    s_target_ta = NULL;
}

/* ========================================================================= */
/*  Shift state management                                                    */
/* ========================================================================= */

static void apply_shift_state(void)
{
    /* Update letter labels */
    const char **row0 = s_shifted ? s_row0_upper : s_row0_lower;
    const char **row1 = s_shifted ? s_row1_upper : s_row1_lower;
    const char **row2 = s_shifted ? s_row2_upper : s_row2_lower;

    int idx = 0;

    /* Row 0: 10 keys */
    for (int i = 0; row0[i] != NULL && idx < s_letter_label_count; i++, idx++) {
        lv_label_set_text(s_letter_labels[idx], row0[i]);
    }
    /* Row 1: 9 keys */
    for (int i = 0; row1[i] != NULL && idx < s_letter_label_count; i++, idx++) {
        lv_label_set_text(s_letter_labels[idx], row1[i]);
    }
    /* Row 2: 7 keys */
    for (int i = 0; row2[i] != NULL && idx < s_letter_label_count; i++, idx++) {
        lv_label_set_text(s_letter_labels[idx], row2[i]);
    }

    /* Update shift key visual */
    if (s_shift_key) {
        if (s_caps_lock) {
            /* Caps lock — cyan highlight */
            lv_obj_set_style_bg_color(s_shift_key, lv_color_hex(KB_KEY_ENTER), 0);
            if (s_shift_lbl) lv_obj_set_style_text_color(s_shift_lbl, lv_color_hex(KB_CYAN), 0);
        } else if (s_shifted) {
            /* Shift active — brighter */
            lv_obj_set_style_bg_color(s_shift_key, lv_color_hex(KB_KEY_PRESS), 0);
            if (s_shift_lbl) lv_obj_set_style_text_color(s_shift_lbl, lv_color_hex(0xFFFFFF), 0);
        } else {
            /* Normal */
            lv_obj_set_style_bg_color(s_shift_key, lv_color_hex(KB_KEY_SPECIAL), 0);
            if (s_shift_lbl) lv_obj_set_style_text_color(s_shift_lbl, lv_color_hex(KB_TEXT), 0);
        }
    }
}

/* ========================================================================= */
/*  Layer switching                                                           */
/* ========================================================================= */

static void switch_layer(bool to_numbers)
{
    s_num_layer = to_numbers;

    /* Lazy-build number rows on first switch */
    if (to_numbers && !s_num_built && s_key_area) {
        ESP_LOGI(TAG, "Lazy-building number rows...");
        build_number_rows(s_key_area);
        s_num_built = true;
    }

    for (int i = 0; i < 4; i++) {
        if (to_numbers) {
            if (s_letter_rows[i]) lv_obj_add_flag(s_letter_rows[i], LV_OBJ_FLAG_HIDDEN);
            if (s_num_rows[i])    lv_obj_clear_flag(s_num_rows[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            if (s_letter_rows[i]) lv_obj_clear_flag(s_letter_rows[i], LV_OBJ_FLAG_HIDDEN);
            if (s_num_rows[i])    lv_obj_add_flag(s_num_rows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Reset shift when switching back to letters */
    if (!to_numbers) {
        s_shifted = false;
        s_caps_lock = false;
        apply_shift_state();
    }
}
