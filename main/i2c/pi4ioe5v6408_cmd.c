/*
 * Console commands for the PI4IOE5V6408. The driver is the pi4ioe5v6408 shared
 * component.
 *
 * Eight pins, 5 V tolerant, with pulls and interrupt-on-change the AW9523B does
 * not have -- which is why both parts are here rather than one standing in for
 * the other. Same default as the AW9523B: everything an input until told
 * otherwise, because an output driven into an unknown board is how a bring-up
 * session breaks hardware.
 */
#include "app_bringup.h"
#include "pi4ioe5v6408_cmd.h"
#include "i2c.h"

#include "pi4ioe5v6408.h"

/* ADDR to GND or pulled high. */
#define PI4IOE_ADDR_FIRST 0x43
#define PI4IOE_ADDR_LAST  0x44

static pi4ioe5v6408_handle_t handle;
static uint8_t handle_address;
static uint8_t outputs_mask;

static bool require_init(void)
{
    if (!handle) {
        diag_error("PI4IOE5V6408 not initialized. Run 'i2c-pi4ioe init [address]' first.");
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
    pi4ioe5v6408_set_device(handle, dev, NULL);
    return true;
}

static int take_byte(const char *token, int *out)
{
    if (cli_parse_num_arg(token, out) < 0 || *out < 0 || *out > 0xff) {
        diag_error("Expected a byte, 0-255 or 0x00-0xff");
        return -1;
    }
    return 0;
}

static void print_bits(const char *label, uint8_t value)
{
    diag_printf("%-11s 0x%02X  ", label, value);
    for (int bit = 7; bit >= 0; bit--) {
        diag_printf("%d", (value >> bit) & 1);
    }
    diag_printf("   (P7 first)\n");
}

int cmd_pi4ioe_init(int argc, char **argv)
{
    if (!i2c_require_bus()) {
        return -1;
    }

    int address = PI4IOE5V6408_I2C_ADDR_DEFAULT;
    uint8_t outputs = 0x00, initial = 0x00;
    uint8_t pull_enable = 0x00, pull_up = 0x00, interrupt_on = 0x00;

    /* The address is optional and leading, so a first argument that is not one
     * of the option keywords is taken as the address. */
    static const char *const options[] = {"out", "init", "pull", "pullup", "int"};
    int index = 1;
    bool first_is_option = false;
    for (size_t i = 0; index < argc && i < sizeof(options) / sizeof(options[0]); i++) {
        if (strcasecmp(argv[index], options[i]) == 0) {
            first_is_option = true;
            break;
        }
    }
    if (index < argc && !first_is_option) {
        if (cli_parse_num_arg(argv[index], &address) < 0 ||
            address < PI4IOE_ADDR_FIRST || address > PI4IOE_ADDR_LAST) {
            diag_error("Address must be 0x%02X or 0x%02X (set by the ADDR pin)",
                       PI4IOE_ADDR_FIRST, PI4IOE_ADDR_LAST);
            return -1;
        }
        index++;
    }

    while (index < argc) {
        const char *token = argv[index];
        if (index + 1 >= argc) {
            diag_error("'%s' needs a mask", token);
            return -1;
        }
        int value = 0;
        if (take_byte(argv[index + 1], &value) < 0) {
            return -1;
        }
        if (strcasecmp(token, "out") == 0)         outputs = (uint8_t)value;
        else if (strcasecmp(token, "init") == 0)   initial = (uint8_t)value;
        else if (strcasecmp(token, "pull") == 0)   pull_enable = (uint8_t)value;
        else if (strcasecmp(token, "pullup") == 0) pull_up = (uint8_t)value;
        else if (strcasecmp(token, "int") == 0)    interrupt_on = (uint8_t)value;
        else {
            diag_error("Unknown option '%s'. Expected out, init, pull, pullup or int.",
                       token);
            return -1;
        }
        index += 2;
    }

    i2c_master_dev_handle_t dev = NULL;
    if (i2c_device_handle((uint8_t)address, &dev) != ESP_OK) {
        diag_error("Addressing 0x%02X failed", address);
        return -1;
    }

    const pi4ioe5v6408_config_t config = {
        .dev = dev,
        .outputs = outputs,
        .initial = initial,
        .pull_enable = pull_enable,
        .pull_up = pull_up,
        .interrupt_on = interrupt_on,
    };

    if (handle) {
        pi4ioe5v6408_delete(handle);
        handle = NULL;
    }

    pi4ioe5v6408_report_t report = {0};
    esp_err_t err = pi4ioe5v6408_create(&config, &handle, &report);
    if (err != ESP_OK) {
        diag_error("0x%02X: %s failed: %s (control register 0x%02X)", address,
                   pi4ioe5v6408_stage_name(report.failed_stage),
                   esp_err_to_name(err), report.control_register);
        if (err == ESP_ERR_INVALID_RESPONSE || err == ESP_ERR_NOT_FOUND ||
            err == ESP_ERR_TIMEOUT) {
            /* The overwhelmingly common case on a bench: nothing is at this
             * address at all. The stage name alone reads as though a present
             * part misbehaved. */
            diag_error("Nothing acknowledged at 0x%02X. 'i2c scan' lists what "
                       "is actually on the bus.", address);
        }
        handle = NULL;
        return -1;
    }

    handle_address = (uint8_t)address;
    outputs_mask = outputs;

    diag_printf("PI4IOE5V6408 ready at 0x%02X (control register 0x%02X)\n",
                address, report.control_register);
    print_bits("outputs:", outputs);
    print_bits("preset:", initial);
    print_bits("pulls on:", pull_enable);
    print_bits("pull up:", pull_up);
    print_bits("interrupt:", interrupt_on);
    diag_printf("Levels were established before any pin became an output. A "
                "switch to ground with no external pull-up needs its bit set in "
                "both 'pull' and 'pullup'.\n");
    return 0;
}

int cmd_pi4ioe_read(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!i2c_require_bus() || !require_init() || !reattach()) {
        return -1;
    }

    uint8_t value = 0;
    esp_err_t err = pi4ioe5v6408_read_port(handle, &value);
    if (err != ESP_OK) {
        diag_error("Reading the port: %s", esp_err_to_name(err));
        return -1;
    }

    print_bits("pins:", value);
    diag_printf("These are pin levels, not the output register. On an output "
                "pin the level can differ from what was driven if something "
                "else is holding it.\n");
    return 0;
}

int cmd_pi4ioe_write(int argc, char **argv)
{
    if (!i2c_require_bus() || !require_init() || !reattach()) {
        return -1;
    }

    if (argc != 2) {
        diag_printf("Usage: write <value>\n");
        return -1;
    }

    int value = 0;
    if (take_byte(argv[1], &value) < 0) {
        return -1;
    }

    esp_err_t err = pi4ioe5v6408_write_port(handle, (uint8_t)value);
    if (err != ESP_OK) {
        diag_error("Writing the port: %s", esp_err_to_name(err));
        return -1;
    }

    diag_printf("Port driven to 0x%02X; pins outside the output mask 0x%02X "
                "ignore it\n", value, outputs_mask);
    return 0;
}

int cmd_pi4ioe_set(int argc, char **argv)
{
    if (!i2c_require_bus() || !require_init() || !reattach()) {
        return -1;
    }

    if (argc != 3) {
        diag_printf("Usage: set <pin> <0|1>\n");
        return -1;
    }

    int pin = 0;
    if (cli_parse_int_arg(argv[1], &pin) < 0 || pin < 0 || pin > 7) {
        diag_error("Pin must be 0-7");
        return -1;
    }

    bool high;
    if (strcasecmp(argv[2], "1") == 0 || strcasecmp(argv[2], "true") == 0 ||
        strcasecmp(argv[2], "high") == 0 || strcasecmp(argv[2], "on") == 0) {
        high = true;
    } else if (strcasecmp(argv[2], "0") == 0 || strcasecmp(argv[2], "false") == 0 ||
               strcasecmp(argv[2], "low") == 0 || strcasecmp(argv[2], "off") == 0) {
        high = false;
    } else {
        diag_error("State must be 0/1, low/high, off/on or false/true");
        return -1;
    }

    esp_err_t err = pi4ioe5v6408_set_pin(handle, (uint8_t)pin, high);
    if (err != ESP_OK) {
        diag_error("Setting P%d: %s", pin, esp_err_to_name(err));
        return -1;
    }

    if (!(outputs_mask & (1u << pin))) {
        diag_printf("P%d set %s, but it is an input and will not drive. Re-run "
                    "'i2c-pi4ioe init' with an 'out' mask that sets bit %d.\n",
                    pin, high ? "high" : "low", pin);
        return 0;
    }

    diag_printf("P%d driven %s\n", pin, high ? "high" : "low");
    return 0;
}

int cmd_pi4ioe_interrupt(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!i2c_require_bus() || !require_init() || !reattach()) {
        return -1;
    }

    uint8_t status = 0;
    esp_err_t err = pi4ioe5v6408_read_interrupt_status(handle, &status);
    if (err != ESP_OK) {
        diag_error("Reading the interrupt status: %s", esp_err_to_name(err));
        return -1;
    }

    print_bits("changed:", status);
    if (status == 0) {
        diag_printf("Nothing has changed since this was last read.\n");
    } else {
        diag_printf("Reading this register clears it, so the next call reports "
                    "only what changes from here.\n");
    }
    return 0;
}
