#include "app_status.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mqtt_manager.h"
#include "ntp_manager.h"
#include "wifi_manager.h"

static const char *TAG = "status";

#define STATUS_TOPIC "status"
#define STATUS_BUF   512

static esp_timer_handle_t s_timer;

int app_status_serialize(char *buf, size_t buf_size)
{
    const esp_app_desc_t *app = esp_app_get_description();

    char ip[16] = "";
    wifi_manager_get_address(ip, sizeof(ip));

    int8_t rssi = 0;
    wifi_manager_get_rssi(&rssi);

    /*
     * A flat object of scalars, which is the shape that survives being graphed,
     * alerted on and diffed. Add what this device measures; keep it flat.
     */
    int n = snprintf(buf, buf_size,
        "{"
        "\"firmware\":\"%s\","
        "\"version\":\"%s\","
        "\"uptime_s\":%llu,"
        "\"heap_free\":%u,"
        "\"heap_min\":%u,"
        "\"ip\":\"%s\","
        "\"rssi\":%d,"
        "\"mqtt\":%s,"
        "\"time_synced\":%s"
        "}",
        app ? app->project_name : "?",
        app ? app->version : "?",
        (unsigned long long)(esp_timer_get_time() / 1000000),
        (unsigned)esp_get_free_heap_size(),
        (unsigned)esp_get_minimum_free_heap_size(),
        ip,
        rssi,
        mqtt_manager_is_connected() ? "true" : "false",
        ntp_manager_is_synced() ? "true" : "false");

    if (n < 0 || (size_t)n >= buf_size) {
        return -1;
    }
    return n;
}

esp_err_t app_status_publish(void)
{
    static char buf[STATUS_BUF];

    int len = app_status_serialize(buf, sizeof(buf));
    if (len < 0) {
        ESP_LOGW(TAG, "status does not fit in %d bytes", STATUS_BUF);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Retained, so a subscriber that connects later sees the last state rather
     * than nothing until the next interval. */
    return mqtt_manager_publish(STATUS_TOPIC, buf, (size_t)len, 0, true);
}

static void on_timer(void *arg)
{
    (void)arg;
    /* Not connected is the normal case for most of a boot, so it is not worth
     * logging; publish() reports it and the next tick tries again. */
    app_status_publish();
}

esp_err_t app_status_start(uint32_t interval_ms)
{
    if (s_timer) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_timer_create_args_t args = {
        .callback = on_timer,
        .name = "status",
    };
    esp_err_t err = esp_timer_create(&args, &s_timer);
    if (err != ESP_OK) {
        return err;
    }
    return esp_timer_start_periodic(s_timer, (uint64_t)interval_ms * 1000);
}
