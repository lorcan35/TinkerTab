/**
 * TinkerTab Service Registry — Lifecycle management (closes #21)
 *
 * Tracks state for all subsystems and enforces init→start→stop ordering.
 */

#include "service_registry.h"
#include "esp_log.h"

static const char *TAG = "svc_reg";

/* Forward declarations — each service file provides these */
extern esp_err_t storage_service_init(void);
extern esp_err_t storage_service_start(void);
extern esp_err_t storage_service_stop(void);

extern esp_err_t display_service_init(void);
extern esp_err_t display_service_start(void);
extern esp_err_t display_service_stop(void);

extern esp_err_t audio_service_init(void);
extern esp_err_t audio_service_start(void);
extern esp_err_t audio_service_stop(void);

extern esp_err_t network_service_init(void);
extern esp_err_t network_service_start(void);
extern esp_err_t network_service_stop(void);

extern esp_err_t dragon_service_init(void);
extern esp_err_t dragon_service_start(void);
extern esp_err_t dragon_service_stop(void);

static tab5_service_t s_services[SERVICE_MAX];

void tab5_services_register_all(void)
{
    s_services[SERVICE_STORAGE] = (tab5_service_t){
        .name  = "storage",
        .init  = storage_service_init,
        .start = storage_service_start,
        .stop  = storage_service_stop,
        .state = SERVICE_STATE_NONE,
    };
    s_services[SERVICE_DISPLAY] = (tab5_service_t){
        .name  = "display",
        .init  = display_service_init,
        .start = display_service_start,
        .stop  = display_service_stop,
        .state = SERVICE_STATE_NONE,
    };
    s_services[SERVICE_AUDIO] = (tab5_service_t){
        .name  = "audio",
        .init  = audio_service_init,
        .start = audio_service_start,
        .stop  = audio_service_stop,
        .state = SERVICE_STATE_NONE,
    };
    s_services[SERVICE_NETWORK] = (tab5_service_t){
        .name  = "network",
        .init  = network_service_init,
        .start = network_service_start,
        .stop  = network_service_stop,
        .state = SERVICE_STATE_NONE,
    };
    s_services[SERVICE_DRAGON] = (tab5_service_t){
        .name  = "dragon",
        .init  = dragon_service_init,
        .start = dragon_service_start,
        .stop  = dragon_service_stop,
        .state = SERVICE_STATE_NONE,
    };

    ESP_LOGI(TAG, "Registered %d services", SERVICE_MAX);
}

esp_err_t tab5_services_init_all(void)
{
    ESP_LOGI(TAG, "Initializing all services...");

    for (int i = 0; i < SERVICE_MAX; i++) {
        tab5_service_t *svc = &s_services[i];
        if (!svc->init) {
            ESP_LOGW(TAG, "[%s] no init function, skipping", svc->name);
            continue;
        }
        ESP_LOGI(TAG, "[%s] init...", svc->name);
        esp_err_t ret = svc->init();
        if (ret == ESP_OK) {
            svc->state = SERVICE_STATE_INITIALIZED;
            ESP_LOGI(TAG, "[%s] init OK", svc->name);
        } else {
            svc->state = SERVICE_STATE_ERROR;
            ESP_LOGE(TAG, "[%s] init FAILED: %s", svc->name, esp_err_to_name(ret));
        }
    }

    return ESP_OK;
}

esp_err_t tab5_services_start(tab5_service_id_t id)
{
    if (id >= SERVICE_MAX) return ESP_ERR_INVALID_ARG;
    tab5_service_t *svc = &s_services[id];

    if (svc->state == SERVICE_STATE_RUNNING) {
        return ESP_OK; /* Already running */
    }
    if (svc->state != SERVICE_STATE_INITIALIZED && svc->state != SERVICE_STATE_STOPPED) {
        ESP_LOGW(TAG, "[%s] cannot start from state %d", svc->name, svc->state);
        return ESP_ERR_INVALID_STATE;
    }
    if (!svc->start) {
        ESP_LOGW(TAG, "[%s] no start function", svc->name);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG, "[%s] start...", svc->name);
    esp_err_t ret = svc->start();
    if (ret == ESP_OK) {
        svc->state = SERVICE_STATE_RUNNING;
        ESP_LOGI(TAG, "[%s] running", svc->name);
    } else {
        svc->state = SERVICE_STATE_ERROR;
        ESP_LOGE(TAG, "[%s] start FAILED: %s", svc->name, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t tab5_services_stop(tab5_service_id_t id)
{
    if (id >= SERVICE_MAX) return ESP_ERR_INVALID_ARG;
    tab5_service_t *svc = &s_services[id];

    if (svc->state == SERVICE_STATE_STOPPED || svc->state == SERVICE_STATE_NONE) {
        return ESP_OK; /* Already stopped or never started */
    }
    if (!svc->stop) {
        ESP_LOGW(TAG, "[%s] no stop function", svc->name);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG, "[%s] stop...", svc->name);
    esp_err_t ret = svc->stop();
    if (ret == ESP_OK) {
        svc->state = SERVICE_STATE_STOPPED;
    } else {
        svc->state = SERVICE_STATE_ERROR;
        ESP_LOGE(TAG, "[%s] stop FAILED: %s", svc->name, esp_err_to_name(ret));
    }
    return ret;
}

tab5_service_state_t tab5_services_get_state(tab5_service_id_t id)
{
    if (id >= SERVICE_MAX) return SERVICE_STATE_NONE;
    return s_services[id].state;
}

const char *tab5_services_get_name(tab5_service_id_t id)
{
    if (id >= SERVICE_MAX) return "unknown";
    return s_services[id].name;
}

bool tab5_services_is_running(tab5_service_id_t id)
{
    if (id >= SERVICE_MAX) return false;
    return s_services[id].state == SERVICE_STATE_RUNNING;
}

void tab5_services_print_status(void)
{
    static const char *state_names[] = {
        [SERVICE_STATE_NONE]        = "NONE",
        [SERVICE_STATE_INITIALIZED] = "INIT",
        [SERVICE_STATE_RUNNING]     = "RUN",
        [SERVICE_STATE_STOPPED]     = "STOP",
        [SERVICE_STATE_ERROR]       = "ERR",
    };

    printf("=== Service Status ===\n");
    for (int i = 0; i < SERVICE_MAX; i++) {
        tab5_service_t *svc = &s_services[i];
        const char *st = (svc->state <= SERVICE_STATE_ERROR) ? state_names[svc->state] : "?";
        printf("  %-10s %s\n", svc->name, st);
    }
}
