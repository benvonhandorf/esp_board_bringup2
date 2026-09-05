/*
 * Console commands for the AW9523B. The driver is the aw9523b shared component.
 *
 * Sixteen pins behind an I2C bus, which is exactly the thing a bring-up rig
 * wants to poke: the `gpio` group cannot reach them, so without this the
 * expander is a part you can see in `i2c scan` and do nothing with.
 *
 * Every pin defaults to an input. That is the part's own reset state and the
 * only safe assumption on a board whose pinout is not yet known -- an output
 * driven into whatever is on the other side is how a bring-up session damages
 * hardware.
 */
#include "app_bringup.h"
#include "aw9523b_cmd.h"
#include "i2c.h"

#include "aw9523b.h"

/* AD0 and AD1 select one of four addresses. */
#define AW9523B_ADDR_FIRST 0x58
#define AW9523B_ADDR_LAST  0x5b

static aw9523b_handle_t handle;
static uint8_t handle_address;
static uint8_t port_inputs[2] = {0xff, 0xff};

static bool require_init(void)
{
    if (!handle) {
        diag_error("AW9523B not initialized. Run 'i2c-aw9523b init [address]' first.");
        return false;
    }
    return true;
}

/* Re-fetched every command: the handle cache in i2c.c recycles wholesale when
 * it fills and is emptied outright by 'i2c bus'. */
static bool reattach(void)
{
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_device_handle(handle_address, &dev) != ESP_OK) {
        diag_error("Addressing 0x%02X failed", handle_address);
        return false;
    }
    aw9523b_set_device(handle, dev, NULL);
    return true;
}

static int take_port(const char *token, int *out)
{
    if (cli_parse_int_arg(token, out) < 0 || *out < 0 || *out > 1) {
        diag_error("Port must be 0 or 1");
        return -1;
    }
    return 0;
}

static int take_byte(const char *token, int *out)
{
    if (cli_parse_num_arg(token, out) < 0 || *out < 0 || *out > 0xff) {
        diag_error("Expected a byte, 0-255 or 0x00-0xff");
        return -1;
    }
    return 0;
}

int cmd_aw9523b_init(int argc, char **argv)
{
    if (!i2c_require_bus()) {
        return -1;
    }

    int address = AW9523B_I2C_ADDR_DEFAULT;
    uint8_t inputs[2] = {0xff, 0xff};
    uint8_t initial[2] = {0x00, 0x00};
    bool push_pull = false;

    int index = 1;
    if (index < argc && argv[index][0] != 'p' && argv[index][0] != 'i') {
        if (cli_parse_num_arg(argv[index], &address) < 0 ||
            address < AW9523B_ADDR_FIRST || address > AW9523B_ADDR_LAST) {
            diag_error("Address must be 0x%02X-0x%02X (set by the AD0/AD1 pins)",
                       AW9523B_ADDR_FIRST, AW9523B_ADDR_LAST);
            return -1;
        }
        index++;
    }

    while (index < argc) {
        const char *token = argv[index];
        if (strcasecmp(token, "pushpull") == 0) {
            push_pull = true;
            index++;
            continue;
        }
        if (index + 1 >= argc) {
            diag_error("'%s' needs a value", token);
            return -1;
        }
        int value = 0;
        if (strcasecmp(token, "p0in") == 0 || strcasecmp(token, "p1in") == 0 ||
            strcasecmp(token, "p0init") == 0 || strcasecmp(token, "p1init") == 0) {
            if (take_byte(argv[index + 1], &value) < 0) {
                return -1;
            }
            if (strcasecmp(token, "p0in") == 0)   inputs[0] = (uint8_t)value;
            if (strcasecmp(token, "p1in") == 0)   inputs[1] = (uint8_t)value;
            if (strcasecmp(token, "p0init") == 0) initial[0] = (uint8_t)value;
            if (strcasecmp(token, "p1init") == 0) initial[1] = (uint8_t)value;
            index += 2;
            continue;
        }
        diag_error("Unknown option '%s'. Expected p0in, p1in, p0init, p1init "
                   "or pushpull.", token);
        return -1;
    }

    i2c_master_dev_handle_t dev = NULL;
    if (i2c_device_handle((uint8_t)address, &dev) != ESP_OK) {
        diag_error("Addressing 0x%02X failed", address);
        return -1;
    }

    const aw9523b_config_t config = {
        .dev = dev,
        .port0_inputs = inputs[0],
        .port1_inputs = inputs[1],
        .port0_initial = initial[0],
        .port1_initial = initial[1],
        .port0_push_pull = push_pull,
    };

    if (handle) {
        aw9523b_delete(handle);
        handle = NULL;
    }

    aw9523b_report_t report = {0};
    esp_err_t err = aw9523b_create(&config, &handle, &report);
    if (err != ESP_OK) {
        if (report.failed_stage == AW9523B_STAGE_IDENTIFY) {
            diag_error("0x%02X is not an AW9523B: chip ID reads 0x%02X, expected 0x23",
                       address, report.chip_id);
        } else {
            diag_error("0x%02X: %s failed: %s", address,
                       aw9523b_stage_name(report.failed_stage), esp_err_to_name(err));
        }
        handle = NULL;
        return -1;
    }

    handle_address = (uint8_t)address;
    port_inputs[0] = inputs[0];
    port_inputs[1] = inputs[1];

    diag_printf("AW9523B ready at 0x%02X (chip ID 0x%02X)\n", address, report.chip_id);
    diag_printf("Port 0: inputs 0x%02X, outputs preset to 0x%02X, %s\n",
                inputs[0], initial[0],
                push_pull ? "push-pull" : "open-drain (add 'pushpull' to drive high)");
    diag_printf("Port 1: inputs 0x%02X, outputs preset to 0x%02X, push-pull\n",
                inputs[1], initial[1]);
    diag_printf("Levels were established before any pin became an output, so "
                "nothing glitched to whatever the register held.\n");
    return 0;
}

int cmd_aw9523b_read(int argc, char **argv)
{
    if (!i2c_require_bus() || !require_init() || !reattach()) {
        return -1;
    }

    int only_port = -1;
    if (argc > 1 && take_port(argv[1], &only_port) < 0) {
        return -1;
    }

    for (int port = 0; port < 2; port++) {
        if (only_port >= 0 && port != only_port) {
            continue;
        }
        uint8_t value = 0;
        esp_err_t err = aw9523b_read_port(handle, (uint8_t)port, &value);
        if (err != ESP_OK) {
            diag_error("Reading port %d: %s", port, esp_err_to_name(err));
            return -1;
        }
        diag_printf("P%d: 0x%02X  ", port, value);
        for (int bit = 7; bit >= 0; bit--) {
            diag_printf("%d", (value >> bit) & 1);
        }
        diag_printf("  (P%d.7 first; inputs 0x%02X)\n", port, port_inputs[port]);
    }

    /*
     * The part has no readable output register, so what comes back is the pin
     * levels. On an output pin that is what the pin is actually at, which may
     * differ from what was driven if something else is holding it.
     */
    diag_printf("These are pin levels, not the output register -- the part has "
                "none to read.\n");
    return 0;
}

int cmd_aw9523b_write(int argc, char **argv)
{
    if (!i2c_require_bus() || !require_init() || !reattach()) {
        return -1;
    }

    if (argc != 3) {
        diag_printf("Usage: write <port> <value>\n");
        return -1;
    }

    int port = 0, value = 0;
    if (take_port(argv[1], &port) < 0 || take_byte(argv[2], &value) < 0) {
        return -1;
    }

    esp_err_t err = aw9523b_write_port(handle, (uint8_t)port, (uint8_t)value);
    if (err != ESP_OK) {
        diag_error("Writing port %d: %s", port, esp_err_to_name(err));
        return -1;
    }

    diag_printf("P%d driven to 0x%02X; pins configured as inputs (0x%02X) ignore it\n",
                port, value, port_inputs[port]);
    return 0;
}

int cmd_aw9523b_set(int argc, char **argv)
{
    if (!i2c_require_bus() || !require_init() || !reattach()) {
        return -1;
    }

    if (argc != 4) {
        diag_printf("Usage: set <port> <pin> <0|1>\n");
        return -1;
    }

    int port = 0, pin = 0;
    if (take_port(argv[1], &port) < 0) {
        return -1;
    }
    if (cli_parse_int_arg(argv[2], &pin) < 0 || pin < 0 || pin > 7) {
        diag_error("Pin must be 0-7 within the port");
        return -1;
    }

    bool high;
    if (strcasecmp(argv[3], "1") == 0 || strcasecmp(argv[3], "true") == 0 ||
        strcasecmp(argv[3], "high") == 0 || strcasecmp(argv[3], "on") == 0) {
        high = true;
    } else if (strcasecmp(argv[3], "0") == 0 || strcasecmp(argv[3], "false") == 0 ||
               strcasecmp(argv[3], "low") == 0 || strcasecmp(argv[3], "off") == 0) {
        high = false;
    } else {
        diag_error("State must be 0/1, low/high, off/on or false/true");
        return -1;
    }

    /*
     * set_pin() works from the driver's shadow of the last value written, not
     * from a read-modify-write: reading a port gives input levels, so folding
     * them back would write pin state over the outputs.
     */
    esp_err_t err = aw9523b_set_pin(handle, (uint8_t)port, (uint8_t)pin, high);
    if (err != ESP_OK) {
        diag_error("Setting P%d.%d: %s", port, pin, esp_err_to_name(err));
        return -1;
    }

    if (port_inputs[port] & (1u << pin)) {
        diag_printf("P%d.%d set %s, but it is configured as an input and will "
                    "not drive. Re-run 'i2c-aw9523b init' with a p%din mask "
                    "that clears bit %d.\n", port, pin, high ? "high" : "low",
                    port, pin);
        return 0;
    }

    diag_printf("P%d.%d driven %s\n", port, pin, high ? "high" : "low");
    return 0;
}
