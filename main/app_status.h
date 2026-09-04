#pragma once

#include <stddef.h>

#include "esp_err.h"

/*
 * The device's status message.
 *
 * This belongs to the project, not to a component. What a device reports is the
 * one thing no library can decide: a power monitor reports relay states, a
 * camera reports frames captured. mqtt_manager publishes bytes and has no
 * opinion about their shape.
 *
 * Replace the fields below with whatever this device actually measures. The
 * plumbing -- the timer, the MQTT topic, the HTTP route -- stays as it is.
 */

/* Serialise the current status as JSON. Returns the length written, or -1 if it
 * would not fit. */
int app_status_serialize(char *buf, size_t buf_size);

/* Publish the status to MQTT, if connected. Called on a timer and whenever
 * something changes. */
esp_err_t app_status_publish(void);

/* Start the periodic publish. */
esp_err_t app_status_start(uint32_t interval_ms);
