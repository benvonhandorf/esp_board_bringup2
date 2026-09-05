/*
 * The wiring point: every command group in this firmware is declared here.
 *
 * Subsystem modules expose bare cmd_* functions and know nothing about the
 * shell; this file names them, groups them and registers the groups. One file
 * therefore answers "what can this device be told to do", which is worth more
 * than the coupling it costs.
 *
 * A command is addressed as exactly two tokens, "<group> <command>". There is
 * no current group and no way to stand inside one, so deeper structure is
 * spelled by hyphenating the group name: `gpio-pwm set`, `i2c-nau7802 read`.
 * That is a deliberate reversal of the nested menus this project used to have
 * -- see the note at the top of cli.h for the two failure modes navigation
 * brought with it.
 *
 * A command receives argv[0] == its own name, so `gpio set 19 true` reaches
 * cmd_gpio_set() as argc=3, argv={"set","19","true"}.
 *
 * Never printf() from a command -- diag_printf() reaches the serial port and
 * the web console alike. Report failures with diag_error(), which prefixes
 * "ERR: " so a host script can tell success from failure without parsing
 * prose, and return -1; return 0 on success.
 */
#include "app_console.h"

#include <stdio.h>
#include <string.h>

#include "app_status.h"
#include "board.h"
#include "cli.h"
#include "config_reader.h"
#include "diag.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "gpio.h"
#include "i2c.h"
#include "mqtt_manager.h"
#include "ota.h"
#include "pwm.h"
#include "sd.h"
#include "spi.h"
#include "sys_hw.h"
#include "touch.h"
#include "uart.h"
#include "wifi_manager.h"

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* ------------------------------------------------------------------ */
/* sys                                                                 */
/* ------------------------------------------------------------------ */

static int cmd_sys_info(int argc, char **argv)
{
    (void)argc; (void)argv;
    const esp_app_desc_t *app = esp_app_get_description();

    sys_hw_print_chip_info();

    diag_printf("Firmware:  %s %s\n", app->project_name, app->version);
    diag_printf("Built:     %s %s\n", app->date, app->time);
    diag_printf("ESP-IDF:   %s\n", app->idf_ver);
    diag_printf("Config:    %s\n", config_reader_source());
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

    /* Let the console -- and any attached browser -- flush before the CPU
     * restarts, or the message is lost. */
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));

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
    {"info",     "",  "Chip, memory, flash, firmware and reset reason", cmd_sys_info},
    {"status",   "",  "The status message, as published",               cmd_sys_status},
    {"lfxtal",   "",  "Configure and report the 32kHz crystal",         cmd_sys_lfxtal},
    {"restart",  "",  "Restart the device",                             cmd_sys_restart},
    {"confirm",  "",  "Mark this image good, cancelling rollback",      cmd_sys_confirm},
    {"rollback", "",  "Return to the previous firmware",                cmd_sys_rollback},
};

static const cli_group_t sys_group = {
    .name = "sys",
    .help = "Device information and control",
    .commands = sys_commands,
    .command_count = ARRAY_COUNT(sys_commands),
};

/* ------------------------------------------------------------------ */
/* net                                                                 */
/* ------------------------------------------------------------------ */

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
    {"status", "", "WiFi and MQTT connection state",       cmd_net_status},
    {"scan",   "", "Scan and join the best known network", cmd_net_scan},
    {"ap",     "", "Serve the configuration access point", cmd_net_ap},
};

static const cli_group_t net_group = {
    .name = "net",
    .help = "Configured networking: the link this device joins on its own",
    .commands = net_commands,
    .command_count = ARRAY_COUNT(net_commands),
};

/* ------------------------------------------------------------------ */
/* gpio, gpio-pwm                                                      */
/* ------------------------------------------------------------------ */

static const cli_command_t gpio_commands[] = {
    {"set",    "<pin> <state>",             "Drive pin(s) high or low",                 cmd_gpio_set},
    {"read",   "<pin> [up|down|none]",      "Read the logic level of pin(s)",           cmd_gpio_read},
    {"aread",  "<pin>",                     "Read pin(s) with the ADC",                 cmd_gpio_aread},
    {"blink",  "<pin> <count> [ms]",        "Blink pin(s) for visual identification",   cmd_gpio_blink},
    {"short",  "<pin>",                     "Find pins shorted together, adjacent pairs first", cmd_gpio_short},
    {"rc",     "<pin> [ref <pin> <kohms>]", "Measure each net's pull-up strength and capacitance", cmd_gpio_rc},
    {"survey", "[pin]",                     "Census every pin: pull-ups, driven lines, and what they suggest", cmd_gpio_survey},
};

static const cli_group_t gpio_group = {
    .name = "gpio",
    .help = "Digital and analog pin access. <pin> accepts 4, 0-5 or 1,4,8-10",
    .commands = gpio_commands,
    .command_count = ARRAY_COUNT(gpio_commands),
};

static const cli_command_t pwm_commands[] = {
    {"set",  "<pin> <freq> <duty>", "Drive a pin with PWM (duty 0-100%)", cmd_pwm_set},
    {"stop", "<pin>",               "Stop PWM and release the channel",   cmd_pwm_stop},
};

static const cli_group_t pwm_group = {
    .name = "gpio-pwm",
    .help = "Hardware PWM via LEDC",
    .commands = pwm_commands,
    .command_count = ARRAY_COUNT(pwm_commands),
};

/* ------------------------------------------------------------------ */
/* i2c                                                                 */
/* ------------------------------------------------------------------ */

static const cli_command_t i2c_commands[] = {
    {"bus",  "<scl> <sda>",         "Initialize the I2C bus on the given pins", cmd_i2c_bus},
    {"scan", "",                    "Probe the bus and tabulate responding devices", cmd_i2c_scan},
    {"read", "<address> [bytes]",   "Read bytes from a device",                 cmd_i2c_read},
};

static const cli_group_t i2c_group = {
    .name = "i2c",
    .help = "I2C master",
    .commands = i2c_commands,
    .command_count = ARRAY_COUNT(i2c_commands),
};

/* ------------------------------------------------------------------ */
/* uart                                                                */
/* ------------------------------------------------------------------ */

static const cli_command_t uart_commands[] = {
    {"init",    "<tx> <rx> <baud>", "Initialize the auxiliary UART",        cmd_uart_init},
    {"send",    "<data>",           "Transmit a string",                    cmd_uart_send},
    {"receive", "",                 "Show data received since the last call", cmd_uart_receive},
};

static const cli_group_t uart_group = {
    .name = "uart",
    .help = "Auxiliary UART (separate from this console)",
    .commands = uart_commands,
    .command_count = ARRAY_COUNT(uart_commands),
};

/* ------------------------------------------------------------------ */
/* spi                                                                 */
/* ------------------------------------------------------------------ */

static const cli_command_t spi_commands[] = {
    {"bus",   "<clk> <mosi> <miso> [cs]", "Initialize the SPI bus",        cmd_spi_bus},
    {"read",  "<addr> <len>",             "Read bytes from an address",    cmd_spi_read},
    {"write", "<addr> <data>...",         "Write bytes to an address",     cmd_spi_write},
    {"free",  "",                         "Release the SPI host so another module can use it", cmd_spi_free},
};

static const cli_group_t spi_group = {
    .name = "spi",
    .help = "SPI master",
    .commands = spi_commands,
    .command_count = ARRAY_COUNT(spi_commands),
};

/* ------------------------------------------------------------------ */
/* sd                                                                  */
/* ------------------------------------------------------------------ */

static const cli_command_t sd_commands[] = {
    {"spi",     "<clk> <mosi> <miso> <cs> [cd <pin>] [khz <freq>]", "Bring a card up over SPI", cmd_sd_spi},
    {"mmc",     "<clk> <cmd> <d0> [<d1> <d2> <d3>] [cd <pin>] [khz <freq>]", "Bring a card up in 1-bit or 4-bit SD mode", cmd_sd_mmc},
    {"info",    "",                                    "Report the detected card",  cmd_sd_info},
    {"bench",   "[size_kb] [block_kb]",                "Measure write and read speed through FAT", cmd_sd_bench},
    {"raw",     "[size_kb] [block_kb] [start_sector]", "Measure read speed with no filesystem", cmd_sd_raw},
    {"sweep",   "[max_khz] [size_kb] [block_kb]",      "Find the fastest clock the card reads correctly at", cmd_sd_sweep},
    {"results", "[clear]",                             "Show or delete the saved results file", cmd_sd_results},
    {"close",   "",                                    "Unmount, release the card and free the bus", cmd_sd_close},
};

static const cli_group_t sd_group = {
    .name = "sd",
    .help = "SD/MMC cards over SPI, 1-bit or 4-bit SD, with speed testing",
    .commands = sd_commands,
    .command_count = ARRAY_COUNT(sd_commands),
};

/* ------------------------------------------------------------------ */
/* touch                                                               */
/* ------------------------------------------------------------------ */

static const cli_command_t touch_commands[] = {
    {"watch", "<pads> [seconds]", "Calibrate touch pads and report all of them, live", cmd_touch_watch},
};

static const cli_group_t touch_group = {
    .name = "touch",
    .help = "Capacitive touch pads: calibrate a set and watch them live",
    .commands = touch_commands,
    .command_count = ARRAY_COUNT(touch_commands),
};

/* ------------------------------------------------------------------ */
/* board, board-<name>                                                 */
/*                                                                     */
/* A preset runs fully qualified command lines, so a board's group can  */
/* only offer what this firmware can already do. The audio presets      */
/* arrive with the audio subsystem.                                    */
/* ------------------------------------------------------------------ */

static const cli_command_t board_commands[] = {
    {"list", "", "List the boards this firmware knows", cmd_board_list},
};

static const cli_group_t board_group = {
    .name = "board",
    .help = "Known board pinouts and per-subsystem setup presets",
    .commands = board_commands,
    .command_count = ARRAY_COUNT(board_commands),
};

static const cli_command_t board_cardputer_commands[] = {
    {"pins", "", "Show the known pinout",                cmd_board_cardputer_pins},
    {"sd",   "", "Bring the microSD slot up over SPI",   cmd_board_cardputer_sd},
};

static const cli_group_t board_cardputer_group = {
    .name = "board-cardputer",
    .help = "M5Stack Cardputer (esp32s3)",
    .commands = board_cardputer_commands,
    .command_count = ARRAY_COUNT(board_cardputer_commands),
};

static const cli_command_t board_xiao_commands[] = {
    {"pins", "", "Show the known pinout",              cmd_board_xiao_pins},
    {"sd",   "", "Bring the microSD slot up over SPI", cmd_board_xiao_sd},
};

static const cli_group_t board_xiao_group = {
    .name = "board-xiao",
    .help = "Seeed XIAO ESP32-S3 Sense (esp32s3)",
    .commands = board_xiao_commands,
    .command_count = ARRAY_COUNT(board_xiao_commands),
};

static const cli_command_t board_sensor_commands[] = {
    {"pins", "", "Show the known pinout", cmd_board_sensor_pins},
    {"i2c",  "", "Initialize the I2C bus", cmd_board_sensor_i2c},
};

static const cli_group_t board_sensor_group = {
    .name = "board-sensor",
    .help = "ESP32-C3 sensor board (esp32c3)",
    .commands = board_sensor_commands,
    .command_count = ARRAY_COUNT(board_sensor_commands),
};

static const cli_command_t board_minstro_commands[] = {
    {"pins", "", "Show the known pinout",              cmd_board_minstro_pins},
    {"i2c",  "", "Initialize the I2C bus",             cmd_board_minstro_i2c},
    {"sd",   "", "Initialize the 4-bit SD interface",  cmd_board_minstro_sd},
};

static const cli_group_t board_minstro_group = {
    .name = "board-minstro",
    .help = "Minstro ESP32-S3 board (esp32s3)",
    .commands = board_minstro_commands,
    .command_count = ARRAY_COUNT(board_minstro_commands),
};

static const cli_command_t board_core_basic_commands[] = {
    {"pins", "", "Show the known pinout",              cmd_board_core_basic_pins},
    {"i2c",  "", "Initialize the I2C bus",             cmd_board_core_basic_i2c},
    {"sd",   "", "Bring the microSD slot up over SPI", cmd_board_core_basic_sd},
};

static const cli_group_t board_core_basic_group = {
    .name = "board-core-basic",
    .help = "M5Stack Core Basic (esp32)",
    .commands = board_core_basic_commands,
    .command_count = ARRAY_COUNT(board_core_basic_commands),
};

/* ------------------------------------------------------------------ */

static const cli_group_t *const groups[] = {
    &sys_group,
    &net_group,
    &gpio_group,
    &pwm_group,
    &i2c_group,
    &uart_group,
    &spi_group,
    &sd_group,
    &touch_group,
    &board_group,
    &board_cardputer_group,
    &board_xiao_group,
    &board_sensor_group,
    &board_minstro_group,
    &board_core_basic_group,
};

esp_err_t app_console_register(void)
{
    for (size_t i = 0; i < ARRAY_COUNT(groups); i++) {
        esp_err_t err = cli_register_group(groups[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
