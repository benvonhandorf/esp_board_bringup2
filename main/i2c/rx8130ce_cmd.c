/*
 * Console commands for the RX8130CE. The driver is the rx8130ce shared
 * component.
 *
 * One fixed address, so there is nothing to configure and no table to keep --
 * closer to nau7802_cmd.c than to the current monitors.
 *
 * The point of these two commands on a bench is the backup cell. A board that
 * has been on the shelf for a month either comes back knowing the time or does
 * not, and only the part itself can say which; a clock whose backup has drained
 * still answers, and still returns registers that decode to something.
 */
#include "app_bringup.h"
#include "rx8130ce_cmd.h"
#include "i2c.h"

#include <time.h>

#include "rx8130ce.h"

static rx8130ce_handle_t handle;

static rx8130ce_handle_t require_device(void)
{
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_device_handle(RX8130CE_I2C_ADDR_DEFAULT, &dev) != ESP_OK) {
        diag_error("Addressing 0x%02X failed", RX8130CE_I2C_ADDR_DEFAULT);
        return NULL;
    }

    rx8130ce_report_t report = {0};
    if (!handle) {
        const rx8130ce_config_t config = {.dev = dev};
        if (rx8130ce_create(&config, &handle, &report) != ESP_OK) {
            diag_error("No RX8130CE answering at 0x%02X", RX8130CE_I2C_ADDR_DEFAULT);
            return NULL;
        }
    } else {
        /* Re-fetched every time: the handle cache in i2c.c recycles wholesale
         * when it fills and is emptied outright by 'i2c bus'. */
        rx8130ce_set_device(handle, dev, &report);
    }
    return handle;
}

static void print_time(const struct timeval *tv, const char *label)
{
    struct tm utc;
    time_t seconds = tv->tv_sec;
    gmtime_r(&seconds, &utc);

    char text[32];
    strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &utc);
    diag_printf("%-9s %s UTC\n", label, text);
}

int cmd_rx8130ce_time(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!i2c_require_bus()) {
        return -1;
    }

    rx8130ce_handle_t rtc = require_device();
    if (!rtc) {
        return -1;
    }

    struct timeval tv = {0};
    esp_err_t err = rx8130ce_get_time(rtc, &tv);
    if (err == ESP_ERR_RX8130CE_NOT_SET) {
        /*
         * The distinction this command exists to make. The part is there and
         * answering; its registers simply do not decode to a date. Returning
         * that as a time would let a dead backup cell pass for a device that
         * believes it is the year 2000.
         */
        diag_error("The clock is answering but its registers do not decode to "
                   "a date -- a drained backup cell, or never set. Run "
                   "'i2c-rx8130ce set'.");
        return -1;
    }
    if (err != ESP_OK) {
        diag_error("Reading the clock: %s", esp_err_to_name(err));
        return -1;
    }

    print_time(&tv, "RTC:");

    struct timeval now = {0};
    gettimeofday(&now, NULL);
    if (now.tv_sec > 1000000000) {
        print_time(&now, "System:");
        long drift = (long)(tv.tv_sec - now.tv_sec);
        diag_printf("Drift:    %+ld s\n", drift);
    } else {
        diag_printf("System:   not set; nothing to compare against. NTP sets it "
                    "once there is a network.\n");
    }

    if (rx8130ce_power_was_lost(rtc)) {
        diag_printf("Power:    lost since the clock was last set, so the time "
                    "above may be wrong. 'i2c-rx8130ce set' clears the "
                    "flag.\n");
    }
    return 0;
}

int cmd_rx8130ce_set(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!i2c_require_bus()) {
        return -1;
    }

    struct timeval now = {0};
    gettimeofday(&now, NULL);
    if (now.tv_sec < 1000000000) {
        diag_error("The system clock is not set, so there is nothing to copy "
                   "into the RTC. Check 'net status' and let NTP set it.");
        return -1;
    }

    rx8130ce_handle_t rtc = require_device();
    if (!rtc) {
        return -1;
    }

    esp_err_t err = rx8130ce_set_time(rtc, &now);
    if (err != ESP_OK) {
        diag_error("Setting the clock: %s", esp_err_to_name(err));
        return -1;
    }

    print_time(&now, "RTC set:");
    diag_printf("The power-lost flag is cleared\n");
    return 0;
}
