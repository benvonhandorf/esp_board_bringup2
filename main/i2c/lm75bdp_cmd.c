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
        STRRES_ERROR(STR_I2C_LM75BDP_ADDRESS_RANGE, LM75BDP_ADDR_FIRST, LM75BDP_ADDR_LAST);
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
        STRRES_ERROR(STR_I2C_LM75BDP_ADDRESSING_FAILED, address);
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
            STRRES_ERROR(STR_I2C_LM75BDP_NOT_AN_LM75B, address, esp_err_to_name(err));
        } else {
            STRRES_ERROR(STR_I2C_LM75BDP_STAGE_FAILED,
                         address, lm75bdp_stage_name(report.failed_stage),
                         esp_err_to_name(err));
        }
        if (err == ESP_ERR_INVALID_RESPONSE || err == ESP_ERR_NOT_FOUND ||
            err == ESP_ERR_TIMEOUT) {
            /* The overwhelmingly common case on a bench: nothing is at
             * this address at all. The stage name alone reads as though
             * a present part misbehaved. */
            STRRES_ERROR(STR_I2C_LM75BDP_NO_ACKNOWLEDGE, address);
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
        STRRES_ERROR(STR_I2C_LM75BDP_READ_FAILED, address, esp_err_to_name(err));
        return -1;
    }

    STRRES_PRINTF(STR_I2C_LM75BDP_READ_LINE,
                  address, reading.temperature_C, reading.temperature_C * 9.0 / 5.0 + 32.0);
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
        STRRES_PRINTF(STR_I2C_LM75BDP_USAGE_LIMITS);
        STRRES_PRINTF(STR_I2C_LM75BDP_USAGE_LIMITS_NOTE);
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
        STRRES_ERROR(STR_I2C_LM75BDP_THRESHOLDS_NUMERIC);
        return -1;
    }
    if (thyst > tos) {
        STRRES_ERROR(STR_I2C_LM75BDP_THYST_ABOVE_TOS, thyst, tos);
        return -1;
    }

    lm75bdp_handle_t lm = require_device((uint8_t)address);
    if (!lm) {
        return -1;
    }

    lm75bdp_report_t report = {0};
    esp_err_t err = lm75bdp_set_thresholds(lm, (float)tos, (float)thyst, &report);
    if (err != ESP_OK) {
        STRRES_ERROR(STR_I2C_LM75BDP_THRESHOLD_WRITE_FAILED,
                     address, esp_err_to_name(err), lm75bdp_stage_name(report.failed_stage));
        return -1;
    }

    /*
     * Report what was programmed, not what was asked for. Thresholds quantise
     * to 0.5 C and clamp to -128..+127.5 C, so the two differ often enough that
     * echoing the request back would be a lie about the part's behaviour.
     */
    STRRES_PRINTF(STR_I2C_LM75BDP_LIMITS_SET, address, report.tos_C, report.thyst_C);
    if (report.tos_C != (float)tos || report.thyst_C != (float)thyst) {
        STRRES_PRINTF(STR_I2C_LM75BDP_LIMITS_QUANTISED, tos, thyst);
    }
    return 0;
}
