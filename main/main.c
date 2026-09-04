#include <stdio.h>
#include <string.h>

#include "app_console.h"
#include "app_http.h"
#include "app_status.h"
#include "cli.h"
#include "cli_web.h"
#include "config_reader.h"
#include "diag.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "http_server.h"
#include "mdns_manager.h"
#include "mqtt_log_sink.h"
#include "mqtt_manager.h"
#include "net_events.h"
#include "nvs_flash.h"
#include "ntp_manager.h"
#include "ota.h"
#include "ota_http.h"
#include "wifi_manager.h"

static const char *TAG = "main";

#define STATUS_INTERVAL_MS 60000

static app_config_full_t s_config;

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* A partition table change or a new NVS format: erase rather than refuse
         * to boot, since nothing here is authoritative -- WiFi credentials live
         * in config.json. */
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

void app_main(void)
{
    /*
     * Bring-up order, and why.
     *
     * Nothing below waits for the network. Every service that needs connectivity
     * subscribes to NET_EVENT and starts itself when there is a link, so this
     * function is a list of "set this up", not a sequence of "wait for that".
     * The only ordering that matters is: output first so failures are visible,
     * configuration next because everything else is configured from it.
     */

    /* 1. Output. First, so anything that fails after this says so on the console
     *    and, later, on the web console and MQTT. */
    ESP_ERROR_CHECK(diag_init(NULL));

    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 2. Configuration. A missing or unreadable file is not fatal: schema
     *    defaults leave the device on its own access point with the console
     *    running, which is what a freshly flashed unit needs to be reconfigured. */
    char err_msg[192] = "";
    esp_err_t cfg_err = config_reader_load(&s_config, err_msg, sizeof(err_msg));
    if (cfg_err != ESP_OK) {
        ESP_LOGE(TAG, "configuration: %s", err_msg);
        ESP_LOGE(TAG, "continuing with defaults; fix it over the console or /api/config");
    }

    /* 3. The shell, before the network. A device that never joins anything must
     *    still be reachable over the serial port. */
    ESP_ERROR_CHECK(app_console_register());
    ESP_ERROR_CHECK(cli_start(NULL));

    /* 4. Services. Each subscribes to NET_EVENT and does nothing until there is
     *    a link -- registration order does not matter and none of them blocks. */
    const mdns_manager_config_t mdns_cfg = {
        .hostname = s_config.top.device_name,
        .instance_name = s_config.top.instance_name,
    };
    ESP_ERROR_CHECK(mdns_manager_start(&mdns_cfg));
    ESP_ERROR_CHECK(ntp_manager_start(&s_config.ntp));

    if (s_config.mqtt.uri[0] != '\0') {
        ESP_ERROR_CHECK(mqtt_manager_start(&s_config.mqtt, s_config.top.device_name));
        /* Send the log to MQTT as well as the console. Dropped lines are counted,
         * not lost silently -- see mqtt_log_sink_dropped(). */
        mqtt_log_sink_start("log", 0);
    } else {
        ESP_LOGW(TAG, "no MQTT broker configured");
    }

    /* 5. HTTP, its routes, and the OTA endpoint. Starting the server before
     *    registering routes would work too -- routes added later are registered
     *    as they arrive -- but registering first means they are all live the
     *    instant it listens. */
    ESP_ERROR_CHECK(app_http_register());
    ESP_ERROR_CHECK(ota_http_register(NULL));
    esp_err_t http_err = http_server_start(&s_config.http);
    if (http_err != ESP_OK) {
        /* Most likely HTTP_SERVER_ERR_NO_PASSWORD. Not fatal: the console still
         * works, and refusing to serve is the correct response to being asked to
         * serve an unauthenticated management interface. */
        ESP_LOGE(TAG, "HTTP server not started: %s", esp_err_to_name(http_err));
    } else {
        /* One server, not two: the web console shares it. */
        const cli_web_config_t web = {
            .server = http_server_handle(),
            .page_uri = "/",
            .ws_uri = "/ws",
            .serve_page = true,
        };
        ESP_ERROR_CHECK(cli_web_start(&web));

        static const mdns_manager_service_t http_service = {
            .type = "_http", .proto = "_tcp", .port = 80,
        };
        mdns_manager_add_service(&http_service);
    }

    /* 6. WiFi last. It is what makes NET_EVENT_LINK_UP happen, so everything
     *    that reacts to it is already listening by the time it can fire. */
    ESP_ERROR_CHECK(wifi_manager_init(&s_config.wifi));
    ESP_ERROR_CHECK(wifi_manager_start_station_mode());
    wifi_manager_scan_and_connect();

    ESP_ERROR_CHECK(app_status_start(STATUS_INTERVAL_MS));

    /*
     * 7. Confirm the image, if this boot is an update on trial.
     *
     * With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, firmware that boots and never
     * confirms itself is rolled back on the next reset. Doing it here, after
     * everything has been brought up, is the point: an image that crashes during
     * start-up never reaches this line and undoes itself. Move it earlier and the
     * rollback mechanism stops meaning anything.
     *
     * A device that must prove more than "it started" -- that it reached the
     * broker, say -- should confirm from there instead, or leave it to `sys confirm`.
     */
    if (ota_pending_verify()) {
        ESP_LOGW(TAG, "this image is on trial; confirming it started cleanly");
        ota_mark_valid();
    }

    ESP_LOGI(TAG, "ready");
}
