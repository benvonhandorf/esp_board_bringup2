/*
 * Console commands for the SHT4x. The driver is the sht4x shared component.
 *
 * What stays here is everything the component deliberately does not do: how an
 * optional leading address argument is told apart from an operand, and every
 * line of text. The datasheet lives on the other side of the API.
 */
#include "app_bringup.h"
#include "sht4x_cmd.h"
#include "i2c.h"

#include "sht4x.h"

/*
 * One handle, re-pointed per command.
 *
 * The part carries no per-device state -- the address is an argument on every
 * command -- so there is nothing to keep per address, unlike the INA237 with
 * its shunt value. Created lazily on first use and never deleted.
 */
static sht4x_handle_t sht;

static bool ensure_handle(void)
{
    if (sht) {
        return true;
    }

    const sht4x_config_t config = {.dev = NULL};
    esp_err_t err = sht4x_create(&config, &sht);
    if (err != ESP_OK) {
        diag_error("Cannot create the SHT4x driver: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

/*
 * Point the handle at an address, re-fetching the device handle from i2c.c
 * every time.
 *
 * The handle cache there recycles wholesale when it fills and is emptied
 * outright by 'i2c bus', so a handle held across either would dangle.
 */
static esp_err_t attach_device(uint8_t address)
{
    if (!ensure_handle()) {
        return ESP_ERR_NO_MEM;
    }

    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = i2c_device_handle(address, &dev);
    if (err != ESP_OK) {
        return err;
    }

    sht4x_set_device(sht, dev);
    return ESP_OK;
}

/*
 * Parse an optional leading address argument.
 *
 * Returns the index of the first argument that is not the address, so callers
 * accept "read", "read 0x45" and "read low" alike.
 *
 * `required_after` is how many arguments the command still needs once the
 * address has been taken. It is what separates "heater 200 1000" (no address,
 * 200 is the power) from "heater 0x45 200 1000" -- without it, a leading
 * numeric argument is indistinguishable from an omitted address.
 */
static int take_address(int argc, char **argv, int required_after,
                        uint8_t *address, bool *ok)
{
    *address = SHT4X_ADDR_DEFAULT;
    *ok = true;

    if (argc < 2) {
        return 1;
    }

    /* Too few arguments for argv[1] to be an address AND leave the command its
     * required operands, so it must be an operand itself. */
    if (argc - 2 < required_after) {
        return 1;
    }

    int value = 0;
    if (cli_parse_num_arg(argv[1], &value) < 0) {
        return 1; /* not a number; leave it for the caller to interpret */
    }

    if (value < SHT4X_ADDR_FIRST || value > SHT4X_ADDR_LAST) {
        diag_error("Address must be 0x%02X-0x%02X (fixed by the part variant: "
                 "A=0x44, B=0x45, C=0x46)", SHT4X_ADDR_FIRST, SHT4X_ADDR_LAST);
        *ok = false;
        return 1;
    }

    *address = (uint8_t)value;
    return 2;
}

/* The CRC detail the driver hands back, in the words docs/i2c.md promises. */
static void report_crc_error(uint8_t address, const sht4x_crc_error_t *crc)
{
    if (crc->word == 1) {
        diag_error("0x%02X: CRC error in the first word (got 0x%02X, computed 0x%02X)",
                 address, crc->received, crc->computed);
    } else {
        diag_error("0x%02X: CRC error in the second word (got 0x%02X, computed 0x%02X)",
                 address, crc->received, crc->computed);
    }
}

/* Shared by `read` and `heater`, both of which return a measurement. */
static int report_measurement(uint8_t address, const sht4x_measurement_t *m,
                              esp_err_t err, const char *description)
{
    if (err == ESP_ERR_SHT4X_CRC) {
        report_crc_error(address, &m->crc_error);
        return -1;
    }
    if (err != ESP_OK) {
        diag_error("Reading 0x%02X: %s", address, esp_err_to_name(err));
        return -1;
    }

    diag_printf("0x%02X  Temp %7.2f C (%7.2f F)  Humidity %6.2f %%RH\n",
              address, m->temperature_c, m->temperature_c * 9.0 / 5.0 + 32.0,
              m->humidity_pct_cropped);
    diag_printf("      raw T 0x%04X  RH 0x%04X  (%s)\n",
              m->temperature_ticks, m->humidity_ticks, description);

    /* The conversion can yield non-physical values just outside 0-100 %RH.
     * The datasheet expects those to be cropped, but say so when it happens:
     * during bringup a wildly out-of-range value means a real problem. */
    if (m->humidity_was_cropped) {
        diag_printf("      Note: uncropped humidity was %.2f %%RH, cropped to the "
                  "physical 0-100 range\n", m->humidity_pct);
    }

    return 0;
}

int cmd_sht4x_read(int argc, char **argv)
{
    if (!i2c_require_bus()) {
        return -1;
    }

    bool ok = false;
    uint8_t address = 0;
    int index = take_address(argc, argv, 0, &address, &ok);
    if (!ok) {
        return -1;
    }

    sht4x_repeatability_t repeatability = SHT4X_REPEATABILITY_HIGH;

    if (index < argc) {
        const char *mode = argv[index];
        if (sht4x_repeatability_from_name(mode, &repeatability) != ESP_OK) {
            diag_error("Repeatability must be high, medium or low (got '%s')", mode);
            return -1;
        }
    }

    /* "high repeatability", "medium repeatability", "low repeatability" -- the
     * driver owns the word, this owns the sentence it sits in. */
    char description[32];
    snprintf(description, sizeof(description), "%s repeatability",
             sht4x_repeatability_name(repeatability));

    sht4x_measurement_t measurement = {0};
    esp_err_t err = attach_device(address);
    if (err == ESP_OK) {
        err = sht4x_measure(sht, repeatability, &measurement);
    }
    return report_measurement(address, &measurement, err, description);
}

int cmd_sht4x_serial(int argc, char **argv)
{
    if (!i2c_require_bus()) {
        return -1;
    }

    bool ok = false;
    uint8_t address = 0;
    take_address(argc, argv, 0, &address, &ok);
    if (!ok) {
        return -1;
    }

    uint32_t serial = 0;
    sht4x_crc_error_t crc = {0};
    esp_err_t err = attach_device(address);
    if (err == ESP_OK) {
        err = sht4x_read_serial(sht, &serial, &crc);
    }
    if (err == ESP_ERR_SHT4X_CRC) {
        report_crc_error(address, &crc);
        return -1;
    }
    if (err != ESP_OK) {
        diag_error("No response from 0x%02X: %s", address, esp_err_to_name(err));
        return -1;
    }

    /* The SHT4x has no ID register; a serial number that reads back with valid
     * CRCs is the available evidence that a real sensor is present. */
    diag_printf("0x%02X  Serial number 0x%04X%04X\n", address,
              (unsigned)(serial >> 16), (unsigned)(serial & 0xFFFF));
    return 0;
}

int cmd_sht4x_heater(int argc, char **argv)
{
    if (!i2c_require_bus()) {
        return -1;
    }

    bool ok = false;
    uint8_t address = 0;
    int index = take_address(argc, argv, 2, &address, &ok);
    if (!ok) {
        return -1;
    }

    if (argc - index < 2) {
        diag_printf("Usage: heater [address] <power_mw> <duration_ms>\n");
        diag_printf("Power is 20, 110 or 200 mW; duration is 100 or 1000 ms.\n");
        return -1;
    }

    int power = 0;
    int duration = 0;
    if (cli_parse_int_arg(argv[index], &power) < 0 ||
        cli_parse_int_arg(argv[index + 1], &duration) < 0) {
        diag_error("Power and duration must be numbers");
        return -1;
    }

    /* Only the six combinations the device implements are available. The
     * duration is checked first so that a bad duration is named as such rather
     * than being reported as a bad power. */
    if (duration != 1000 && duration != 100) {
        diag_error("Duration must be 100 or 1000 ms");
        return -1;
    }

    sht4x_heater_t heater;
    if (sht4x_heater_from_values(power, duration, &heater) != ESP_OK) {
        diag_error("Power must be 20, 110 or 200 mW");
        return -1;
    }

    diag_printf("Running the heater at %d mW for %d ms; the sensor measures once "
              "just before it switches off.\n", power, duration);

    sht4x_measurement_t measurement = {0};
    esp_err_t err = attach_device(address);
    if (err == ESP_OK) {
        err = sht4x_run_heater(sht, heater, &measurement);
    }
    return report_measurement(address, &measurement, err, "after heating");
}

int cmd_sht4x_reset(int argc, char **argv)
{
    if (!i2c_require_bus()) {
        return -1;
    }

    bool ok = false;
    uint8_t address = 0;
    take_address(argc, argv, 0, &address, &ok);
    if (!ok) {
        return -1;
    }

    esp_err_t err = attach_device(address);
    if (err == ESP_OK) {
        err = sht4x_soft_reset(sht);
    }
    if (err != ESP_OK) {
        diag_error("Resetting 0x%02X: %s", address, esp_err_to_name(err));
        return -1;
    }

    diag_printf("0x%02X soft reset\n", address);
    return 0;
}
