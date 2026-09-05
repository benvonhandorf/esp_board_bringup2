/*
 * Console commands for the INA237. The driver is the ina237 shared component.
 *
 * What stays here is everything the component deliberately does not do: which
 * parts the user has configured, how an address argument is parsed and bounded,
 * and every line of text. The datasheet lives on the other side of the API.
 */
#include "app_bringup.h"
#include "ina237_cmd.h"
#include "i2c.h"

#include "ina237.h"

#define DEFAULT_SHUNT_OHMS 0.004

/* Matches MAX_CACHED_DEVICES in i2c.c, so every configured part keeps a
 * cached device handle. */
#define MAX_DEVICES 8

/*
 * Which parts the user has configured.
 *
 * This is console state, not device state: the component knows one INA237 at a
 * time and is told which one, while "the set of parts on this board" is a fact
 * about this session. Each entry owns a handle for the life of the entry.
 */
typedef struct {
    bool used;
    uint8_t address;
    ina237_handle_t handle;
} ina237_entry_t;

static ina237_entry_t devices[MAX_DEVICES];

static ina237_entry_t *find_device(uint8_t address)
{
    for (size_t i = 0; i < MAX_DEVICES; i++) {
        if (devices[i].used && devices[i].address == address) {
            return &devices[i];
        }
    }
    return NULL;
}

static size_t configured_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < MAX_DEVICES; i++) {
        if (devices[i].used) {
            count++;
        }
    }
    return count;
}

/*
 * Point a handle at its device, re-fetching it from i2c.c every time.
 *
 * The handle cache there recycles wholesale when it fills and is emptied
 * outright by 'i2c bus', so a handle held across either would dangle.
 */
static bool attach_device(ina237_handle_t handle, uint8_t address)
{
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = i2c_device_handle(address, &dev);
    if (err != ESP_OK) {
        diag_error("Addressing 0x%02X: %s", address, esp_err_to_name(err));
        return false;
    }

    ina237_set_device(handle, dev);
    return true;
}

/*
 * Register a part: confirm something INA237-shaped is really there, program
 * the calibration, and remember the shunt value. Re-running on a known address
 * updates it in place.
 *
 * The table slot is claimed only after the device has answered, so a full table
 * is reported as such rather than pre-empting the more useful "this is not an
 * INA237".
 */
static int configure_device(uint8_t address, double shunt_ohms, bool quiet)
{
    ina237_entry_t *entry = find_device(address);
    ina237_handle_t handle = entry ? entry->handle : NULL;
    bool handle_is_new = false;

    if (!handle) {
        const ina237_config_t config = {.dev = NULL, .shunt_ohms = shunt_ohms};
        esp_err_t err = ina237_create(&config, &handle);
        if (err != ESP_OK) {
            diag_error("Addressing 0x%02X: %s", address, esp_err_to_name(err));
            return -1;
        }
        handle_is_new = true;
    }

    ina237_config_report_t report = {0};
    esp_err_t err = ESP_FAIL;

    if (attach_device(handle, address)) {
        err = ina237_configure(handle, shunt_ohms, &report);
        switch (report.failed_stage) {
        case INA237_STAGE_NONE:
            break;
        case INA237_STAGE_PROBE:
            diag_error("No response from 0x%02X: %s", address, esp_err_to_name(err));
            break;
        case INA237_STAGE_IDENTIFY:
            diag_error("0x%02X is not an INA237: MANUFACTURER_ID reads 0x%04X, expected 0x%04X",
                     address, report.manufacturer_id, INA237_MANUFACTURER_ID_TI);
            break;
        case INA237_STAGE_SHUNT_CAL:
            diag_error("Writing SHUNT_CAL to 0x%02X: %s", address, esp_err_to_name(err));
            break;
        }
    }

    if (err != ESP_OK) {
        if (handle_is_new) {
            ina237_delete(handle);
        }
        return -1;
    }

    if (!entry) {
        for (size_t i = 0; i < MAX_DEVICES; i++) {
            if (!devices[i].used) {
                entry = &devices[i];
                break;
            }
        }
        if (!entry) {
            diag_error("Cannot track more than %d INA237s", MAX_DEVICES);
            if (handle_is_new) {
                ina237_delete(handle);
            }
            return -1;
        }
    }

    entry->used = true;
    entry->address = address;
    entry->handle = handle;

    if (!quiet) {
        diag_printf("0x%02X configured: shunt %.4f ohm, %.4f mA/LSB, range +/-%.2f A\n",
                  address, shunt_ohms, report.current_lsb * 1000.0,
                  report.full_scale_amps);
    }

    return 0;
}

/* Parse and bound an address argument, which every command shares. */
static int take_address(const char *token, int *out)
{
    if (cli_parse_num_arg(token, out) < 0 ||
        *out < INA237_ADDR_FIRST || *out > INA237_ADDR_LAST) {
        diag_error("Address must be 0x%02X-0x%02X (set by the A0/A1 pins)",
                 INA237_ADDR_FIRST, INA237_ADDR_LAST);
        return -1;
    }
    return 0;
}

int cmd_ina237_config(int argc, char **argv)
{
    if (!i2c_require_bus()) {
        return -1;
    }

    if (argc < 2) {
        diag_printf("Usage: config <address> [shunt_ohms]\n");
        diag_printf("Address is 0x%02X-0x%02X; shunt defaults to %.3f ohm.\n",
                  INA237_ADDR_FIRST, INA237_ADDR_LAST, DEFAULT_SHUNT_OHMS);
        return -1;
    }

    int address = 0;
    if (take_address(argv[1], &address) < 0) {
        return -1;
    }

    double shunt_ohms = DEFAULT_SHUNT_OHMS;
    if (argc > 2) {
        if (cli_parse_double_arg(argv[2], &shunt_ohms) < 0 || shunt_ohms <= 0.0) {
            diag_error("Shunt resistance must be a positive number of ohms, e.g. 0.004");
            return -1;
        }
    }

    return configure_device((uint8_t)address, shunt_ohms, false);
}

static void report_health(const ina237_reading_t *reading)
{
    if (!reading->trim_checksum_ok) {
        diag_error("      Trim memory checksum error (DIAG_ALRT.MEMSTAT); "
                 "readings cannot be trusted");
    }
    if (reading->math_overflow) {
        diag_error("      Arithmetic overflow (DIAG_ALRT.MATHOF); current and "
                 "power are invalid");
    }
    if (!reading->shunt_cal_matches) {
        diag_error("      SHUNT_CAL reads %u, expected %d; the device has been "
                 "reset since it was configured. Re-run 'i2c-ina237 config'.",
                 reading->shunt_cal, INA237_SHUNT_CAL_VALUE);
    }
}

static int read_device(const ina237_entry_t *entry)
{
    if (!attach_device(entry->handle, entry->address)) {
        return -1;
    }

    ina237_reading_t reading = {0};
    esp_err_t err = ina237_read(entry->handle, &reading);
    if (err != ESP_OK) {
        diag_error("Reading 0x%02X: %s", entry->address, esp_err_to_name(err));
        return -1;
    }

    diag_printf("0x%02X  Bus %8.3f V  Current %8.4f A  Power %8.3f W\n",
              entry->address, reading.bus_v, reading.current_a, reading.power_w);
    diag_printf("      Shunt %8.3f mV  Temp %5.1f C\n",
              reading.shunt_v * 1000.0, reading.temp_c);
    diag_printf("      SHUNT_CAL %u  CURRENT_LSB %.4f mA  shunt %.4f ohm\n",
              reading.shunt_cal, ina237_current_lsb(entry->handle) * 1000.0,
              ina237_shunt_ohms(entry->handle));

    report_health(&reading);
    return 0;
}

int cmd_ina237_read(int argc, char **argv)
{
    if (!i2c_require_bus()) {
        return -1;
    }

    if (argc > 1) {
        int address = 0;
        if (take_address(argv[1], &address) < 0) {
            return -1;
        }

        ina237_entry_t *entry = find_device((uint8_t)address);
        if (!entry) {
            /* Reading an address nobody configured is the common quick path;
             * register it at the default shunt rather than refusing. */
            diag_printf("0x%02X is not configured; using the default %.3f ohm shunt.\n",
                      address, DEFAULT_SHUNT_OHMS);
            if (configure_device((uint8_t)address, DEFAULT_SHUNT_OHMS, true) < 0) {
                return -1;
            }
            entry = find_device((uint8_t)address);
        }

        return read_device(entry);
    }

    if (configured_count() == 0) {
        diag_error("No INA237s configured. Run 'i2c-ina237 config <address> [ohms]', "
                 "or 'i2c-ina237 read <address>' to use the default shunt.");
        return -1;
    }

    int failures = 0;
    for (size_t i = 0; i < MAX_DEVICES; i++) {
        if (devices[i].used && read_device(&devices[i]) < 0) {
            failures++;
        }
    }

    return failures ? -1 : 0;
}

int cmd_ina237_list(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (configured_count() == 0) {
        diag_printf("No INA237s configured.\n");
        return 0;
    }

    diag_printf("%-8s %12s %14s %14s\n", "ADDRESS", "SHUNT (ohm)", "CURRENT_LSB", "FULL SCALE");
    for (size_t i = 0; i < MAX_DEVICES; i++) {
        if (!devices[i].used) {
            continue;
        }
        diag_printf("0x%02X     %12.4f %11.4f mA %11.2f A\n",
                  devices[i].address,
                  ina237_shunt_ohms(devices[i].handle),
                  ina237_current_lsb(devices[i].handle) * 1000.0,
                  ina237_full_scale_amps(devices[i].handle));
    }

    return 0;
}
