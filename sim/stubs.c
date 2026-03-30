/*
 * stubs.c — Hardware stub implementations for desktop simulator
 *
 * Every hardware function returns a safe default: OK / false / 0 / empty string.
 * The simulator UI works without any real hardware.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- esp_err.h types ---- */
typedef int esp_err_t;
#define ESP_OK  0
#define ESP_FAIL -1

/* ── camera.h stubs ──────────────────────────────────────────────── */
typedef enum { TAB5_CAM_FMT_JPEG, TAB5_CAM_FMT_RGB565, TAB5_CAM_FMT_YUV422 } tab5_cam_format_t;
typedef enum { TAB5_CAM_RES_QVGA, TAB5_CAM_RES_VGA, TAB5_CAM_RES_HD, TAB5_CAM_RES_FULL } tab5_cam_resolution_t;
typedef struct { uint8_t *data; uint32_t size; uint16_t width; uint16_t height; tab5_cam_format_t format; } tab5_cam_frame_t;

esp_err_t tab5_camera_init(void)   { return ESP_FAIL; }
esp_err_t tab5_camera_deinit(void) { return ESP_OK; }
bool      tab5_camera_initialized(void) { return false; }
esp_err_t tab5_camera_set_resolution(tab5_cam_resolution_t r) { return ESP_FAIL; }
esp_err_t tab5_camera_capture(tab5_cam_frame_t *f) { return ESP_FAIL; }
esp_err_t tab5_camera_save_jpeg(const tab5_cam_frame_t *f, const char *p) { return ESP_FAIL; }
const char *tab5_camera_info(void) { return "SimCamera (stub)"; }

/* ── sdcard.h stubs ──────────────────────────────────────────────── */
esp_err_t   tab5_sdcard_init(void)          { return ESP_FAIL; }
esp_err_t   tab5_sdcard_deinit(void)        { return ESP_OK; }
bool        tab5_sdcard_mounted(void)        { return false; }
uint64_t    tab5_sdcard_total_bytes(void)    { return 0; }
uint64_t    tab5_sdcard_free_bytes(void)     { return 0; }
const char *tab5_sdcard_mount_point(void)    { return "/sdcard"; }

/* ── wifi.h stubs ────────────────────────────────────────────────── */
esp_err_t tab5_wifi_init(void)                    { return ESP_FAIL; }
esp_err_t tab5_wifi_wait_connected(int timeout_ms) { return ESP_FAIL; }
bool      tab5_wifi_connected(void)                { return false; }

/* ── battery.h stubs ─────────────────────────────────────────────── */
typedef struct { float voltage; float current; float power; uint8_t percent; bool charging; } tab5_battery_info_t;
esp_err_t tab5_battery_init(void *bus)      { return ESP_OK; }
esp_err_t tab5_battery_read(tab5_battery_info_t *i) {
    if (i) { i->voltage = 7.6f; i->current = 0.5f; i->power = 3.8f; i->percent = 72; i->charging = false; }
    return ESP_OK;
}
uint8_t tab5_battery_percent(void)  { return 72; }
bool    tab5_battery_charging(void) { return false; }

/* ── rtc.h stubs ─────────────────────────────────────────────────── */
typedef struct { uint8_t year, month, day, hour, minute, second, weekday; } tab5_rtc_time_t;
esp_err_t tab5_rtc_init(void *bus) { return ESP_OK; }
esp_err_t tab5_rtc_get_time(tab5_rtc_time_t *t) {
    if (t) { t->year=26; t->month=3; t->day=30; t->hour=12; t->minute=0; t->second=0; t->weekday=1; }
    return ESP_OK;
}
esp_err_t tab5_rtc_set_time(const tab5_rtc_time_t *t) { return ESP_OK; }
esp_err_t tab5_rtc_sync_ntp(const char *s) { return ESP_OK; }

/* ── imu.h stubs ─────────────────────────────────────────────────── */
typedef struct { float x, y, z; } tab5_imu_vec_t;
typedef struct { tab5_imu_vec_t accel; tab5_imu_vec_t gyro; float temp_c; } tab5_imu_data_t;
esp_err_t tab5_imu_init(void *bus) { return ESP_OK; }
esp_err_t tab5_imu_read(tab5_imu_data_t *d) {
    if (d) { d->accel.x=0; d->accel.y=0; d->accel.z=1.0f; d->gyro.x=0; d->gyro.y=0; d->gyro.z=0; d->temp_c=25.0f; }
    return ESP_OK;
}

/* ── bluetooth.h stubs ───────────────────────────────────────────── */
esp_err_t tab5_bluetooth_init(void) { return -106; /* NOT_SUPPORTED */ }

/* ── audio.h stubs ───────────────────────────────────────────────── */
esp_err_t tab5_audio_init(void *bus)          { return ESP_OK; }
void     *tab5_audio_get_i2s_rx(void)         { return NULL; }
const void *tab5_audio_get_data_if(void)      { return NULL; }
esp_err_t tab5_audio_play_raw(const int16_t *d, size_t s) { return ESP_OK; }
esp_err_t tab5_audio_set_volume(uint8_t v)    { return ESP_OK; }
uint8_t   tab5_audio_get_volume(void)          { return 70; }
esp_err_t tab5_audio_speaker_enable(bool e)   { return ESP_OK; }
esp_err_t tab5_audio_test_tone(uint32_t f, uint32_t ms) { return ESP_OK; }
esp_err_t tab5_mic_init(void *bus)            { return ESP_OK; }
esp_err_t tab5_mic_read(int16_t *b, size_t s, uint32_t t) { return ESP_FAIL; }
esp_err_t tab5_mic_set_gain(uint8_t g)        { return ESP_OK; }
void      tab5_mic_diag(void)                  { }

/* ── display.h stubs ─────────────────────────────────────────────── */
esp_err_t tab5_display_init(void)             { return ESP_OK; }
void      tab5_display_fill_color(uint16_t c) { }
esp_err_t tab5_display_set_brightness(int p)  { return ESP_OK; }
void      tab5_display_show_status(const char *m) { printf("[display] %s\n", m); }
void      tab5_display_test_pattern(int t)    { }
void     *tab5_display_get_panel(void)        { return NULL; }
esp_err_t tab5_display_jpeg_init(void)        { return ESP_OK; }
esp_err_t tab5_display_draw_jpeg(const uint8_t *d, uint32_t s) { return ESP_OK; }
void      tab5_display_set_jpeg_enabled(bool e) { }
bool      tab5_display_is_jpeg_enabled(void)   { return false; }

/* ── dragon_link.h stubs ─────────────────────────────────────────── */
typedef enum { DRAGON_STATE_IDLE, DRAGON_STATE_DISCOVERING, DRAGON_STATE_HANDSHAKE,
               DRAGON_STATE_CONNECTED, DRAGON_STATE_STREAMING, DRAGON_STATE_RECONNECTING,
               DRAGON_STATE_OFFLINE } dragon_state_t;
esp_err_t     tab5_dragon_link_init(void)    { return ESP_OK; }
dragon_state_t tab5_dragon_get_state(void)   { return DRAGON_STATE_OFFLINE; }
const char   *tab5_dragon_state_str(void)    { return "Offline (Sim)"; }
float         tab5_dragon_get_fps(void)      { return 0.0f; }
void          tab5_dragon_request_stop(void) { }
void          tab5_dragon_request_start(void){ }
bool          tab5_dragon_is_streaming(void) { return false; }

/* ── voice.h stubs ───────────────────────────────────────────────── */
typedef enum { VOICE_STATE_IDLE, VOICE_STATE_CONNECTING, VOICE_STATE_READY,
               VOICE_STATE_LISTENING, VOICE_STATE_PROCESSING, VOICE_STATE_SPEAKING } voice_state_t;
typedef void (*voice_state_cb_t)(voice_state_t, const char*);
esp_err_t    voice_init(voice_state_cb_t cb)                     { return ESP_OK; }
esp_err_t    voice_connect(const char *h, uint16_t p)            { return ESP_FAIL; }
esp_err_t    voice_connect_async(const char *h, uint16_t p, bool l) { return ESP_OK; }
esp_err_t    voice_start_listening(void)                         { return ESP_FAIL; }
esp_err_t    voice_stop_listening(void)                          { return ESP_OK; }
esp_err_t    voice_cancel(void)                                  { return ESP_OK; }
esp_err_t    voice_disconnect(void)                              { return ESP_OK; }
voice_state_t voice_get_state(void)                              { return VOICE_STATE_IDLE; }
const char  *voice_get_last_transcript(void)                     { return ""; }
const char  *voice_get_stt_text(void)                           { return ""; }
const char  *voice_get_llm_text(void)                           { return ""; }

/* ── mode_manager.h stubs ────────────────────────────────────────── */
typedef enum { MODE_IDLE, MODE_STREAMING, MODE_VOICE, MODE_BROWSING } tab5_mode_t;
void          tab5_mode_init(void)              { }
tab5_mode_t   tab5_mode_get(void)               { return MODE_IDLE; }
const char   *tab5_mode_str(void)               { return "Idle"; }
esp_err_t     tab5_mode_switch(tab5_mode_t m)   { return ESP_OK; }

/* ── settings.h stubs ────────────────────────────────────────────── */
static char s_ssid[64]   = "SimWifi";
static char s_pass[64]   = "password";
static char s_host[128]  = "192.168.1.89";
static uint16_t s_port   = 3001;
static uint8_t  s_bright = 80;
static uint8_t  s_vol    = 70;

esp_err_t tab5_settings_init(void) { return ESP_OK; }
esp_err_t tab5_settings_get_wifi_ssid(char *b, size_t l) { strncpy(b, s_ssid, l); return ESP_OK; }
esp_err_t tab5_settings_set_wifi_ssid(const char *s)     { strncpy(s_ssid, s, sizeof(s_ssid)); return ESP_OK; }
esp_err_t tab5_settings_get_wifi_pass(char *b, size_t l) { strncpy(b, s_pass, l); return ESP_OK; }
esp_err_t tab5_settings_set_wifi_pass(const char *s)     { strncpy(s_pass, s, sizeof(s_pass)); return ESP_OK; }
esp_err_t tab5_settings_get_dragon_host(char *b, size_t l){ strncpy(b, s_host, l); return ESP_OK; }
esp_err_t tab5_settings_set_dragon_host(const char *s)   { strncpy(s_host, s, sizeof(s_host)); return ESP_OK; }
uint16_t  tab5_settings_get_dragon_port(void)             { return s_port; }
esp_err_t tab5_settings_set_dragon_port(uint16_t p)       { s_port = p; return ESP_OK; }
uint8_t   tab5_settings_get_brightness(void)              { return s_bright; }
esp_err_t tab5_settings_set_brightness(uint8_t v)         { s_bright = v; return ESP_OK; }
uint8_t   tab5_settings_get_volume(void)                  { return s_vol; }
esp_err_t tab5_settings_set_volume(uint8_t v)             { s_vol = v; return ESP_OK; }
esp_err_t tab5_settings_get_device_id(char *b, size_t l)  { strncpy(b, "sim000000000000", l); return ESP_OK; }
esp_err_t tab5_settings_get_hardware_id(char *b, size_t l){ strncpy(b, "00:00:00:00:00:00", l); return ESP_OK; }
esp_err_t tab5_settings_get_session_id(char *b, size_t l) { if(l>0) b[0]='\0'; return ESP_OK; }
esp_err_t tab5_settings_set_session_id(const char *s)     { return ESP_OK; }

/* ── service_registry.h stubs ────────────────────────────────────── */
typedef enum { SERVICE_STORAGE, SERVICE_DISPLAY, SERVICE_AUDIO, SERVICE_NETWORK,
               SERVICE_DRAGON, SERVICE_MAX } tab5_service_id_t;
typedef enum { SERVICE_STATE_NONE, SERVICE_STATE_INITIALIZED, SERVICE_STATE_RUNNING,
               SERVICE_STATE_STOPPED, SERVICE_STATE_ERROR } tab5_service_state_t;
void                tab5_services_register_all(void)           { }
esp_err_t           tab5_services_init_all(void)               { return ESP_OK; }
esp_err_t           tab5_services_start(tab5_service_id_t id)  { return ESP_OK; }
esp_err_t           tab5_services_stop(tab5_service_id_t id)   { return ESP_OK; }
tab5_service_state_t tab5_services_get_state(tab5_service_id_t id) { return SERVICE_STATE_RUNNING; }
const char         *tab5_services_get_name(tab5_service_id_t id)   { return "SimService"; }
bool                tab5_services_is_running(tab5_service_id_t id) { return true; }
void                tab5_services_print_status(void)           { }

/* ── debug_server.h stubs ────────────────────────────────────────── */
esp_err_t tab5_debug_server_init(void) { return ESP_OK; }
bool tab5_debug_touch_override(int32_t *x, int32_t *y, bool *pressed) {
    (void)x; (void)y; (void)pressed;
    return false; /* SDL mouse handles touch directly */
}

/* ── imu extras ──────────────────────────────────────────────────── */
typedef enum { TAB5_IMU_ORIENT_PORTRAIT=0, TAB5_IMU_ORIENT_LANDSCAPE } tab5_imu_orientation_t;
tab5_imu_orientation_t tab5_imu_get_orientation(void) { return TAB5_IMU_ORIENT_PORTRAIT; }

/* ── rtc extras ──────────────────────────────────────────────────── */
esp_err_t tab5_rtc_sync_from_ntp(const char *s) { return ESP_OK; }

/* ── esp_wifi extras ─────────────────────────────────────────────── */
typedef struct { uint8_t ssid[33]; uint8_t bssid[6]; uint8_t channel; int8_t rssi; uint32_t authmode; } wifi_ap_record_t_ext;
esp_err_t esp_wifi_sta_get_ap_info(void *info) { return -1; }

/* ── esp_netif extras ────────────────────────────────────────────── */
void *esp_netif_get_handle_from_ifkey(const char *key) { return NULL; }

/* ── esp_event extras ────────────────────────────────────────────── */
esp_err_t esp_event_handler_instance_unregister(int base, int32_t id, void *inst) { return ESP_OK; }

/* ── mjpeg_stream / udp_stream / touch_ws / mdns stubs ──────────── */
esp_err_t tab5_mjpeg_stream_start(void)   { return ESP_OK; }
esp_err_t tab5_mjpeg_stream_stop(void)    { return ESP_OK; }
bool      tab5_mjpeg_stream_running(void) { return false; }
esp_err_t tab5_udp_stream_start(void)     { return ESP_OK; }
esp_err_t tab5_udp_stream_stop(void)      { return ESP_OK; }
esp_err_t tab5_touch_ws_start(void)       { return ESP_OK; }
esp_err_t tab5_touch_ws_stop(void)        { return ESP_OK; }
esp_err_t tab5_mdns_init(void)            { return ESP_OK; }
