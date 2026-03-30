/*
 * ui_files.c — TinkerTab file browser screen
 * 720x1280 portrait, LVGL v9, dark theme
 *
 * Browse /sdcard contents: folders first, then files, alphabetically sorted.
 * Tap folder → navigate in. Tap .wav → audio player. Tap .jpg/.png → preview.
 * Long-press → file info popup.
 */

#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "ui_files.h"
#include "ui_home.h"
#include "ui_audio.h"
#include "ui_core.h"
#include "sdcard.h"
#include "config.h"
#include "ui_port.h"

#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ui_files";

/* ── Palette ─────────────────────────────────────────────────── */
#define COL_BG          0x000000
#define COL_TOPBAR      0x1A1A2E
#define COL_ACCENT      0x3B82F6
#define COL_WHITE       0xFFFFFF
#define COL_GRAY        0x888888
#define COL_DARK_GRAY   0x333333
#define COL_ROW_BG      0x111122
#define COL_ROW_PRESS   0x1E293B
#define COL_PANEL_BG    0x1E293B
#define COL_RED         0xEF4444

/* ── Layout constants (720 x 1280) ───────────────────────────── */
#define SCREEN_W        720
#define SCREEN_H        1280
#define TOPBAR_H        48
#define ROW_H           64
#define BOTTOMBAR_H     44
#define LIST_H          (SCREEN_H - TOPBAR_H - BOTTOMBAR_H)
#define MAX_PATH_LEN    256
#define MAX_ENTRIES     256
#define ROOT_PATH       "/sdcard"

/* ── Directory entry (for sorting) ───────────────────────────── */
typedef struct {
    char     name[128];
    bool     is_dir;
    off_t    size;
} file_entry_t;

/* ── Module state ────────────────────────────────────────────── */
static lv_obj_t    *scr_files      = NULL;
static lv_obj_t    *topbar         = NULL;
static lv_obj_t    *lbl_path       = NULL;
static lv_obj_t    *file_list      = NULL;
static lv_obj_t    *bottombar      = NULL;
static lv_obj_t    *lbl_storage    = NULL;
static lv_obj_t    *no_sd_panel    = NULL;
static lv_obj_t    *img_preview    = NULL;

static char         current_path[MAX_PATH_LEN] = ROOT_PATH;
static file_entry_t *entries       = NULL;
static int          entry_count    = 0;

/* Forward declarations */
static void rebuild_list(void);
static void navigate_to(const char *path);
static void show_no_sd(void);
static void show_image_preview(const char *filepath);
static void show_file_info(const char *filepath, off_t size);
static const char *get_file_icon(const char *name, bool is_dir);
static const char *get_extension(const char *name);
static void format_size(off_t bytes, char *buf, size_t buf_len);
static int entry_cmp(const void *a, const void *b);

/* ── Event callbacks ─────────────────────────────────────────── */

static void cb_back_btn(lv_event_t *e)
{
    (void)e;
    /* At root → destroy screen (go back to home) */
    if (strcmp(current_path, ROOT_PATH) == 0) {
        ui_files_destroy();
        lv_screen_load(ui_home_get_screen());
        return;
    }
    /* Go up one directory */
    char *last_slash = strrchr(current_path, '/');
    if (last_slash && last_slash != current_path) {
        *last_slash = '\0';
        /* Ensure we don't go above /sdcard */
        if (strlen(current_path) < strlen(ROOT_PATH)) {
            strncpy(current_path, ROOT_PATH, MAX_PATH_LEN - 1);
            current_path[MAX_PATH_LEN - 1] = '\0';
        }
    } else {
        strncpy(current_path, ROOT_PATH, MAX_PATH_LEN - 1);
        current_path[MAX_PATH_LEN - 1] = '\0';
    }
    rebuild_list();
}

static void cb_row_click(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= entry_count) return;

    file_entry_t *ent = &entries[idx];

    if (ent->is_dir) {
        /* Navigate into folder */
        char new_path[MAX_PATH_LEN];
        snprintf(new_path, sizeof(new_path), "%s/%s", current_path, ent->name);
        navigate_to(new_path);
        return;
    }

    /* Build full filepath */
    char filepath[MAX_PATH_LEN];
    snprintf(filepath, sizeof(filepath), "%s/%s", current_path, ent->name);

    const char *ext = get_extension(ent->name);

    /* .wav → audio player */
    if (ext && strcasecmp(ext, "wav") == 0) {
        ui_audio_create(filepath);
        return;
    }

    /* .jpg/.png → image preview */
    if (ext && (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0 ||
                strcasecmp(ext, "png") == 0)) {
        show_image_preview(filepath);
        return;
    }
}

static void cb_row_long_press(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= entry_count) return;

    file_entry_t *ent = &entries[idx];
    char filepath[MAX_PATH_LEN];
    snprintf(filepath, sizeof(filepath), "%s/%s", current_path, ent->name);
    show_file_info(filepath, ent->size);
}

static void cb_retry_sd(lv_event_t *e)
{
    (void)e;
    if (no_sd_panel) {
        lv_obj_delete(no_sd_panel);
        no_sd_panel = NULL;
    }
    if (tab5_sdcard_mounted()) {
        rebuild_list();
    } else {
        show_no_sd();
    }
}

static void cb_close_preview(lv_event_t *e)
{
    (void)e;
    if (img_preview) {
        lv_obj_delete(img_preview);
        img_preview = NULL;
    }
}

static void cb_close_info(lv_event_t *e)
{
    lv_obj_t *panel = (lv_obj_t *)lv_event_get_user_data(e);
    if (panel) {
        lv_obj_delete(panel);
    }
}

/* ── Helpers ─────────────────────────────────────────────────── */

static const char *get_extension(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) return NULL;
    return dot + 1;
}

static const char *get_file_icon(const char *name, bool is_dir)
{
    if (is_dir) return LV_SYMBOL_DIRECTORY;

    const char *ext = get_extension(name);
    if (!ext) return LV_SYMBOL_FILE;

    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0 ||
        strcasecmp(ext, "png") == 0 || strcasecmp(ext, "bmp") == 0) {
        return LV_SYMBOL_IMAGE;
    }
    if (strcasecmp(ext, "wav") == 0 || strcasecmp(ext, "mp3") == 0 ||
        strcasecmp(ext, "flac") == 0) {
        return LV_SYMBOL_AUDIO;
    }
    if (strcasecmp(ext, "txt") == 0 || strcasecmp(ext, "md") == 0 ||
        strcasecmp(ext, "log") == 0 || strcasecmp(ext, "json") == 0) {
        return LV_SYMBOL_LIST;
    }
    return LV_SYMBOL_FILE;
}

static void format_size(off_t bytes, char *buf, size_t buf_len)
{
    if (bytes < 1024) {
        snprintf(buf, buf_len, "%d B", (int)bytes);
    } else if (bytes < 1024 * 1024) {
        snprintf(buf, buf_len, "%.1f KB", (double)bytes / 1024.0);
    } else if (bytes < (off_t)1024 * 1024 * 1024) {
        snprintf(buf, buf_len, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else {
        snprintf(buf, buf_len, "%.1f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    }
}

static int entry_cmp(const void *a, const void *b)
{
    const file_entry_t *ea = (const file_entry_t *)a;
    const file_entry_t *eb = (const file_entry_t *)b;
    /* Directories first */
    if (ea->is_dir && !eb->is_dir) return -1;
    if (!ea->is_dir && eb->is_dir) return 1;
    /* Then alphabetical (case-insensitive) */
    return strcasecmp(ea->name, eb->name);
}

static void navigate_to(const char *path)
{
    strncpy(current_path, path, MAX_PATH_LEN - 1);
    current_path[MAX_PATH_LEN - 1] = '\0';
    rebuild_list();
}

/* ── Read directory and populate entries array ───────────────── */

static void read_directory(void)
{
    entry_count = 0;

    if (!entries) {
        entries = UI_MALLOC_PSRAM(MAX_ENTRIES * sizeof(file_entry_t));
        if (!entries) {
            UI_LOGE(TAG, "Failed to allocate entries array");
            return;
        }
    }

    DIR *dir = opendir(current_path);
    if (!dir) {
        UI_LOGE(TAG, "Failed to open directory: %s", current_path);
        return;
    }

    struct dirent *de;
    while ((de = readdir(dir)) != NULL && entry_count < MAX_ENTRIES) {
        /* Skip hidden files and . / .. */
        if (de->d_name[0] == '.') continue;

        file_entry_t *ent = &entries[entry_count];
        strncpy(ent->name, de->d_name, sizeof(ent->name) - 1);
        ent->name[sizeof(ent->name) - 1] = '\0';

        /* Stat for size and type */
        char fullpath[MAX_PATH_LEN];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", current_path, de->d_name);

        struct stat st;
        if (stat(fullpath, &st) == 0) {
            ent->is_dir = S_ISDIR(st.st_mode);
            ent->size   = ent->is_dir ? 0 : st.st_size;
        } else {
            ent->is_dir = (de->d_type == DT_DIR);
            ent->size   = 0;
        }

        entry_count++;
    }
    closedir(dir);

    /* Sort: dirs first, then alphabetical */
    if (entry_count > 1) {
        qsort(entries, entry_count, sizeof(file_entry_t), entry_cmp);
    }

    UI_LOGI(TAG, "Read %d entries from %s", entry_count, current_path);
}

/* ── Rebuild the LVGL file list ──────────────────────────────── */

static void rebuild_list(void)
{
    if (!scr_files) return;

    /* Update path label */
    if (lbl_path) {
        lv_label_set_text(lbl_path, current_path);
    }

    /* Update storage info */
    if (lbl_storage && tab5_sdcard_mounted()) {
        uint64_t free_b  = tab5_sdcard_free_bytes();
        uint64_t total_b = tab5_sdcard_total_bytes();
        double free_gb   = (double)free_b  / (1024.0 * 1024.0 * 1024.0);
        double total_gb  = (double)total_b / (1024.0 * 1024.0 * 1024.0);
        char info[64];
        snprintf(info, sizeof(info), "%.1f GB free of %.1f GB", free_gb, total_gb);
        lv_label_set_text(lbl_storage, info);
    }

    /* Clear old list content */
    if (file_list) {
        lv_obj_clean(file_list);
    }

    /* Read directory */
    read_directory();

    if (entry_count == 0) {
        lv_obj_t *lbl_empty = lv_label_create(file_list);
        lv_label_set_text(lbl_empty, "Empty folder");
        lv_obj_set_style_text_color(lbl_empty, lv_color_hex(COL_GRAY), 0);
        lv_obj_set_style_text_font(lbl_empty, &lv_font_montserrat_18, 0);
        lv_obj_align(lbl_empty, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    /* Create rows */
    for (int i = 0; i < entry_count; i++) {
        file_entry_t *ent = &entries[i];

        /* Row container */
        lv_obj_t *row = lv_obj_create(file_list);
        lv_obj_set_size(row, SCREEN_W, ROW_H);
        lv_obj_set_style_bg_color(row, lv_color_hex(COL_ROW_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, lv_color_hex(COL_ROW_PRESS), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(COL_DARK_GRAY), 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_left(row, 16, 0);
        lv_obj_set_style_pad_right(row, 16, 0);
        lv_obj_set_style_pad_ver(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 12, 0);

        /* Icon */
        lv_obj_t *lbl_icon = lv_label_create(row);
        lv_label_set_text(lbl_icon, get_file_icon(ent->name, ent->is_dir));
        lv_obj_set_style_text_color(lbl_icon,
            ent->is_dir ? lv_color_hex(COL_ACCENT) : lv_color_hex(COL_GRAY), 0);
        lv_obj_set_style_text_font(lbl_icon, &lv_font_montserrat_24, 0);
        lv_obj_set_style_min_width(lbl_icon, 28, 0);

        /* Filename */
        lv_obj_t *lbl_name = lv_label_create(row);
        lv_label_set_text(lbl_name, ent->name);
        lv_label_set_long_mode(lbl_name, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(lbl_name, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_text_font(lbl_name, &lv_font_montserrat_18, 0);
        lv_obj_set_flex_grow(lbl_name, 1);
        lv_obj_set_style_max_width(lbl_name, SCREEN_W - 160, 0);

        /* Size (files only) */
        if (!ent->is_dir) {
            char size_str[32];
            format_size(ent->size, size_str, sizeof(size_str));
            lv_obj_t *lbl_size = lv_label_create(row);
            lv_label_set_text(lbl_size, size_str);
            lv_obj_set_style_text_color(lbl_size, lv_color_hex(COL_GRAY), 0);
            lv_obj_set_style_text_font(lbl_size, &lv_font_montserrat_18, 0);
        } else {
            /* Chevron for directories */
            lv_obj_t *lbl_chev = lv_label_create(row);
            lv_label_set_text(lbl_chev, LV_SYMBOL_RIGHT);
            lv_obj_set_style_text_color(lbl_chev, lv_color_hex(COL_GRAY), 0);
            lv_obj_set_style_text_font(lbl_chev, &lv_font_montserrat_16, 0);
        }

        /* Click handler */
        lv_obj_add_event_cb(row, cb_row_click, LV_EVENT_SHORT_CLICKED,
                            (void *)(intptr_t)i);
        /* Long-press handler */
        lv_obj_add_event_cb(row, cb_row_long_press, LV_EVENT_LONG_PRESSED,
                            (void *)(intptr_t)i);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    }
}

/* ── No SD Card screen ───────────────────────────────────────── */

static void show_no_sd(void)
{
    if (!scr_files) return;

    /* Hide normal UI elements */
    if (file_list) lv_obj_add_flag(file_list, LV_OBJ_FLAG_HIDDEN);
    if (bottombar) lv_obj_add_flag(bottombar, LV_OBJ_FLAG_HIDDEN);

    no_sd_panel = lv_obj_create(scr_files);
    lv_obj_set_size(no_sd_panel, SCREEN_W, SCREEN_H - TOPBAR_H);
    lv_obj_align(no_sd_panel, LV_ALIGN_TOP_LEFT, 0, TOPBAR_H);
    lv_obj_set_style_bg_color(no_sd_panel, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(no_sd_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(no_sd_panel, 0, 0);
    lv_obj_clear_flag(no_sd_panel, LV_OBJ_FLAG_SCROLLABLE);

    /* SD card icon */
    lv_obj_t *lbl_sd_icon = lv_label_create(no_sd_panel);
    lv_label_set_text(lbl_sd_icon, LV_SYMBOL_SD_CARD);
    lv_obj_set_style_text_color(lbl_sd_icon, lv_color_hex(COL_GRAY), 0);
    lv_obj_set_style_text_font(lbl_sd_icon, &lv_font_montserrat_48, 0);
    lv_obj_align(lbl_sd_icon, LV_ALIGN_CENTER, 0, -80);

    /* Message */
    lv_obj_t *lbl_msg = lv_label_create(no_sd_panel);
    lv_label_set_text(lbl_msg, "No SD Card");
    lv_obj_set_style_text_color(lbl_msg, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(lbl_msg, &lv_font_montserrat_28, 0);
    lv_obj_align(lbl_msg, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *lbl_sub = lv_label_create(no_sd_panel);
    lv_label_set_text(lbl_sub, "Insert a micro-SD card and tap Retry");
    lv_obj_set_style_text_color(lbl_sub, lv_color_hex(COL_GRAY), 0);
    lv_obj_set_style_text_font(lbl_sub, &lv_font_montserrat_18, 0);
    lv_obj_align(lbl_sub, LV_ALIGN_CENTER, 0, 20);

    /* Retry button */
    lv_obj_t *btn_retry = lv_button_create(no_sd_panel);
    lv_obj_set_size(btn_retry, 180, 48);
    lv_obj_align(btn_retry, LV_ALIGN_CENTER, 0, 80);
    lv_obj_set_style_bg_color(btn_retry, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_radius(btn_retry, 12, 0);

    lv_obj_t *lbl_retry = lv_label_create(btn_retry);
    lv_label_set_text(lbl_retry, "Retry");
    lv_obj_set_style_text_color(lbl_retry, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(lbl_retry, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl_retry);

    lv_obj_add_event_cb(btn_retry, cb_retry_sd, LV_EVENT_CLICKED, NULL);
}

/* ── Image preview overlay ───────────────────────────────────── */

static void show_image_preview(const char *filepath)
{
    if (img_preview) {
        lv_obj_delete(img_preview);
        img_preview = NULL;
    }

    img_preview = lv_obj_create(scr_files);
    lv_obj_set_size(img_preview, SCREEN_W, SCREEN_H);
    lv_obj_align(img_preview, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(img_preview, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(img_preview, LV_OPA_90, 0);
    lv_obj_set_style_border_width(img_preview, 0, 0);
    lv_obj_clear_flag(img_preview, LV_OBJ_FLAG_SCROLLABLE);

    /* Filename label */
    const char *basename = strrchr(filepath, '/');
    basename = basename ? basename + 1 : filepath;

    char display_text[160];
    snprintf(display_text, sizeof(display_text), "%s  %s", LV_SYMBOL_IMAGE, basename);

    lv_obj_t *lbl = lv_label_create(img_preview);
    lv_label_set_text(lbl, display_text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

    /* Close button */
    lv_obj_t *btn_close = lv_button_create(img_preview);
    lv_obj_set_size(btn_close, 48, 48);
    lv_obj_align(btn_close, LV_ALIGN_TOP_RIGHT, -16, 16);
    lv_obj_set_style_bg_color(btn_close, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_radius(btn_close, 24, 0);

    lv_obj_t *lbl_x = lv_label_create(btn_close);
    lv_label_set_text(lbl_x, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(lbl_x, lv_color_hex(COL_WHITE), 0);
    lv_obj_center(lbl_x);

    lv_obj_add_event_cb(btn_close, cb_close_preview, LV_EVENT_CLICKED, NULL);
}

/* ── File info popup ─────────────────────────────────────────── */

static void show_file_info(const char *filepath, off_t size)
{
    lv_obj_t *panel = lv_obj_create(scr_files);
    lv_obj_set_size(panel, SCREEN_W - 80, 200);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(COL_PANEL_BG), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 16, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_pad_all(panel, 20, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    /* Title */
    lv_obj_t *lbl_title = lv_label_create(panel);
    lv_label_set_text(lbl_title, "File Info");
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_20, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_LEFT, 0, 0);

    /* Path */
    lv_obj_t *lbl_fpath = lv_label_create(panel);
    lv_label_set_long_mode(lbl_fpath, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_fpath, SCREEN_W - 120);
    lv_label_set_text(lbl_fpath, filepath);
    lv_obj_set_style_text_color(lbl_fpath, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(lbl_fpath, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl_fpath, LV_ALIGN_TOP_LEFT, 0, 30);

    /* Size */
    char size_str[32];
    format_size(size, size_str, sizeof(size_str));
    char size_line[64];
    snprintf(size_line, sizeof(size_line), "Size: %s", size_str);

    lv_obj_t *lbl_fsize = lv_label_create(panel);
    lv_label_set_text(lbl_fsize, size_line);
    lv_obj_set_style_text_color(lbl_fsize, lv_color_hex(COL_GRAY), 0);
    lv_obj_set_style_text_font(lbl_fsize, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl_fsize, LV_ALIGN_TOP_LEFT, 0, 70);

    /* Close button */
    lv_obj_t *btn_close = lv_button_create(panel);
    lv_obj_set_size(btn_close, 120, 48);
    lv_obj_align(btn_close, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(btn_close, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_radius(btn_close, 8, 0);

    lv_obj_t *lbl_ok = lv_label_create(btn_close);
    lv_label_set_text(lbl_ok, "OK");
    lv_obj_set_style_text_color(lbl_ok, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(lbl_ok, &lv_font_montserrat_18, 0);
    lv_obj_center(lbl_ok);

    lv_obj_add_event_cb(btn_close, cb_close_info, LV_EVENT_CLICKED, panel);
}

/* ================================================================
 * ui_files_create
 * ================================================================ */
lv_obj_t *ui_files_create(void)
{
    strncpy(current_path, ROOT_PATH, MAX_PATH_LEN - 1);
    current_path[MAX_PATH_LEN - 1] = '\0';

    /* ── Screen ──────────────────────────────────────────────── */
    scr_files = lv_obj_create(NULL);
    lv_obj_set_size(scr_files, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(scr_files, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr_files, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr_files, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Top bar ─────────────────────────────────────────────── */
    topbar = lv_obj_create(scr_files);
    lv_obj_set_size(topbar, SCREEN_W, TOPBAR_H);
    lv_obj_align(topbar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(topbar, lv_color_hex(COL_TOPBAR), 0);
    lv_obj_set_style_bg_opa(topbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(topbar, 0, 0);
    lv_obj_set_style_radius(topbar, 0, 0);
    lv_obj_set_style_pad_left(topbar, 8, 0);
    lv_obj_set_style_pad_right(topbar, 8, 0);
    lv_obj_clear_flag(topbar, LV_OBJ_FLAG_SCROLLABLE);

    /* Back button */
    lv_obj_t *btn_back = lv_button_create(topbar);
    lv_obj_set_size(btn_back, 56, 48);
    lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_opa(btn_back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(btn_back, 0, 0);

    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(lbl_back, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(lbl_back, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl_back);

    lv_obj_add_event_cb(btn_back, cb_back_btn, LV_EVENT_CLICKED, NULL);

    /* Path label */
    lbl_path = lv_label_create(topbar);
    lv_label_set_text(lbl_path, current_path);
    lv_label_set_long_mode(lbl_path, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_path, SCREEN_W - 80);
    lv_obj_set_style_text_color(lbl_path, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(lbl_path, &lv_font_montserrat_18, 0);
    lv_obj_align(lbl_path, LV_ALIGN_LEFT_MID, 52, 0);

    /* ── File list (scrollable) ──────────────────────────────── */
    file_list = lv_obj_create(scr_files);
    lv_obj_set_size(file_list, SCREEN_W, LIST_H);
    lv_obj_align(file_list, LV_ALIGN_TOP_LEFT, 0, TOPBAR_H);
    lv_obj_set_style_bg_color(file_list, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(file_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(file_list, 0, 0);
    lv_obj_set_style_radius(file_list, 0, 0);
    lv_obj_set_style_pad_all(file_list, 0, 0);
    lv_obj_set_style_pad_row(file_list, 0, 0);
    lv_obj_set_flex_flow(file_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(file_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(file_list, LV_DIR_VER);

    /* ── Bottom bar ──────────────────────────────────────────── */
    bottombar = lv_obj_create(scr_files);
    lv_obj_set_size(bottombar, SCREEN_W, BOTTOMBAR_H);
    lv_obj_align(bottombar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bottombar, lv_color_hex(COL_TOPBAR), 0);
    lv_obj_set_style_bg_opa(bottombar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bottombar, 0, 0);
    lv_obj_set_style_radius(bottombar, 0, 0);
    lv_obj_clear_flag(bottombar, LV_OBJ_FLAG_SCROLLABLE);

    lbl_storage = lv_label_create(bottombar);
    lv_label_set_text(lbl_storage, "");
    lv_obj_set_style_text_color(lbl_storage, lv_color_hex(COL_GRAY), 0);
    lv_obj_set_style_text_font(lbl_storage, &lv_font_montserrat_16, 0);
    lv_obj_center(lbl_storage);

    /* ── Check SD and populate ───────────────────────────────── */
    if (!tab5_sdcard_mounted()) {
        show_no_sd();
    } else {
        rebuild_list();
    }

    /* ── Load the screen ─────────────────────────────────────── */
    lv_screen_load(scr_files);
    UI_LOGI(TAG, "File browser created");

    return scr_files;
}

/* ================================================================
 * ui_files_destroy
 * ================================================================ */
void ui_files_destroy(void)
{
    if (scr_files) {
        lv_obj_delete(scr_files);
        scr_files   = NULL;
        topbar      = NULL;
        lbl_path    = NULL;
        file_list   = NULL;
        bottombar   = NULL;
        lbl_storage = NULL;
        no_sd_panel = NULL;
        img_preview = NULL;
        UI_LOGI(TAG, "File browser destroyed");
    }
    if (entries) {
        UI_FREE(entries);
        entries     = NULL;
        entry_count = 0;
    }
}
