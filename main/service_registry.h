/**
 * TinkerTab Service Registry — Lifecycle management for all subsystems
 *
 * Each service has init/start/stop/deinit lifecycle. The registry
 * tracks state and enforces ordering.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

typedef enum {
    SERVICE_STORAGE,
    SERVICE_DISPLAY,
    SERVICE_AUDIO,
    SERVICE_NETWORK,
    SERVICE_DRAGON,
    SERVICE_MAX
} tab5_service_id_t;

typedef enum {
    SERVICE_STATE_NONE,        // Not initialized
    SERVICE_STATE_INITIALIZED, // init() called, not running
    SERVICE_STATE_RUNNING,     // start() called, active
    SERVICE_STATE_STOPPED,     // stop() called, can restart
    SERVICE_STATE_ERROR,       // Failed, needs recovery
} tab5_service_state_t;

typedef struct {
    const char *name;
    esp_err_t (*init)(void);
    esp_err_t (*start)(void);
    esp_err_t (*stop)(void);
    tab5_service_state_t state;
} tab5_service_t;

/** Register all services. Call once at boot before anything else. */
void tab5_services_register_all(void);

/** Init all registered services in order. */
esp_err_t tab5_services_init_all(void);

/** Start a specific service. Must be initialized first. */
esp_err_t tab5_services_start(tab5_service_id_t id);

/** Stop a specific service. Idempotent. */
esp_err_t tab5_services_stop(tab5_service_id_t id);

/** Get service state. */
tab5_service_state_t tab5_services_get_state(tab5_service_id_t id);

/** Get service name string. */
const char *tab5_services_get_name(tab5_service_id_t id);

/** Check if service is running. */
bool tab5_services_is_running(tab5_service_id_t id);

/** Print all service states to console. */
void tab5_services_print_status(void);
