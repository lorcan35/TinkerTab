/*
 * stubs.c — Hardware stub implementations for ESP-IDF Linux target.
 *
 * Every hardware function returns a safe default so main.c and the
 * service layer boot successfully without real hardware.
 *
 * The hal_linux component is only compiled when IDF_TARGET == linux.
 * On esp32p4 the real drivers in main/ are used instead.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* IDF types available on linux target */
#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2c_master.h"

/* Our hardware headers (accessed via PRIV_INCLUDE_DIRS in CMakeLists.txt) */
#include "io_expander.h"
#include "display.h"
#include "touch.h"
#include "camera.h"
#include "audio.h"   /* also declares tab5_mic_* */
#include "imu.h"
#include "rtc.h"
#include "battery.h"
#include "bluetooth.h"
#include "wifi.h"
#include "sdcard.h"
#include "dragon_link.h"
#include "mjpeg_stream.h"
#include "udp_stream.h"
#include "touch_ws.h"
#include "mdns_discovery.h"
#include "mode_manager.h"
#include "service_registry.h"
#include "debug_server.h"
#include "voice.h"

static const char *TAG = "hal_linux";

/* ── esp_timer ──────────────────────────────────────────────────────── */
#include "esp_timer.h"
#include <time.h>
int64_t esp_timer_get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}

/* ── io_expander.h ──────────────────────────────────────────────────── */
esp_err_t tab5_io_expander_init(i2c_master_bus_handle_t bus)    { (void)bus; return ESP_OK; }
esp_err_t tab5_io_expander_set_pin(uint8_t pin, bool val)       { return ESP_OK; }
esp_err_t tab5_io_expander_get_pin(uint8_t pin, bool *val)      { if (val) *val = false; return ESP_OK; }
void      tab5_reset_display_and_touch(void)                     { }
void      tab5_set_wifi_power(bool on)                           { ESP_LOGI(TAG, "WiFi power: %s (sim)", on ? "ON" : "OFF"); }

/* ── display.h ─────────────────────────────────────────────────────── */
/* display_linux.c provides the SDL2 implementations for display_init
 * and display_get_panel. The remaining functions are stubs here.       */
void      tab5_display_fill_color(uint16_t c)                    { }
esp_err_t tab5_display_set_brightness(int p)                     { return ESP_OK; }
void      tab5_display_show_status(const char *m)                { printf("[display] %s\n", m); }
void      tab5_display_test_pattern(int t)                       { }
esp_lcd_panel_handle_t tab5_display_get_panel(void)              { return NULL; }
esp_err_t tab5_display_jpeg_init(void)                           { return ESP_OK; }
esp_err_t tab5_display_draw_jpeg(const uint8_t *d, uint32_t s)  { return ESP_OK; }
void      tab5_display_set_jpeg_enabled(bool e)                  { }
bool      tab5_display_is_jpeg_enabled(void)                     { return false; }

/* ── touch.h ────────────────────────────────────────────────────────── */
esp_err_t tab5_touch_init(i2c_master_bus_handle_t i2c_bus)       { (void)i2c_bus; return ESP_OK; }
bool      tab5_touch_read(tab5_touch_point_t *pts, uint8_t *cnt) { if(cnt) *cnt=0; (void)pts; return false; }
int       tab5_touch_diag(void)                                   { return 0; }

/* ── camera.h ──────────────────────────────────────────────────────── */
esp_err_t   tab5_camera_init(void)                               { return ESP_FAIL; }
esp_err_t   tab5_camera_deinit(void)                             { return ESP_OK; }
bool        tab5_camera_initialized(void)                        { return false; }
esp_err_t   tab5_camera_set_resolution(tab5_cam_resolution_t r) { return ESP_FAIL; }
esp_err_t   tab5_camera_capture(tab5_cam_frame_t *f)             { return ESP_FAIL; }
esp_err_t   tab5_camera_save_jpeg(const tab5_cam_frame_t *f, const char *p) { return ESP_FAIL; }
const char *tab5_camera_info(void)                               { return "LinuxSim-NoCamera"; }

/* ── sdcard.h ──────────────────────────────────────────────────────── */
esp_err_t   tab5_sdcard_init(void)                               { return ESP_FAIL; }
esp_err_t   tab5_sdcard_deinit(void)                             { return ESP_OK; }
bool        tab5_sdcard_mounted(void)                            { return false; }
uint64_t    tab5_sdcard_total_bytes(void)                        { return 0; }
uint64_t    tab5_sdcard_free_bytes(void)                         { return 0; }
const char *tab5_sdcard_mount_point(void)                        { return "/sdcard"; }

/* ── wifi.h ────────────────────────────────────────────────────────── */
esp_err_t tab5_wifi_init(void)                                    { return ESP_FAIL; }
esp_err_t tab5_wifi_wait_connected(int timeout_ms)                { return ESP_FAIL; }
bool      tab5_wifi_connected(void)                               { return false; }

/* ── battery.h ─────────────────────────────────────────────────────── */
esp_err_t tab5_battery_init(i2c_master_bus_handle_t bus)          { (void)bus; return ESP_OK; }
esp_err_t tab5_battery_read(tab5_battery_info_t *i) {
    if (i) { i->voltage=7.6f; i->current=0.5f; i->power=3.8f; i->percent=72; i->charging=false; }
    return ESP_OK;
}
uint8_t   tab5_battery_percent(void)                              { return 72; }
bool      tab5_battery_charging(void)                             { return false; }

/* ── rtc.h ─────────────────────────────────────────────────────────── */
esp_err_t tab5_rtc_init(i2c_master_bus_handle_t bus)              { (void)bus; return ESP_OK; }
esp_err_t tab5_rtc_get_time(tab5_rtc_time_t *t) {
    if (t) { t->year=26; t->month=3; t->day=30; t->hour=12; t->minute=0; t->second=0; t->weekday=1; }
    return ESP_OK;
}
esp_err_t tab5_rtc_set_time(const tab5_rtc_time_t *t)            { return ESP_OK; }
esp_err_t tab5_rtc_sync_ntp(const char *s)                        { return ESP_OK; }
esp_err_t tab5_rtc_sync_from_ntp(void)                            { return ESP_OK; }

/* ── imu.h ─────────────────────────────────────────────────────────── */
esp_err_t tab5_imu_init(i2c_master_bus_handle_t bus)              { (void)bus; return ESP_OK; }
esp_err_t tab5_imu_read(tab5_imu_data_t *d) {
    if (d) { d->accel.x=0; d->accel.y=0; d->accel.z=1.0f;
             d->gyro.x=0; d->gyro.y=0; d->gyro.z=0; }
    return ESP_OK;
}
tab5_orientation_t tab5_imu_get_orientation(void)                 { return TAB5_ORIENT_PORTRAIT; }
bool tab5_imu_detect_tap(void)                                     { return false; }
bool tab5_imu_detect_shake(void)                                   { return false; }

/* ── bluetooth.h ────────────────────────────────────────────────────── */
esp_err_t tab5_bluetooth_init(void)                               { return ESP_FAIL; }

/* ── audio.h ────────────────────────────────────────────────────────── */
esp_err_t       tab5_audio_init(i2c_master_bus_handle_t bus)       { (void)bus; return ESP_OK; }
void           *tab5_audio_get_i2s_rx(void)                        { return NULL; }
const void     *tab5_audio_get_data_if(void)                       { return NULL; }
esp_err_t       tab5_audio_play_raw(const int16_t *d, size_t s)    { return ESP_OK; }
esp_err_t       tab5_audio_set_volume(uint8_t v)                   { return ESP_OK; }
uint8_t         tab5_audio_get_volume(void)                        { return 70; }
esp_err_t       tab5_audio_speaker_enable(bool e)                  { return ESP_OK; }
esp_err_t       tab5_audio_test_tone(uint32_t f, uint32_t ms)      { return ESP_OK; }
esp_err_t       tab5_mic_init(i2c_master_bus_handle_t bus)         { (void)bus; return ESP_OK; }
esp_err_t       tab5_mic_read(int16_t *b, size_t s, uint32_t t)   { return ESP_FAIL; }
esp_err_t       tab5_mic_set_gain(uint8_t g)                       { return ESP_OK; }
void            tab5_mic_diag(void)                                 { }

/* ── dragon_link.h ──────────────────────────────────────────────────── */
esp_err_t       tab5_dragon_link_init(void)                        { return ESP_OK; }
dragon_state_t  tab5_dragon_get_state(void)                        { return DRAGON_STATE_OFFLINE; }
const char     *tab5_dragon_state_str(void)                        { return "Offline (Linux)"; }
float           tab5_dragon_get_fps(void)                          { return 0.0f; }
void            tab5_dragon_request_stop(void)                     { }
void            tab5_dragon_request_start(void)                    { }
bool            tab5_dragon_is_streaming(void)                     { return false; }

/* ── mode_manager.h ─────────────────────────────────────────────────── */
void          tab5_mode_init(void)                                  { }
tab5_mode_t   tab5_mode_get(void)                                  { return MODE_IDLE; }
const char   *tab5_mode_str(void)                                  { return "Idle"; }
esp_err_t     tab5_mode_switch(tab5_mode_t m)                      { return ESP_OK; }

/* ── service_registry.h ─────────────────────────────────────────────── */
void                 tab5_services_register_all(void)              { }
esp_err_t            tab5_services_init_all(void)                  { return ESP_OK; }
esp_err_t            tab5_services_start(tab5_service_id_t id)     { return ESP_OK; }
esp_err_t            tab5_services_stop(tab5_service_id_t id)      { return ESP_OK; }
tab5_service_state_t tab5_services_get_state(tab5_service_id_t id) { return SERVICE_STATE_RUNNING; }
const char          *tab5_services_get_name(tab5_service_id_t id)  { return "LinuxService"; }
bool                 tab5_services_is_running(tab5_service_id_t id){ return true; }
void                 tab5_services_print_status(void)              { }

/* ── mjpeg_stream / udp_stream / touch_ws / mdns ────────────────────── */
esp_err_t tab5_mjpeg_stream_start(void)   { return ESP_OK; }
esp_err_t tab5_mjpeg_stream_stop(void)    { return ESP_OK; }
bool      tab5_mjpeg_stream_running(void) { return false; }
esp_err_t tab5_udp_stream_start(void)     { return ESP_OK; }
esp_err_t tab5_udp_stream_stop(void)      { return ESP_OK; }
void tab5_touch_ws_start(void)            { }
void tab5_touch_ws_stop(void)             { }
void tab5_touch_ws_send(const tab5_touch_point_t *pts, uint8_t count) { (void)pts; (void)count; }
void tab5_touch_ws_send_release(void)     { }
bool tab5_touch_ws_connected(void)        { return false; }
void tab5_touch_ws_set_disconnect_cb(touch_ws_disconnect_cb_t cb) { (void)cb; }
esp_err_t tab5_mdns_init(void)            { return ESP_OK; }

/* ── debug_server.h ─────────────────────────────────────────────────── */
esp_err_t tab5_debug_server_init(void)    { return ESP_OK; }
bool tab5_debug_touch_override(int32_t *x, int32_t *y, bool *pressed) {
    (void)x; (void)y; (void)pressed;
    return false;
}

/* ── voice.h ────────────────────────────────────────────────────────── */
esp_err_t    voice_init(voice_state_cb_t cb)                       { return ESP_OK; }
esp_err_t    voice_connect(const char *h, uint16_t p)              { return ESP_FAIL; }
esp_err_t    voice_connect_async(const char *h, uint16_t p, bool l){ return ESP_OK; }
esp_err_t    voice_start_listening(void)                           { return ESP_FAIL; }
esp_err_t    voice_stop_listening(void)                            { return ESP_OK; }
esp_err_t    voice_cancel(void)                                    { return ESP_OK; }
esp_err_t    voice_disconnect(void)                                { return ESP_OK; }
voice_state_t voice_get_state(void)                                { return VOICE_STATE_IDLE; }
const char  *voice_get_last_transcript(void)                       { return ""; }
const char  *voice_get_stt_text(void)                             { return ""; }
const char  *voice_get_llm_text(void)                             { return ""; }

/* ── settings.h ─────────────────────────────────────────────────────── */
#include "settings.h"
static char s_ssid[64]  = "LinuxSim";
static char s_pass[64]  = "password";
static char s_host[128] = "192.168.1.89";
static uint16_t s_port  = 3001;
static uint8_t  s_bright = 80;
static uint8_t  s_vol    = 70;

esp_err_t tab5_settings_init(void)                                 { return ESP_OK; }
esp_err_t tab5_settings_get_wifi_ssid(char *b, size_t l)          { strncpy(b, s_ssid, l); return ESP_OK; }
esp_err_t tab5_settings_set_wifi_ssid(const char *s)              { strncpy(s_ssid, s, sizeof(s_ssid)-1); s_ssid[sizeof(s_ssid)-1]='\0'; return ESP_OK; }
esp_err_t tab5_settings_get_wifi_pass(char *b, size_t l)          { strncpy(b, s_pass, l); return ESP_OK; }
esp_err_t tab5_settings_set_wifi_pass(const char *s)              { strncpy(s_pass, s, sizeof(s_pass)-1); s_pass[sizeof(s_pass)-1]='\0'; return ESP_OK; }
esp_err_t tab5_settings_get_dragon_host(char *b, size_t l)        { strncpy(b, s_host, l); return ESP_OK; }
esp_err_t tab5_settings_set_dragon_host(const char *s)            { strncpy(s_host, s, sizeof(s_host)-1); s_host[sizeof(s_host)-1]='\0'; return ESP_OK; }
uint16_t  tab5_settings_get_dragon_port(void)                      { return s_port; }
esp_err_t tab5_settings_set_dragon_port(uint16_t p)               { s_port = p; return ESP_OK; }
uint8_t   tab5_settings_get_brightness(void)                       { return s_bright; }
esp_err_t tab5_settings_set_brightness(uint8_t v)                 { s_bright = v; return ESP_OK; }
uint8_t   tab5_settings_get_volume(void)                          { return s_vol; }
esp_err_t tab5_settings_set_volume(uint8_t v)                     { s_vol = v; return ESP_OK; }
esp_err_t tab5_settings_get_device_id(char *b, size_t l)          { strncpy(b, "linux000000000000", l); return ESP_OK; }
esp_err_t tab5_settings_get_hardware_id(char *b, size_t l)        { strncpy(b, "00:00:00:00:00:00", l); return ESP_OK; }
esp_err_t tab5_settings_get_session_id(char *b, size_t l)         { if(l>0) b[0]='\0'; return ESP_OK; }
esp_err_t tab5_settings_set_session_id(const char *s)             { return ESP_OK; }
