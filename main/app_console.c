#include "app_console.h"

#include <stdio.h>
#include <string.h>

#include "app_status.h"
#include "cli.h"
#include "config_reader.h"
#include "diag.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "mqtt_manager.h"
#include "ota.h"
#include "wifi_manager.h"

/*
 * Command groups are two tokens: "<group> <command>". A command receives
 * argv[0] == its own name, so `sys info` arrives here as argc=1, argv={"info"}.
 *
 * Never printf() from a command -- diag_printf() reaches the serial port and the
 * web console alike. Report failures with diag_error(), which prefixes "ERR: " so
 * a host script can tell success from failure without parsing prose, and return
 * -1; return 0 on success.
 */

static int cmd_sys_info(int argc, char **argv)
{
    (void)argc; (void)argv;
    const esp_app_desc_t *app = esp_app_get_description();

    diag_printf("firmware:  %s %s\n", app->project_name, app->version);
    diag_printf("built:     %s %s\n", app->date, app->time);
    diag_printf("idf:       %s\n", app->idf_ver);
    diag_printf("heap free: %u (min %u)\n",
                (unsigned)esp_get_free_heap_size(),
                (unsigned)esp_get_minimum_free_heap_size());
    diag_printf("config:    %s\n", config_reader_source());
    return 0;
}

static int cmd_sys_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    char buf[512];
    if (app_status_serialize(buf, sizeof(buf)) < 0) {
        diag_error("status does not fit");
        return -1;
    }
    diag_printf("%s\n", buf);
    return 0;
}

static int cmd_sys_restart(int argc, char **argv)
{
    (void)argc; (void)argv;
    diag_printf("restarting\n");
    fflush(stdout);
    esp_restart();
    return 0;
}

static int cmd_sys_confirm(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!ota_pending_verify()) {
        diag_printf("nothing to confirm; this image is already marked good\n");
        return 0;
    }
    esp_err_t err = ota_mark_valid();
    if (err != ESP_OK) {
        diag_error("confirming this image: %s", esp_err_to_name(err));
        return -1;
    }
    diag_printf("image confirmed; it will not be rolled back\n");
    return 0;
}

static int cmd_sys_rollback(int argc, char **argv)
{
    (void)argc; (void)argv;
    diag_printf("rolling back to the previous firmware\n");
    fflush(stdout);
    ota_rollback_and_reboot();
    /* Only reached if there is nothing to roll back to. */
    diag_error("no previous firmware to roll back to");
    return -1;
}

static const cli_command_t sys_commands[] = {
    {"info",     NULL, "Firmware, build and heap",            cmd_sys_info},
    {"status",   NULL, "The status message, as published",    cmd_sys_status},
    {"restart",  NULL, "Restart the device",                  cmd_sys_restart},
    {"confirm",  NULL, "Mark this image good, cancelling rollback", cmd_sys_confirm},
    {"rollback", NULL, "Return to the previous firmware",     cmd_sys_rollback},
};

static const cli_group_t sys_group = {
    .name = "sys",
    .help = "Device information and control",
    .commands = sys_commands,
    .command_count = sizeof(sys_commands) / sizeof(sys_commands[0]),
};

static int cmd_net_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    char ip[16] = "";
    int8_t rssi = 0;

    wifi_manager_get_address(ip, sizeof(ip));
    wifi_manager_get_rssi(&rssi);

    diag_printf("wifi:  state %d, ip %s, rssi %d\n",
                (int)wifi_manager_get_state(), ip[0] ? ip : "-", rssi);
    diag_printf("mqtt:  %s", mqtt_manager_is_connected() ? "connected" : "disconnected");
    if (mqtt_manager_topic_prefix()) {
        diag_printf(" under %s", mqtt_manager_topic_prefix());
    }
    diag_printf("\n");
    return 0;
}

static int cmd_net_scan(int argc, char **argv)
{
    (void)argc; (void)argv;
    esp_err_t err = wifi_manager_scan_and_connect();
    if (err != ESP_OK) {
        diag_error("scan: %s", esp_err_to_name(err));
        return -1;
    }
    diag_printf("scanning; results arrive asynchronously\n");
    return 0;
}

static int cmd_net_ap(int argc, char **argv)
{
    (void)argc; (void)argv;
    esp_err_t err = wifi_manager_start_ap_mode();
    if (err != ESP_OK) {
        diag_error("starting the access point: %s", esp_err_to_name(err));
        return -1;
    }
    return 0;
}

static const cli_command_t net_commands[] = {
    {"status", NULL, "WiFi and MQTT connection state", cmd_net_status},
    {"scan",   NULL, "Scan and join the best known network", cmd_net_scan},
    {"ap",     NULL, "Serve the configuration access point", cmd_net_ap},
};

static const cli_group_t net_group = {
    .name = "net",
    .help = "Network state and control",
    .commands = net_commands,
    .command_count = sizeof(net_commands) / sizeof(net_commands[0]),
};

esp_err_t app_console_register(void)
{
    esp_err_t err = cli_register_group(&sys_group);
    if (err != ESP_OK) {
        return err;
    }
    return cli_register_group(&net_group);
}
