/*
 * Console commands for the LM75B. The driver is the lm75bdp shared component.
 *
 * Simpler than the current monitors: there is no calibration to carry, so a
 * handle is created on demand for whatever address was named and kept only so
 * repeated reads do not re-allocate.
 */
#include "app_bringup.h"
#include "lm75bdp_cmd.h"
#include "i2c.h"

#include "lm75bdp.h"

/* A2..A0 select one of eight addresses. */
#define LM75BDP_ADDR_FIRST 0x48
#define LM75BDP_ADDR_LAST  0x4f

static lm75bdp_handle_t handle;
static uint8_t handle_address;

static int take_address(int argc, char **argv, int index, int *out)
{
    if (argc <= index) {
        *out = LM75BDP_I2C_ADDR_DEFAULT;
        return 0;
    }
    if (cli_parse_num_arg(argv[index], out) < 0 ||
        *out < LM75BDP_ADDR_FIRST || *out > LM75BDP_ADDR_LAST) {
        diag_error("Address must be 0x%02X-0x%02X (set by the A0-A2 pins)",
                   LM75BDP_ADDR_FIRST, LM75BDP_ADDR_LAST);
        return -1;
    }
    return 0;
}

/*
 * Get a handle pointed at `address`.
 *
 * The device handle is re-fetched from i2c.c every time: that cache recycles
 * wholesale when it fills and is emptied outright by 'i2c bus', so a handle
 * held across either would dangle.
 */
static lm75bdp_handle_t require_device(uint8_t address)
{
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_device_handle(address, &dev) != ESP_OK) {
        diag_error("Addressing 0x%02X failed", address);
        return NULL;
    }

    lm75bdp_report_t report = {0};
    esp_err_t err;
    if (!handle) {
        const lm75bdp_config_t config = {.dev = dev};
        err = lm75bdp_create(&config, &handle, &report);
    } else {
        err = lm75bdp_set_device(handle, dev, &report);
    }

    if (err != ESP_OK) {
        /*
         * Name the stage rather than the errno alone. On a bench the common
         * case by far is nothing at this address, and "confirming the part
         * responds" says that; an earlier version reported every failure here
         * as "out of memory", which sent the reader looking at the firmware
         * instead of at the board.
         */
        if (report.failed_stage == LM75BDP_STAGE_IDENTIFY) {
            diag_error("Nothing answering as an LM75B at 0x%02X: the "
                       "configuration register did not read back what was "
                       "written (%s)", address, esp_err_to_name(err));
        } else {
            diag_error("0x%02X: %s failed: %s", address,
                       lm75bdp_stage_name(report.failed_stage), esp_err_to_name(err));
        }
        if (err == ESP_ERR_INVALID_RESPONSE || err == ESP_ERR_NOT_FOUND ||
            err == ESP_ERR_TIMEOUT) {
            /* The overwhelmingly common case on a bench: nothing is at
             * this address at all. The stage name alone reads as though
             * a present part misbehaved. */
            diag_error("Nothing acknowledged at 0x%02X. 'i2c scan' lists "
                       "what is actually on the bus.", address);
        }
        if (!handle_address) {
            /* Never successfully attached; do not keep a handle that would make
             * the next call look initialised. */
            lm75bdp_delete(handle);
            handle = NULL;
        }
        return NULL;
    }

    handle_address = address;
    return handle;
}

int cmd_lm75bdp_read(int argc, char **argv)
{
    if (!i2c_require_bus()) {
        return -1;
    }

    int address = 0;
    if (take_address(argc, argv, 1, &address) < 0) {
        return -1;
    }

    lm75bdp_handle_t lm = require_device((uint8_t)address);
    if (!lm) {
        return -1;
    }

    lm75bdp_reading_t reading = {0};
    esp_err_t err = lm75bdp_read(lm, &reading);
    if (err != ESP_OK) {
        diag_error("Reading 0x%02X: %s", address, esp_err_to_name(err));
        return -1;
    }

    diag_printf("0x%02X  Temp %7.3f C (%7.2f F)   0.125 C resolution\n",
                address, reading.temperature_C,
                reading.temperature_C * 9.0 / 5.0 + 32.0);
    return 0;
}

int cmd_lm75bdp_limits(int argc, char **argv)
{
    if (!i2c_require_bus()) {
        return -1;
    }

    /* "limits [address] <tos> <thyst>": the address is optional and leading,
     * so the two thresholds are the last two arguments either way. */
    if (argc < 3 || argc > 4) {
        diag_printf("Usage: limits [address] <tos_C> <thyst_C>\n");
        diag_printf("The OS output asserts above tos and releases below thyst. "
                    "Equal values make it chatter around the threshold.\n");
        return -1;
    }

    int address = LM75BDP_I2C_ADDR_DEFAULT;
    int first = 1;
    if (argc == 4) {
        if (take_address(argc, argv, 1, &address) < 0) {
            return -1;
        }
        first = 2;
    }

    double tos = 0.0, thyst = 0.0;
    if (cli_parse_double_arg(argv[first], &tos) < 0 ||
        cli_parse_double_arg(argv[first + 1], &thyst) < 0) {
        diag_error("Both thresholds must be numbers in degrees Celsius");
        return -1;
    }
    if (thyst > tos) {
        diag_error("thyst (%.2f C) is above tos (%.2f C); the output would "
                   "never release", thyst, tos);
        return -1;
    }

    lm75bdp_handle_t lm = require_device((uint8_t)address);
    if (!lm) {
        return -1;
    }

    lm75bdp_report_t report = {0};
    esp_err_t err = lm75bdp_set_thresholds(lm, (float)tos, (float)thyst, &report);
    if (err != ESP_OK) {
        diag_error("Writing thresholds to 0x%02X: %s (%s)", address,
                   esp_err_to_name(err), lm75bdp_stage_name(report.failed_stage));
        return -1;
    }

    /*
     * Report what was programmed, not what was asked for. Thresholds quantise
     * to 0.5 C and clamp to -128..+127.5 C, so the two differ often enough that
     * echoing the request back would be a lie about the part's behaviour.
     */
    diag_printf("0x%02X  OS asserts above %.1f C, releases below %.1f C\n",
                address, report.tos_C, report.thyst_C);
    if (report.tos_C != (float)tos || report.thyst_C != (float)thyst) {
        diag_printf("      Asked for %.2f / %.2f C; thresholds quantise to 0.5 C "
                    "and clamp to -128..+127.5 C.\n", tos, thyst);
    }
    return 0;
}
