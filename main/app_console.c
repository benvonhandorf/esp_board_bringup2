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
#include "audio.h"
#include "aw9523b_cmd.h"
#include "board.h"
#include "cli.h"
#include "codec_nau8822.h"
#include "codec_ns4168.h"
#include "codec_sph0645.h"
#include "config_reader.h"
#include "diag.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "gpio.h"
#include "hx711_cmd.h"
#include "i2c.h"
#include "ina219_cmd.h"
#include "ina226_cmd.h"
#include "ina237_cmd.h"
#include "lm75bdp_cmd.h"
#include "mqtt_manager.h"
#include "nau7802_cmd.h"
#include "ota.h"
#include "pi4ioe5v6408_cmd.h"
#include "pwm.h"
#include "rx8130ce_cmd.h"
#include "sd.h"
#include "sht4x_cmd.h"
#include "spi.h"
#include "strres.h"
#include "strres_ids.h"
#include "sys_hw.h"
#include "touch.h"
#include "uart.h"
#include "wifi.h"
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

    STRRES_PRINTF(STR_SYS_INFO_FIRMWARE, app->project_name, app->version);
    STRRES_PRINTF(STR_SYS_INFO_BUILT, app->date, app->time);
    STRRES_PRINTF(STR_SYS_INFO_IDF, app->idf_ver);
    STRRES_PRINTF(STR_SYS_INFO_CONFIG, config_reader_source());
    return 0;
}

static int cmd_sys_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    char buf[512];
    if (app_status_serialize(buf, sizeof(buf)) < 0) {
        STRRES_ERROR(STR_SYS_STATUS_TOO_LARGE);
        return -1;
    }
    diag_printf("%s\n", buf);
    return 0;
}

static int cmd_sys_restart(int argc, char **argv)
{
    (void)argc; (void)argv;
    STRRES_PRINTF(STR_SYS_RESTARTING);

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
        STRRES_PRINTF(STR_SYS_OTA_NOTHING_TO_CONFIRM);
        return 0;
    }
    esp_err_t err = ota_mark_valid();
    if (err != ESP_OK) {
        STRRES_ERROR(STR_SYS_OTA_CONFIRM_FAILED, esp_err_to_name(err));
        return -1;
    }
    STRRES_PRINTF(STR_SYS_OTA_CONFIRMED);
    return 0;
}

static int cmd_sys_rollback(int argc, char **argv)
{
    (void)argc; (void)argv;
    STRRES_PRINTF(STR_SYS_OTA_ROLLING_BACK);
    fflush(stdout);
    ota_rollback_and_reboot();
    /* Only reached if there is nothing to roll back to. */
    STRRES_ERROR(STR_SYS_OTA_NO_PREVIOUS);
    return -1;
}

static const cli_command_t sys_commands[] = {
    {"info",     NULL, NULL, cmd_sys_info},
    {"status",   NULL, NULL, cmd_sys_status},
    {"lfxtal",   NULL, NULL, cmd_sys_lfxtal},
    {"restart",  NULL, NULL, cmd_sys_restart},
    {"confirm",  NULL, NULL, cmd_sys_confirm},
    {"rollback", NULL, NULL, cmd_sys_rollback},
};

/* Parallel to sys_commands[]. A row here and a row there must stay in step;
 * the count is checked below. */
static const cli_command_text_t sys_text[] = {
    {STR_SYS_INFO_USAGE,      STR_SYS_INFO_HELP},
    {STR_SYS_STATUS_USAGE,    STR_SYS_STATUS_HELP},
    {STR_SYS_LFXTAL_USAGE,    STR_SYS_LFXTAL_HELP},
    {STR_SYS_RESTART_USAGE,   STR_SYS_RESTART_HELP},
    {STR_SYS_CONFIRM_USAGE,   STR_SYS_CONFIRM_HELP},
    {STR_SYS_ROLLBACK_USAGE,  STR_SYS_ROLLBACK_HELP},
};
_Static_assert(ARRAY_COUNT(sys_text) == ARRAY_COUNT(sys_commands),
               "sys help ids and commands must be the same length");

static const cli_group_t sys_group = {
    .name = "sys",
    .commands = sys_commands,
    .command_count = ARRAY_COUNT(sys_commands),
    .command_text = sys_text,
    .help_id = STR_SYS_GROUP_HELP,
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

    STRRES_PRINTF(STR_SYS_NET_WIFI, (int)wifi_manager_get_state(), ip[0] ? ip : "-", rssi);
    STRRES_PRINTF(STR_SYS_NET_MQTT, mqtt_manager_is_connected() ? "connected" : "disconnected");
    if (mqtt_manager_topic_prefix()) {
        STRRES_PRINTF(STR_SYS_NET_MQTT_UNDER, mqtt_manager_topic_prefix());
    }
    diag_printf("\n");
    return 0;
}

static int cmd_net_scan(int argc, char **argv)
{
    (void)argc; (void)argv;
    esp_err_t err = wifi_manager_scan_and_connect();
    if (err != ESP_OK) {
        STRRES_ERROR(STR_SYS_NET_SCAN_FAILED, esp_err_to_name(err));
        return -1;
    }
    STRRES_PRINTF(STR_SYS_NET_SCANNING);
    return 0;
}

static int cmd_net_ap(int argc, char **argv)
{
    (void)argc; (void)argv;
    esp_err_t err = wifi_manager_start_ap_mode();
    if (err != ESP_OK) {
        STRRES_ERROR(STR_SYS_NET_AP_START_FAILED, esp_err_to_name(err));
        return -1;
    }
    return 0;
}

static const cli_command_t net_commands[] = {
    {"status", NULL, NULL, cmd_net_status},
    {"scan",   NULL, NULL, cmd_net_scan},
    {"ap",     NULL, NULL, cmd_net_ap},
};

/* Parallel to net_commands[]. A row here and a row there must stay in step;
 * the count is checked below. */
static const cli_command_text_t net_text[] = {
    {STR_NET_STATUS_USAGE,  STR_NET_STATUS_HELP},
    {STR_NET_SCAN_USAGE,    STR_NET_SCAN_HELP},
    {STR_NET_AP_USAGE,      STR_NET_AP_HELP},
};
_Static_assert(ARRAY_COUNT(net_text) == ARRAY_COUNT(net_commands),
               "net help ids and commands must be the same length");

static const cli_group_t net_group = {
    .name = "net",
    .commands = net_commands,
    .command_count = ARRAY_COUNT(net_commands),
    .command_text = net_text,
    .help_id = STR_NET_GROUP_HELP,
};

/* ------------------------------------------------------------------ */
/* wifi -- the bench half; see main/wifi/wifi.h for the split from net */
/* ------------------------------------------------------------------ */

static const cli_command_t wifi_commands[] = {
    {"scan",     NULL, NULL, cmd_wifi_scan},
    {"connect",  NULL, NULL, cmd_wifi_connect},
    {"ap",       NULL, NULL, cmd_wifi_ap},
    {"status",   NULL, NULL, cmd_wifi_status},
    {"off",      NULL, NULL, cmd_wifi_off},
    {"on",       NULL, NULL, cmd_wifi_on},
    {"iperf",    NULL, NULL, cmd_wifi_iperf},
    {"netstats", NULL, NULL, cmd_wifi_netstats},
};

/* Parallel to wifi_commands[]. A row here and a row there must stay in step;
 * the count is checked below. */
static const cli_command_text_t wifi_text[] = {
    {STR_WIFI_SCAN_USAGE,      STR_WIFI_SCAN_HELP},
    {STR_WIFI_CONNECT_USAGE,   STR_WIFI_CONNECT_HELP},
    {STR_WIFI_AP_USAGE,        STR_WIFI_AP_HELP},
    {STR_WIFI_STATUS_USAGE,    STR_WIFI_STATUS_HELP},
    {STR_WIFI_OFF_USAGE,       STR_WIFI_OFF_HELP},
    {STR_WIFI_ON_USAGE,        STR_WIFI_ON_HELP},
    {STR_WIFI_IPERF_USAGE,     STR_WIFI_IPERF_HELP},
    {STR_WIFI_NETSTATS_USAGE,  STR_WIFI_NETSTATS_HELP},
};
_Static_assert(ARRAY_COUNT(wifi_text) == ARRAY_COUNT(wifi_commands),
               "wifi help ids and commands must be the same length");

static const cli_group_t wifi_group = {
    .name = "wifi",
    .commands = wifi_commands,
    .command_count = ARRAY_COUNT(wifi_commands),
    .command_text = wifi_text,
    .help_id = STR_WIFI_GROUP_HELP,
};

/* ------------------------------------------------------------------ */
/* gpio, gpio-pwm                                                      */
/* ------------------------------------------------------------------ */

static const cli_command_t gpio_commands[] = {
    {"set",    NULL, NULL, cmd_gpio_set},
    {"read",   NULL, NULL, cmd_gpio_read},
    {"aread",  NULL, NULL, cmd_gpio_aread},
    {"blink",  NULL, NULL, cmd_gpio_blink},
    {"short",  NULL, NULL, cmd_gpio_short},
    {"rc",     NULL, NULL, cmd_gpio_rc},
    {"survey", NULL, NULL, cmd_gpio_survey},
};

/* Parallel to gpio_commands[]. A row here and a row there must stay in step;
 * the count is checked below. */
static const cli_command_text_t gpio_text[] = {
    {STR_GPIO_SET_USAGE,     STR_GPIO_SET_HELP},
    {STR_GPIO_READ_USAGE,    STR_GPIO_READ_HELP},
    {STR_GPIO_AREAD_USAGE,   STR_GPIO_AREAD_HELP},
    {STR_GPIO_BLINK_USAGE,   STR_GPIO_BLINK_HELP},
    {STR_GPIO_SHORT_USAGE,   STR_GPIO_SHORT_HELP},
    {STR_GPIO_RC_USAGE,      STR_GPIO_RC_HELP},
    {STR_GPIO_SURVEY_USAGE,  STR_GPIO_SURVEY_HELP},
};
_Static_assert(ARRAY_COUNT(gpio_text) == ARRAY_COUNT(gpio_commands),
               "gpio help ids and commands must be the same length");

static const cli_group_t gpio_group = {
    .name = "gpio",
    .commands = gpio_commands,
    .command_count = ARRAY_COUNT(gpio_commands),
    .command_text = gpio_text,
    .help_id = STR_GPIO_GROUP_HELP,
};

static const cli_command_t pwm_commands[] = {
    {"set",  NULL, NULL, cmd_pwm_set},
    {"stop", NULL, NULL, cmd_pwm_stop},
};

/* Parallel to pwm_commands[]. A row here and a row there must stay in step;
 * the count is checked below. */
static const cli_command_text_t pwm_text[] = {
    {STR_GPIO_PWM_SET_USAGE,   STR_GPIO_PWM_SET_HELP},
    {STR_GPIO_PWM_STOP_USAGE,  STR_GPIO_PWM_STOP_HELP},
};
_Static_assert(ARRAY_COUNT(pwm_text) == ARRAY_COUNT(pwm_commands),
               "gpio-pwm help ids and commands must be the same length");

static const cli_group_t pwm_group = {
    .name = "gpio-pwm",
    .commands = pwm_commands,
    .command_count = ARRAY_COUNT(pwm_commands),
    .command_text = pwm_text,
    .help_id = STR_GPIO_PWM_GROUP_HELP,
};

/* ------------------------------------------------------------------ */
/* i2c                                                                 */
/* ------------------------------------------------------------------ */

/*
 * The first group whose help text lives on the `res` partition rather than in
 * the image: .usage and .help are left NULL and the ids beside them name the
 * strings, which app_console_register() teaches cli to resolve. The command
 * names stay here -- they are what the user types and what completion matches,
 * not prose.
 */
static const cli_command_t i2c_commands[] = {
    {"bus",      NULL, NULL, cmd_i2c_bus},
    {"scan",     NULL, NULL, cmd_i2c_scan},
    {"read",     NULL, NULL, cmd_i2c_read},
    {"identify", NULL, NULL, cmd_i2c_identify},
};

/* Parallel to i2c_commands[]. A row here and a row there must stay in step;
 * the count is checked below. */
static const cli_command_text_t i2c_text[] = {
    {STR_I2C_BUS_USAGE,      STR_I2C_BUS_HELP},
    {STR_I2C_SCAN_USAGE,     STR_I2C_SCAN_HELP},
    {STR_I2C_READ_USAGE,     STR_I2C_READ_HELP},
    {STR_I2C_IDENTIFY_USAGE, STR_I2C_IDENTIFY_HELP},
};
_Static_assert(ARRAY_COUNT(i2c_text) == ARRAY_COUNT(i2c_commands),
               "i2c help ids and commands must be the same length");

static const cli_group_t i2c_group = {
    .name = "i2c",
    .commands = i2c_commands,
    .command_count = ARRAY_COUNT(i2c_commands),
    .command_text = i2c_text,
    .help_id = STR_I2C_GROUP_HELP,
};

/* ------------------------------------------------------------------ */
/* i2c-<part>: the console half of a driver that lives in a component  */
/* ------------------------------------------------------------------ */

static const cli_command_t ina219_commands[] = {
    {"config", NULL, NULL, cmd_ina219_config},
    {"read",   NULL, NULL, cmd_ina219_read},
    {"list",   NULL, NULL, cmd_ina219_list},
};

/* Parallel to ina219_commands[]. A row here and a row there must stay in step;
 * the count is checked below. */
static const cli_command_text_t ina219_text[] = {
    {STR_I2C_INA219_CONFIG_USAGE,  STR_I2C_INA219_CONFIG_HELP},
    {STR_I2C_INA219_READ_USAGE,    STR_I2C_INA219_READ_HELP},
    {STR_I2C_INA219_LIST_USAGE,    STR_I2C_INA219_LIST_HELP},
};
_Static_assert(ARRAY_COUNT(ina219_text) == ARRAY_COUNT(ina219_commands),
               "i2c-ina219 help ids and commands must be the same length");

static const cli_group_t ina219_group = {
    .name = "i2c-ina219",
    .commands = ina219_commands,
    .command_count = ARRAY_COUNT(ina219_commands),
    .command_text = ina219_text,
    .help_id = STR_I2C_INA219_GROUP_HELP,
};

static const cli_command_t ina226_commands[] = {
    {"config", NULL, NULL, cmd_ina226_config},
    {"read",   NULL, NULL, cmd_ina226_read},
    {"list",   NULL, NULL, cmd_ina226_list},
};

/* Parallel to ina226_commands[]. A row here and a row there must stay in step;
 * the count is checked below. */
static const cli_command_text_t ina226_text[] = {
    {STR_I2C_INA226_CONFIG_USAGE,  STR_I2C_INA226_CONFIG_HELP},
    {STR_I2C_INA226_READ_USAGE,    STR_I2C_INA226_READ_HELP},
    {STR_I2C_INA226_LIST_USAGE,    STR_I2C_INA226_LIST_HELP},
};
_Static_assert(ARRAY_COUNT(ina226_text) == ARRAY_COUNT(ina226_commands),
               "i2c-ina226 help ids and commands must be the same length");

static const cli_group_t ina226_group = {
    .name = "i2c-ina226",
    .commands = ina226_commands,
    .command_count = ARRAY_COUNT(ina226_commands),
    .command_text = ina226_text,
    .help_id = STR_I2C_INA226_GROUP_HELP,
};

static const cli_command_t lm75bdp_commands[] = {
    {"read",   NULL, NULL, cmd_lm75bdp_read},
    {"limits", NULL, NULL, cmd_lm75bdp_limits},
};

/* Parallel to lm75bdp_commands[]. A row here and a row there must stay in step;
 * the count is checked below. */
static const cli_command_text_t lm75bdp_text[] = {
    {STR_I2C_LM75BDP_READ_USAGE,    STR_I2C_LM75BDP_READ_HELP},
    {STR_I2C_LM75BDP_LIMITS_USAGE,  STR_I2C_LM75BDP_LIMITS_HELP},
};
_Static_assert(ARRAY_COUNT(lm75bdp_text) == ARRAY_COUNT(lm75bdp_commands),
               "i2c-lm75bdp help ids and commands must be the same length");

static const cli_group_t lm75bdp_group = {
    .name = "i2c-lm75bdp",
    .commands = lm75bdp_commands,
    .command_count = ARRAY_COUNT(lm75bdp_commands),
    .command_text = lm75bdp_text,
    .help_id = STR_I2C_LM75BDP_GROUP_HELP,
};

static const cli_command_t rx8130ce_commands[] = {
    {"time", NULL, NULL, cmd_rx8130ce_time},
    {"set",  NULL, NULL, cmd_rx8130ce_set},
};

/* Parallel to rx8130ce_commands[]. A row here and a row there must stay in step;
 * the count is checked below. */
static const cli_command_text_t rx8130ce_text[] = {
    {STR_I2C_RX8130CE_TIME_USAGE,  STR_I2C_RX8130CE_TIME_HELP},
    {STR_I2C_RX8130CE_SET_USAGE,   STR_I2C_RX8130CE_SET_HELP},
};
_Static_assert(ARRAY_COUNT(rx8130ce_text) == ARRAY_COUNT(rx8130ce_commands),
               "i2c-rx8130ce help ids and commands must be the same length");

static const cli_group_t rx8130ce_group = {
    .name = "i2c-rx8130ce",
    .commands = rx8130ce_commands,
    .command_count = ARRAY_COUNT(rx8130ce_commands),
    .command_text = rx8130ce_text,
    .help_id = STR_I2C_RX8130CE_GROUP_HELP,
};

static const cli_command_t aw9523b_commands[] = {
    {"init",  NULL, NULL, cmd_aw9523b_init},
    {"read",  NULL, NULL, cmd_aw9523b_read},
    {"write", NULL, NULL, cmd_aw9523b_write},
    {"set",   NULL, NULL, cmd_aw9523b_set},
};

/* Parallel to aw9523b_commands[]. A row here and a row there must stay in step;
 * the count is checked below. */
static const cli_command_text_t aw9523b_text[] = {
    {STR_I2C_AW9523B_INIT_USAGE,   STR_I2C_AW9523B_INIT_HELP},
    {STR_I2C_AW9523B_READ_USAGE,   STR_I2C_AW9523B_READ_HELP},
    {STR_I2C_AW9523B_WRITE_USAGE,  STR_I2C_AW9523B_WRITE_HELP},
    {STR_I2C_AW9523B_SET_USAGE,    STR_I2C_AW9523B_SET_HELP},
};
_Static_assert(ARRAY_COUNT(aw9523b_text) == ARRAY_COUNT(aw9523b_commands),
               "i2c-aw9523b help ids and commands must be the same length");

static const cli_group_t aw9523b_group = {
    .name = "i2c-aw9523b",
    .commands = aw9523b_commands,
    .command_count = ARRAY_COUNT(aw9523b_commands),
    .command_text = aw9523b_text,
    .help_id = STR_I2C_AW9523B_GROUP_HELP,
};

static const cli_command_t pi4ioe_commands[] = {
    {"init",      NULL, NULL, cmd_pi4ioe_init},
    {"read",      NULL, NULL, cmd_pi4ioe_read},
    {"write",     NULL, NULL, cmd_pi4ioe_write},
    {"set",       NULL, NULL, cmd_pi4ioe_set},
    {"interrupt", NULL, NULL, cmd_pi4ioe_interrupt},
};

/* Parallel to pi4ioe_commands[]. A row here and a row there must stay in step;
 * the count is checked below. */
static const cli_command_text_t pi4ioe_text[] = {
    {STR_I2C_PI4IOE_INIT_USAGE,       STR_I2C_PI4IOE_INIT_HELP},
    {STR_I2C_PI4IOE_READ_USAGE,       STR_I2C_PI4IOE_READ_HELP},
    {STR_I2C_PI4IOE_WRITE_USAGE,      STR_I2C_PI4IOE_WRITE_HELP},
    {STR_I2C_PI4IOE_SET_USAGE,        STR_I2C_PI4IOE_SET_HELP},
    {STR_I2C_PI4IOE_INTERRUPT_USAGE,  STR_I2C_PI4IOE_INTERRUPT_HELP},
};
_Static_assert(ARRAY_COUNT(pi4ioe_text) == ARRAY_COUNT(pi4ioe_commands),
               "i2c-pi4ioe help ids and commands must be the same length");

static const cli_group_t pi4ioe_group = {
    .name = "i2c-pi4ioe",
    .commands = pi4ioe_commands,
    .command_count = ARRAY_COUNT(pi4ioe_commands),
    .command_text = pi4ioe_text,
    .help_id = STR_I2C_PI4IOE_GROUP_HELP,
};

static const cli_command_t ina237_commands[] = {
    {"config", NULL, NULL, cmd_ina237_config},
    {"read",   NULL, NULL, cmd_ina237_read},
    {"list",   NULL, NULL, cmd_ina237_list},
};

/* Parallel to ina237_commands[]. A row here and a row there must stay in step;
 * the count is checked below. */
static const cli_command_text_t ina237_text[] = {
    {STR_I2C_INA237_CONFIG_USAGE,  STR_I2C_INA237_CONFIG_HELP},
    {STR_I2C_INA237_READ_USAGE,    STR_I2C_INA237_READ_HELP},
    {STR_I2C_INA237_LIST_USAGE,    STR_I2C_INA237_LIST_HELP},
};
_Static_assert(ARRAY_COUNT(ina237_text) == ARRAY_COUNT(ina237_commands),
               "i2c-ina237 help ids and commands must be the same length");

static const cli_group_t ina237_group = {
    .name = "i2c-ina237",
    .commands = ina237_commands,
    .command_count = ARRAY_COUNT(ina237_commands),
    .command_text = ina237_text,
    .help_id = STR_I2C_INA237_GROUP_HELP,
};

static const cli_command_t sht4x_commands[] = {
    {"read",   NULL, NULL, cmd_sht4x_read},
    {"serial", NULL, NULL, cmd_sht4x_serial},
    {"heater", NULL, NULL, cmd_sht4x_heater},
    {"reset",  NULL, NULL, cmd_sht4x_reset},
};

/* Parallel to sht4x_commands[]. A row here and a row there must stay in step;
 * the count is checked below. */
static const cli_command_text_t sht4x_text[] = {
    {STR_I2C_SHT4X_READ_USAGE,    STR_I2C_SHT4X_READ_HELP},
    {STR_I2C_SHT4X_SERIAL_USAGE,  STR_I2C_SHT4X_SERIAL_HELP},
    {STR_I2C_SHT4X_HEATER_USAGE,  STR_I2C_SHT4X_HEATER_HELP},
    {STR_I2C_SHT4X_RESET_USAGE,   STR_I2C_SHT4X_RESET_HELP},
};
_Static_assert(ARRAY_COUNT(sht4x_text) == ARRAY_COUNT(sht4x_commands),
               "i2c-sht4x help ids and commands must be the same length");

static const cli_group_t sht4x_group = {
    .name = "i2c-sht4x",
    .commands = sht4x_commands,
    .command_count = ARRAY_COUNT(sht4x_commands),
    .command_text = sht4x_text,
    .help_id = STR_I2C_SHT4X_GROUP_HELP,
};

static const cli_command_t nau7802_commands[] = {
    {"init",      NULL, NULL, cmd_nau7802_init},
    {"status",    NULL, NULL, cmd_nau7802_status},
    {"gain",      NULL, NULL, cmd_nau7802_gain},
    {"rate",      NULL, NULL, cmd_nau7802_rate},
    {"input",     NULL, NULL, cmd_nau7802_input},
    {"drdy",      NULL, NULL, cmd_nau7802_drdy},
    {"ldomode",   NULL, NULL, cmd_nau7802_ldomode},
    {"pgacap",    NULL, NULL, cmd_nau7802_pgacap},
    {"raw",       NULL, NULL, cmd_nau7802_raw},
    {"registers", NULL, NULL, cmd_nau7802_registers},
    {"read",      NULL, NULL, cmd_nau7802_read},
    {"tare",      NULL, NULL, cmd_nau7802_tare},
    {"calibrate", NULL, NULL, cmd_nau7802_calibrate},
    {"scale",     NULL, NULL, cmd_nau7802_scale},
    {"weight",    NULL, NULL, cmd_nau7802_weight},
};

/* Parallel to nau7802_commands[]. A row here and a row there must stay in step;
 * the count is checked below. */
static const cli_command_text_t nau7802_text[] = {
    {STR_I2C_NAU7802_INIT_USAGE,       STR_I2C_NAU7802_INIT_HELP},
    {STR_I2C_NAU7802_STATUS_USAGE,     STR_I2C_NAU7802_STATUS_HELP},
    {STR_I2C_NAU7802_GAIN_USAGE,       STR_I2C_NAU7802_GAIN_HELP},
    {STR_I2C_NAU7802_RATE_USAGE,       STR_I2C_NAU7802_RATE_HELP},
    {STR_I2C_NAU7802_INPUT_USAGE,      STR_I2C_NAU7802_INPUT_HELP},
    {STR_I2C_NAU7802_DRDY_USAGE,       STR_I2C_NAU7802_DRDY_HELP},
    {STR_I2C_NAU7802_LDOMODE_USAGE,    STR_I2C_NAU7802_LDOMODE_HELP},
    {STR_I2C_NAU7802_PGACAP_USAGE,     STR_I2C_NAU7802_PGACAP_HELP},
    {STR_I2C_NAU7802_RAW_USAGE,        STR_I2C_NAU7802_RAW_HELP},
    {STR_I2C_NAU7802_REGISTERS_USAGE,  STR_I2C_NAU7802_REGISTERS_HELP},
    {STR_I2C_NAU7802_READ_USAGE,       STR_I2C_NAU7802_READ_HELP},
    {STR_I2C_NAU7802_TARE_USAGE,       STR_I2C_NAU7802_TARE_HELP},
    {STR_I2C_NAU7802_CALIBRATE_USAGE,  STR_I2C_NAU7802_CALIBRATE_HELP},
    {STR_I2C_NAU7802_SCALE_USAGE,      STR_I2C_NAU7802_SCALE_HELP},
    {STR_I2C_NAU7802_WEIGHT_USAGE,     STR_I2C_NAU7802_WEIGHT_HELP},
};
_Static_assert(ARRAY_COUNT(nau7802_text) == ARRAY_COUNT(nau7802_commands),
               "i2c-nau7802 help ids and commands must be the same length");

static const cli_group_t nau7802_group = {
    .name = "i2c-nau7802",
    .commands = nau7802_commands,
    .command_count = ARRAY_COUNT(nau7802_commands),
    .command_text = nau7802_text,
    .help_id = STR_I2C_NAU7802_GROUP_HELP,
};

/* ------------------------------------------------------------------ */
/* loadcell -- the HX711 has no bus, so it is not under i2c            */
/* ------------------------------------------------------------------ */

static const cli_command_t loadcell_commands[] = {
    {"init",      NULL, NULL, cmd_hx711_init},
    {"status",    NULL, NULL, cmd_hx711_status},
    {"gain",      NULL, NULL, cmd_hx711_gain},
    {"input",     NULL, NULL, cmd_hx711_input},
    {"power",     NULL, NULL, cmd_hx711_power},
    {"raw",       NULL, NULL, cmd_hx711_raw},
    {"read",      NULL, NULL, cmd_hx711_read},
    {"tare",      NULL, NULL, cmd_hx711_tare},
    {"calibrate", NULL, NULL, cmd_hx711_calibrate},
    {"scale",     NULL, NULL, cmd_hx711_scale},
    {"weight",    NULL, NULL, cmd_hx711_weight},
    {"close",     NULL, NULL, cmd_hx711_close},
};

/* Parallel to loadcell_commands[]. A row here and a row there must stay in step;
 * the count is checked below. */
static const cli_command_text_t loadcell_text[] = {
    {STR_LOADCELL_INIT_USAGE,       STR_LOADCELL_INIT_HELP},
    {STR_LOADCELL_STATUS_USAGE,     STR_LOADCELL_STATUS_HELP},
    {STR_LOADCELL_GAIN_USAGE,       STR_LOADCELL_GAIN_HELP},
    {STR_LOADCELL_INPUT_USAGE,      STR_LOADCELL_INPUT_HELP},
    {STR_LOADCELL_POWER_USAGE,      STR_LOADCELL_POWER_HELP},
    {STR_LOADCELL_RAW_USAGE,        STR_LOADCELL_RAW_HELP},
    {STR_LOADCELL_READ_USAGE,       STR_LOADCELL_READ_HELP},
    {STR_LOADCELL_TARE_USAGE,       STR_LOADCELL_TARE_HELP},
    {STR_LOADCELL_CALIBRATE_USAGE,  STR_LOADCELL_CALIBRATE_HELP},
    {STR_LOADCELL_SCALE_USAGE,      STR_LOADCELL_SCALE_HELP},
    {STR_LOADCELL_WEIGHT_USAGE,     STR_LOADCELL_WEIGHT_HELP},
    {STR_LOADCELL_CLOSE_USAGE,      STR_LOADCELL_CLOSE_HELP},
};
_Static_assert(ARRAY_COUNT(loadcell_text) == ARRAY_COUNT(loadcell_commands),
               "loadcell help ids and commands must be the same length");

static const cli_group_t loadcell_group = {
    .name = "loadcell",
    .commands = loadcell_commands,
    .command_count = ARRAY_COUNT(loadcell_commands),
    .command_text = loadcell_text,
    .help_id = STR_LOADCELL_GROUP_HELP,
};

/* ------------------------------------------------------------------ */
/* uart                                                                */
/* ------------------------------------------------------------------ */

static const cli_command_t uart_commands[] = {
    {"init",    NULL, NULL, cmd_uart_init},
    {"send",    NULL, NULL, cmd_uart_send},
    {"receive", NULL, NULL, cmd_uart_receive},
};

/* Parallel to uart_commands[]. A row here and a row there must stay in step;
 * the count is checked below. */
static const cli_command_text_t uart_text[] = {
    {STR_UART_INIT_USAGE,     STR_UART_INIT_HELP},
    {STR_UART_SEND_USAGE,     STR_UART_SEND_HELP},
    {STR_UART_RECEIVE_USAGE,  STR_UART_RECEIVE_HELP},
};
_Static_assert(ARRAY_COUNT(uart_text) == ARRAY_COUNT(uart_commands),
               "uart help ids and commands must be the same length");

static const cli_group_t uart_group = {
    .name = "uart",
    .commands = uart_commands,
    .command_count = ARRAY_COUNT(uart_commands),
    .command_text = uart_text,
    .help_id = STR_UART_GROUP_HELP,
};

/* ------------------------------------------------------------------ */
/* spi                                                                 */
/* ------------------------------------------------------------------ */

static const cli_command_t spi_commands[] = {
    {"bus",   NULL, NULL, cmd_spi_bus},
    {"read",  NULL, NULL, cmd_spi_read},
    {"write", NULL, NULL, cmd_spi_write},
    {"free",  NULL, NULL, cmd_spi_free},
};

/* Parallel to spi_commands[]. A row here and a row there must stay in step;
 * the count is checked below. */
static const cli_command_text_t spi_text[] = {
    {STR_SPI_BUS_USAGE,    STR_SPI_BUS_HELP},
    {STR_SPI_READ_USAGE,   STR_SPI_READ_HELP},
    {STR_SPI_WRITE_USAGE,  STR_SPI_WRITE_HELP},
    {STR_SPI_FREE_USAGE,   STR_SPI_FREE_HELP},
};
_Static_assert(ARRAY_COUNT(spi_text) == ARRAY_COUNT(spi_commands),
               "spi help ids and commands must be the same length");

static const cli_group_t spi_group = {
    .name = "spi",
    .commands = spi_commands,
    .command_count = ARRAY_COUNT(spi_commands),
    .command_text = spi_text,
    .help_id = STR_SPI_GROUP_HELP,
};

/* ------------------------------------------------------------------ */
/* audio, audio-<part>                                                 */
/*                                                                     */
/* A capability group, not a bus group: it owns I2S and delegates the  */
/* part to a codec driver. Adding a part is a new codec_*.c, a row in  */
/* the registry in audio.c, and a group here.                          */
/* ------------------------------------------------------------------ */

static const cli_command_t audio_commands[] = {
    {"bus",      NULL, NULL, cmd_audio_bus},
    {"pdm",      NULL, NULL, cmd_audio_pdm},
    {"info",     NULL, NULL, cmd_audio_info},
    {"codecs",   NULL, NULL, cmd_audio_codecs},
    {"tone",     NULL, NULL, cmd_audio_tone},
    {"sweep",    NULL, NULL, cmd_audio_sweep},
    {"stop",     NULL, NULL, cmd_audio_stop},
    {"record",   NULL, NULL, cmd_audio_record},
    {"capture",  NULL, NULL, cmd_audio_capture_file},
    {"level",    NULL, NULL, cmd_audio_level},
    {"loopback", NULL, NULL, cmd_audio_loopback},
    {"volume",   NULL, NULL, cmd_audio_volume},
    {"mute",     NULL, NULL, cmd_audio_mute},
    {"close",    NULL, NULL, cmd_audio_close},
};

/* Parallel to audio_commands[]. A row here and a row there must stay in step;
 * the count is checked below. */
static const cli_command_text_t audio_text[] = {
    {STR_AUDIO_BUS_USAGE,       STR_AUDIO_BUS_HELP},
    {STR_AUDIO_PDM_USAGE,       STR_AUDIO_PDM_HELP},
    {STR_AUDIO_INFO_USAGE,      STR_AUDIO_INFO_HELP},
    {STR_AUDIO_CODECS_USAGE,    STR_AUDIO_CODECS_HELP},
    {STR_AUDIO_TONE_USAGE,      STR_AUDIO_TONE_HELP},
    {STR_AUDIO_SWEEP_USAGE,     STR_AUDIO_SWEEP_HELP},
    {STR_AUDIO_STOP_USAGE,      STR_AUDIO_STOP_HELP},
    {STR_AUDIO_RECORD_USAGE,    STR_AUDIO_RECORD_HELP},
    {STR_AUDIO_CAPTURE_USAGE,   STR_AUDIO_CAPTURE_HELP},
    {STR_AUDIO_LEVEL_USAGE,     STR_AUDIO_LEVEL_HELP},
    {STR_AUDIO_LOOPBACK_USAGE,  STR_AUDIO_LOOPBACK_HELP},
    {STR_AUDIO_VOLUME_USAGE,    STR_AUDIO_VOLUME_HELP},
    {STR_AUDIO_MUTE_USAGE,      STR_AUDIO_MUTE_HELP},
    {STR_AUDIO_CLOSE_USAGE,     STR_AUDIO_CLOSE_HELP},
};
_Static_assert(ARRAY_COUNT(audio_text) == ARRAY_COUNT(audio_commands),
               "audio help ids and commands must be the same length");

static const cli_group_t audio_group = {
    .name = "audio",
    .commands = audio_commands,
    .command_count = ARRAY_COUNT(audio_commands),
    .command_text = audio_text,
    .help_id = STR_AUDIO_GROUP_HELP,
};

static const cli_command_t nau8822_commands[] = {
    {"init",   NULL, NULL, cmd_nau8822_init},
    {"status", NULL, NULL, cmd_nau8822_status},
    {"reg",    NULL, NULL, cmd_nau8822_reg},
    {"route",  NULL, NULL, cmd_nau8822_route},
    {"input",  NULL, NULL, cmd_nau8822_input},
    {"gain",   NULL, NULL, cmd_nau8822_gain},
};

/* Parallel to nau8822_commands[]. A row here and a row there must stay in step;
 * the count is checked below. */
static const cli_command_text_t nau8822_text[] = {
    {STR_AUDIO_NAU8822_INIT_USAGE,    STR_AUDIO_NAU8822_INIT_HELP},
    {STR_AUDIO_NAU8822_STATUS_USAGE,  STR_AUDIO_NAU8822_STATUS_HELP},
    {STR_AUDIO_NAU8822_REG_USAGE,     STR_AUDIO_NAU8822_REG_HELP},
    {STR_AUDIO_NAU8822_ROUTE_USAGE,   STR_AUDIO_NAU8822_ROUTE_HELP},
    {STR_AUDIO_NAU8822_INPUT_USAGE,   STR_AUDIO_NAU8822_INPUT_HELP},
    {STR_AUDIO_NAU8822_GAIN_USAGE,    STR_AUDIO_NAU8822_GAIN_HELP},
};
_Static_assert(ARRAY_COUNT(nau8822_text) == ARRAY_COUNT(nau8822_commands),
               "audio-nau8822 help ids and commands must be the same length");

static const cli_group_t nau8822_group = {
    .name = "audio-nau8822",
    .commands = nau8822_commands,
    .command_count = ARRAY_COUNT(nau8822_commands),
    .command_text = nau8822_text,
    .help_id = STR_AUDIO_NAU8822_GROUP_HELP,
};

static const cli_command_t ns4168_commands[] = {
    {"init",   NULL, NULL, cmd_ns4168_init},
    {"status", NULL, NULL, cmd_ns4168_status},
};

/* Parallel to ns4168_commands[]. A row here and a row there must stay in step;
 * the count is checked below. */
static const cli_command_text_t ns4168_text[] = {
    {STR_AUDIO_NS4168_INIT_USAGE,    STR_AUDIO_NS4168_INIT_HELP},
    {STR_AUDIO_NS4168_STATUS_USAGE,  STR_AUDIO_NS4168_STATUS_HELP},
};
_Static_assert(ARRAY_COUNT(ns4168_text) == ARRAY_COUNT(ns4168_commands),
               "audio-ns4168 help ids and commands must be the same length");

static const cli_group_t ns4168_group = {
    .name = "audio-ns4168",
    .commands = ns4168_commands,
    .command_count = ARRAY_COUNT(ns4168_commands),
    .command_text = ns4168_text,
    .help_id = STR_AUDIO_NS4168_GROUP_HELP,
};

static const cli_command_t sph0645_commands[] = {
    {"init",   NULL, NULL, cmd_sph0645_init},
    {"status", NULL, NULL, cmd_sph0645_status},
};

/* Parallel to sph0645_commands[]. A row here and a row there must stay in step;
 * the count is checked below. */
static const cli_command_text_t sph0645_text[] = {
    {STR_AUDIO_SPH0645_INIT_USAGE,    STR_AUDIO_SPH0645_INIT_HELP},
    {STR_AUDIO_SPH0645_STATUS_USAGE,  STR_AUDIO_SPH0645_STATUS_HELP},
};
_Static_assert(ARRAY_COUNT(sph0645_text) == ARRAY_COUNT(sph0645_commands),
               "audio-sph0645 help ids and commands must be the same length");

static const cli_group_t sph0645_group = {
    .name = "audio-sph0645",
    .commands = sph0645_commands,
    .command_count = ARRAY_COUNT(sph0645_commands),
    .command_text = sph0645_text,
    .help_id = STR_AUDIO_SPH0645_GROUP_HELP,
};

/* ------------------------------------------------------------------ */
/* sd                                                                  */
/* ------------------------------------------------------------------ */

static const cli_command_t sd_commands[] = {
    {"spi",     NULL, NULL, cmd_sd_spi},
    {"mmc",     NULL, NULL, cmd_sd_mmc},
    {"info",    NULL, NULL, cmd_sd_info},
    {"bench",   NULL, NULL, cmd_sd_bench},
    {"raw",     NULL, NULL, cmd_sd_raw},
    {"sweep",   NULL, NULL, cmd_sd_sweep},
    {"results", NULL, NULL, cmd_sd_results},
    {"close",   NULL, NULL, cmd_sd_close},
};

/* Parallel to sd_commands[]. A row here and a row there must stay in step;
 * the count is checked below. */
static const cli_command_text_t sd_text[] = {
    {STR_SD_SPI_USAGE,      STR_SD_SPI_HELP},
    {STR_SD_MMC_USAGE,      STR_SD_MMC_HELP},
    {STR_SD_INFO_USAGE,     STR_SD_INFO_HELP},
    {STR_SD_BENCH_USAGE,    STR_SD_BENCH_HELP},
    {STR_SD_RAW_USAGE,      STR_SD_RAW_HELP},
    {STR_SD_SWEEP_USAGE,    STR_SD_SWEEP_HELP},
    {STR_SD_RESULTS_USAGE,  STR_SD_RESULTS_HELP},
    {STR_SD_CLOSE_USAGE,    STR_SD_CLOSE_HELP},
};
_Static_assert(ARRAY_COUNT(sd_text) == ARRAY_COUNT(sd_commands),
               "sd help ids and commands must be the same length");

static const cli_group_t sd_group = {
    .name = "sd",
    .commands = sd_commands,
    .command_count = ARRAY_COUNT(sd_commands),
    .command_text = sd_text,
    .help_id = STR_SD_GROUP_HELP,
};

/* ------------------------------------------------------------------ */
/* touch                                                               */
/* ------------------------------------------------------------------ */

static const cli_command_t touch_commands[] = {
    {"watch", NULL, NULL, cmd_touch_watch},
};

/* Parallel to touch_commands[]. A row here and a row there must stay in step;
 * the count is checked below. */
static const cli_command_text_t touch_text[] = {
    {STR_TOUCH_WATCH_USAGE,  STR_TOUCH_WATCH_HELP},
};
_Static_assert(ARRAY_COUNT(touch_text) == ARRAY_COUNT(touch_commands),
               "touch help ids and commands must be the same length");

static const cli_group_t touch_group = {
    .name = "touch",
    .commands = touch_commands,
    .command_count = ARRAY_COUNT(touch_commands),
    .command_text = touch_text,
    .help_id = STR_TOUCH_GROUP_HELP,
};

/* ------------------------------------------------------------------ */
/* board, board-<name>                                                 */
/*                                                                     */
/* A preset runs fully qualified command lines, so a board's group can  */
/* only offer what this firmware can already do -- which is now all of  */
/* it. `chip_matches()` refuses a preset for another chip.              */
/* ------------------------------------------------------------------ */

static const cli_command_t board_commands[] = {
    {"list", NULL, NULL, cmd_board_list},
};

/* Parallel to board_commands[]. A row here and a row there must stay in step;
 * the count is checked below. */
static const cli_command_text_t board_text[] = {
    {STR_BOARD_LIST_USAGE,  STR_BOARD_LIST_HELP},
};
_Static_assert(ARRAY_COUNT(board_text) == ARRAY_COUNT(board_commands),
               "board help ids and commands must be the same length");

static const cli_group_t board_group = {
    .name = "board",
    .commands = board_commands,
    .command_count = ARRAY_COUNT(board_commands),
    .command_text = board_text,
    .help_id = STR_BOARD_GROUP_HELP,
};

static const cli_command_t board_cardputer_commands[] = {
    {"pins",  NULL, NULL, cmd_board_cardputer_pins},
    {"audio", NULL, NULL, cmd_board_cardputer_audio},
    {"mic",   NULL, NULL, cmd_board_cardputer_mic},
    {"sd",    NULL, NULL, cmd_board_cardputer_sd},
};

/* Parallel to board_cardputer_commands[]. A row here and a row there must stay in step;
 * the count is checked below. */
static const cli_command_text_t board_cardputer_text[] = {
    {STR_BOARD_CARDPUTER_PINS_USAGE,   STR_BOARD_CARDPUTER_PINS_HELP},
    {STR_BOARD_CARDPUTER_AUDIO_USAGE,  STR_BOARD_CARDPUTER_AUDIO_HELP},
    {STR_BOARD_CARDPUTER_MIC_USAGE,    STR_BOARD_CARDPUTER_MIC_HELP},
    {STR_BOARD_CARDPUTER_SD_USAGE,     STR_BOARD_CARDPUTER_SD_HELP},
};
_Static_assert(ARRAY_COUNT(board_cardputer_text) == ARRAY_COUNT(board_cardputer_commands),
               "board-cardputer help ids and commands must be the same length");

static const cli_group_t board_cardputer_group = {
    .name = "board-cardputer",
    .commands = board_cardputer_commands,
    .command_count = ARRAY_COUNT(board_cardputer_commands),
    .command_text = board_cardputer_text,
    .help_id = STR_BOARD_CARDPUTER_GROUP_HELP,
};

static const cli_command_t board_xiao_commands[] = {
    {"pins", NULL, NULL, cmd_board_xiao_pins},
    {"mic",  NULL, NULL, cmd_board_xiao_mic},
    {"sd",   NULL, NULL, cmd_board_xiao_sd},
};

/* Parallel to board_xiao_commands[]. A row here and a row there must stay in step;
 * the count is checked below. */
static const cli_command_text_t board_xiao_text[] = {
    {STR_BOARD_XIAO_PINS_USAGE,  STR_BOARD_XIAO_PINS_HELP},
    {STR_BOARD_XIAO_MIC_USAGE,   STR_BOARD_XIAO_MIC_HELP},
    {STR_BOARD_XIAO_SD_USAGE,    STR_BOARD_XIAO_SD_HELP},
};
_Static_assert(ARRAY_COUNT(board_xiao_text) == ARRAY_COUNT(board_xiao_commands),
               "board-xiao help ids and commands must be the same length");

static const cli_group_t board_xiao_group = {
    .name = "board-xiao",
    .commands = board_xiao_commands,
    .command_count = ARRAY_COUNT(board_xiao_commands),
    .command_text = board_xiao_text,
    .help_id = STR_BOARD_XIAO_GROUP_HELP,
};

static const cli_command_t board_sensor_commands[] = {
    {"pins", NULL, NULL, cmd_board_sensor_pins},
    {"i2c",  NULL, NULL, cmd_board_sensor_i2c},
};

/* Parallel to board_sensor_commands[]. A row here and a row there must stay in step;
 * the count is checked below. */
static const cli_command_text_t board_sensor_text[] = {
    {STR_BOARD_SENSOR_PINS_USAGE,  STR_BOARD_SENSOR_PINS_HELP},
    {STR_BOARD_SENSOR_I2C_USAGE,   STR_BOARD_SENSOR_I2C_HELP},
};
_Static_assert(ARRAY_COUNT(board_sensor_text) == ARRAY_COUNT(board_sensor_commands),
               "board-sensor help ids and commands must be the same length");

static const cli_group_t board_sensor_group = {
    .name = "board-sensor",
    .commands = board_sensor_commands,
    .command_count = ARRAY_COUNT(board_sensor_commands),
    .command_text = board_sensor_text,
    .help_id = STR_BOARD_SENSOR_GROUP_HELP,
};

static const cli_command_t board_minstro_commands[] = {
    {"pins",  NULL, NULL, cmd_board_minstro_pins},
    {"audio", NULL, NULL, cmd_board_minstro_audio},
    {"i2c",   NULL, NULL, cmd_board_minstro_i2c},
    {"sd",    NULL, NULL, cmd_board_minstro_sd},
};

/* Parallel to board_minstro_commands[]. A row here and a row there must stay in step;
 * the count is checked below. */
static const cli_command_text_t board_minstro_text[] = {
    {STR_BOARD_MINSTRO_PINS_USAGE,   STR_BOARD_MINSTRO_PINS_HELP},
    {STR_BOARD_MINSTRO_AUDIO_USAGE,  STR_BOARD_MINSTRO_AUDIO_HELP},
    {STR_BOARD_MINSTRO_I2C_USAGE,    STR_BOARD_MINSTRO_I2C_HELP},
    {STR_BOARD_MINSTRO_SD_USAGE,     STR_BOARD_MINSTRO_SD_HELP},
};
_Static_assert(ARRAY_COUNT(board_minstro_text) == ARRAY_COUNT(board_minstro_commands),
               "board-minstro help ids and commands must be the same length");

static const cli_group_t board_minstro_group = {
    .name = "board-minstro",
    .commands = board_minstro_commands,
    .command_count = ARRAY_COUNT(board_minstro_commands),
    .command_text = board_minstro_text,
    .help_id = STR_BOARD_MINSTRO_GROUP_HELP,
};

static const cli_command_t board_core_basic_commands[] = {
    {"pins",  NULL, NULL, cmd_board_core_basic_pins},
    {"audio", NULL, NULL, cmd_board_core_basic_audio},
    {"mic",   NULL, NULL, cmd_board_core_basic_mic},
    {"i2c",   NULL, NULL, cmd_board_core_basic_i2c},
    {"sd",    NULL, NULL, cmd_board_core_basic_sd},
};

/* Parallel to board_core_basic_commands[]. A row here and a row there must stay in step;
 * the count is checked below. */
static const cli_command_text_t board_core_basic_text[] = {
    {STR_BOARD_CORE_BASIC_PINS_USAGE,   STR_BOARD_CORE_BASIC_PINS_HELP},
    {STR_BOARD_CORE_BASIC_AUDIO_USAGE,  STR_BOARD_CORE_BASIC_AUDIO_HELP},
    {STR_BOARD_CORE_BASIC_MIC_USAGE,    STR_BOARD_CORE_BASIC_MIC_HELP},
    {STR_BOARD_CORE_BASIC_I2C_USAGE,    STR_BOARD_CORE_BASIC_I2C_HELP},
    {STR_BOARD_CORE_BASIC_SD_USAGE,     STR_BOARD_CORE_BASIC_SD_HELP},
};
_Static_assert(ARRAY_COUNT(board_core_basic_text) == ARRAY_COUNT(board_core_basic_commands),
               "board-core-basic help ids and commands must be the same length");

static const cli_group_t board_core_basic_group = {
    .name = "board-core-basic",
    .commands = board_core_basic_commands,
    .command_count = ARRAY_COUNT(board_core_basic_commands),
    .command_text = board_core_basic_text,
    .help_id = STR_BOARD_CORE_BASIC_GROUP_HELP,
};

/* ------------------------------------------------------------------ */

static const cli_group_t *const groups[] = {
    &sys_group,
    &net_group,
    &wifi_group,
    &gpio_group,
    &pwm_group,
    &i2c_group,
    &aw9523b_group,
    &ina219_group,
    &ina226_group,
    &ina237_group,
    &lm75bdp_group,
    &pi4ioe_group,
    &rx8130ce_group,
    &nau7802_group,
    &sht4x_group,
    &loadcell_group,
    &audio_group,
    &nau8822_group,
    &ns4168_group,
    &sph0645_group,
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

/*
 * How cli turns a help id back into words.
 *
 * cli has no idea where the text lives -- that is the point of the callback --
 * so this is the one place the shell and the string catalogue meet.
 */
static const char *resolve_help_text(uint16_t id, char *buf, size_t buflen)
{
    return strres_copy((strres_id_t)id, buf, buflen) < 0 ? NULL : buf;
}

esp_err_t app_console_register(void)
{
    cli_set_text_resolver(resolve_help_text);

    for (size_t i = 0; i < ARRAY_COUNT(groups); i++) {
        esp_err_t err = cli_register_group(groups[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
