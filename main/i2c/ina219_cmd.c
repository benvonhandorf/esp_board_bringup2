/*
 * Console commands for the INA219. The driver is the ina219 shared component.
 *
 * Same shape as ina237_cmd.c: a table of the parts the user has configured,
 * argument parsing, and every line of text. The differences are the part's,
 * not this file's -- the INA219 has no manufacturer or die ID to read, so a
 * wrong part is inferred from a calibration read-back, and it takes a
 * full-scale current because the shunt alone does not fix the resolution.
 */
#include "app_bringup.h"
#include "ina219_cmd.h"
#include "i2c.h"

#include "ina219.h"

/* A 0.1 ohm shunt and 3.2 A is the part's own reference case: 3.2 A across
 * 0.1 ohm is 320 mV, exactly the widest range the PGA offers. */
#define DEFAULT_SHUNT_OHMS   0.1
#define DEFAULT_MAX_CURRENT  3.2

/* The A0/A1 pins select one of sixteen addresses. */
#define INA219_ADDR_FIRST 0x40
#define INA219_ADDR_LAST  0x4f

/* Matches MAX_CACHED_DEVICES in i2c.c, so every configured part keeps a
 * cached device handle. */
#define MAX_DEVICES 8

typedef struct {
    bool used;
    uint8_t address;
    ina219_handle_t handle;
    double shunt_ohms;
    double max_current_a;
    ina219_report_t report;
} entry_t;

static entry_t devices[MAX_DEVICES];

static entry_t *find_device(uint8_t address)
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

/* Parse and bound an address argument, which every command shares. */
static int take_address(const char *token, int *out)
{
    if (cli_parse_num_arg(token, out) < 0 ||
        *out < INA219_ADDR_FIRST || *out > INA219_ADDR_LAST) {
        diag_error("Address must be 0x%02X-0x%02X (set by the A0/A1 pins)",
                   INA219_ADDR_FIRST, INA219_ADDR_LAST);
        return -1;
    }
    return 0;
}

static void report_stage(uint8_t address, const ina219_report_t *report,
                         esp_err_t err)
{
    switch (report->failed_stage) {
    case INA219_STAGE_NONE:
        return;
    case INA219_STAGE_RANGE:
        diag_error("0x%02X: that shunt and range cannot be represented. The "
                   "shunt drop at full scale must be between about 20 mV and "
                   "320 mV, which the PGA is the limit of.", address);
        return;
    case INA219_STAGE_IDENTIFY:
        diag_error("0x%02X does not behave like an INA219: the calibration "
                   "register did not read back what was written. The part has "
                   "no ID register, so this is the only identification there "
                   "is -- something else answers at this address.", address);
        return;
    default:
        diag_error("0x%02X: %s failed: %s", address,
                   ina219_stage_name(report->failed_stage), esp_err_to_name(err));
        return;
    }
}

/*
 * Point a handle at its device, re-fetching it from i2c.c every time.
 *
 * The handle cache there recycles wholesale when it fills and is emptied
 * outright by 'i2c bus', so a handle held across either would dangle.
 *
 * ina219_set_device() reconfigures the part as it attaches, which is wanted:
 * a part that has been power-cycled or reset since it was configured comes
 * back calibrated rather than reading plausible nonsense.
 */
static bool attach_device(entry_t *entry)
{
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = i2c_device_handle(entry->address, &dev);
    if (err != ESP_OK) {
        diag_error("Addressing 0x%02X: %s", entry->address, esp_err_to_name(err));
        return false;
    }

    err = ina219_set_device(entry->handle, dev, &entry->report);
    if (err != ESP_OK) {
        report_stage(entry->address, &entry->report, err);
        return false;
    }
    return true;
}

static int configure_device(uint8_t address, double shunt_ohms,
                            double max_current_a, bool quiet)
{
    entry_t *entry = find_device(address);
    ina219_handle_t handle = entry ? entry->handle : NULL;
    bool handle_is_new = false;
    ina219_report_t report = {0};

    if (!handle) {
        const ina219_config_t config = {
            .dev = NULL,
            .shunt_ohms = (float)shunt_ohms,
            .max_current_a = (float)max_current_a,
        };
        esp_err_t err = ina219_create(&config, &handle, &report);
        if (err != ESP_OK) {
            report_stage(address, &report, err);
            return -1;
        }
        handle_is_new = true;
    }

    /* Claim the slot only after the device has answered, so a full table is
     * reported as such rather than pre-empting "this is not an INA219". */
    if (!entry) {
        for (size_t i = 0; i < MAX_DEVICES; i++) {
            if (!devices[i].used) {
                entry = &devices[i];
                break;
            }
        }
        if (!entry) {
            diag_error("Cannot track more than %d INA219s", MAX_DEVICES);
            if (handle_is_new) {
                ina219_delete(handle);
            }
            return -1;
        }
    }

    entry->address = address;
    entry->handle = handle;
    entry->shunt_ohms = shunt_ohms;
    entry->max_current_a = max_current_a;
    entry->report = report;

    if (!attach_device(entry)) {
        if (handle_is_new) {
            ina219_delete(handle);
            entry->used = false;
        }
        return -1;
    }
    entry->used = true;

    if (!quiet) {
        diag_printf("0x%02X configured: shunt %.4f ohm, %.4f mA/LSB, range +/-%.3f A\n",
                    address, shunt_ohms, entry->report.current_lsb_a * 1000.0,
                    entry->report.full_scale_a);
        diag_printf("      CALIBRATION 0x%04X. The register's low bit is void, so "
                    "an odd value cannot be written and the resolution above is "
                    "what the part actually has, not what was asked for.\n",
                    entry->report.calibration);
    }

    return 0;
}

int cmd_ina219_config(int argc, char **argv)
{
    if (!i2c_require_bus()) {
        return -1;
    }

    if (argc < 2) {
        diag_printf("Usage: config <address> [shunt_ohms] [max_amps]\n");
        diag_printf("Address is 0x%02X-0x%02X; defaults are %.3f ohm and %.1f A.\n",
                    INA219_ADDR_FIRST, INA219_ADDR_LAST,
                    DEFAULT_SHUNT_OHMS, DEFAULT_MAX_CURRENT);
        return -1;
    }

    int address = 0;
    if (take_address(argv[1], &address) < 0) {
        return -1;
    }

    double shunt_ohms = DEFAULT_SHUNT_OHMS;
    if (argc > 2) {
        if (cli_parse_double_arg(argv[2], &shunt_ohms) < 0 || shunt_ohms <= 0.0) {
            diag_error("Shunt resistance must be a positive number of ohms, e.g. 0.1");
            return -1;
        }
    }

    double max_current = DEFAULT_MAX_CURRENT;
    if (argc > 3) {
        if (cli_parse_double_arg(argv[3], &max_current) < 0 || max_current <= 0.0) {
            diag_error("Full-scale current must be a positive number of amps, e.g. 3.2");
            return -1;
        }
    }

    return configure_device((uint8_t)address, shunt_ohms, max_current, false);
}

static int read_device(entry_t *entry)
{
    if (!attach_device(entry)) {
        return -1;
    }

    ina219_reading_t reading = {0};
    esp_err_t err = ina219_read(entry->handle, &reading);
    if (err != ESP_OK) {
        diag_error("Reading 0x%02X: %s", entry->address, esp_err_to_name(err));
        return -1;
    }

    diag_printf("0x%02X  Bus %8.3f V  Current %8.4f A  Power %8.3f W\n",
                entry->address, reading.bus_voltage, reading.current, reading.power);
    diag_printf("      Shunt %8.3f mV  %.4f mA/LSB over +/-%.3f A\n",
                reading.shunt_voltage * 1000.0,
                entry->report.current_lsb_a * 1000.0, entry->report.full_scale_a);
    return 0;
}

int cmd_ina219_read(int argc, char **argv)
{
    if (!i2c_require_bus()) {
        return -1;
    }

    if (argc > 1) {
        int address = 0;
        if (take_address(argv[1], &address) < 0) {
            return -1;
        }

        entry_t *entry = find_device((uint8_t)address);
        if (!entry) {
            /* Reading an address nobody configured is the common quick path;
             * register it at the defaults rather than refusing. */
            diag_printf("0x%02X is not configured; using %.3f ohm over %.1f A.\n",
                        address, DEFAULT_SHUNT_OHMS, DEFAULT_MAX_CURRENT);
            if (configure_device((uint8_t)address, DEFAULT_SHUNT_OHMS,
                                 DEFAULT_MAX_CURRENT, true) < 0) {
                return -1;
            }
            entry = find_device((uint8_t)address);
        }

        return read_device(entry);
    }

    if (configured_count() == 0) {
        diag_error("No INA219s configured. Run 'i2c-ina219 config <address> "
                   "[shunt_ohms] [max_amps]', or 'i2c-ina219 read <address>' "
                   "to use the defaults.");
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

int cmd_ina219_list(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (configured_count() == 0) {
        diag_printf("No INA219s configured.\n");
        return 0;
    }

    diag_printf("%-8s %12s %14s %14s %8s\n",
                "ADDRESS", "SHUNT (ohm)", "CURRENT_LSB", "FULL SCALE", "CAL");
    for (size_t i = 0; i < MAX_DEVICES; i++) {
        if (!devices[i].used) {
            continue;
        }
        diag_printf("0x%02X     %12.4f %11.4f mA %11.3f A   0x%04X\n",
                    devices[i].address, devices[i].shunt_ohms,
                    devices[i].report.current_lsb_a * 1000.0,
                    devices[i].report.full_scale_a,
                    devices[i].report.calibration);
    }

    return 0;
}
