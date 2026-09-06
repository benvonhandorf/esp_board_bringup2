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
        STRRES_ERROR(STR_I2C_PI4IOE_NOT_INITIALIZED);
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
        STRRES_ERROR(STR_I2C_PI4IOE_ADDRESSING_FAILED, handle_address);
        return false;
    }
    pi4ioe5v6408_set_device(handle, dev, NULL);
    return true;
}

static int take_byte(const char *token, int *out)
{
    if (cli_parse_num_arg(token, out) < 0 || *out < 0 || *out > 0xff) {
        STRRES_ERROR(STR_I2C_PI4IOE_BYTE_INVALID);
        return -1;
    }
    return 0;
}

static void print_bits(const char *label, uint8_t value)
{
    STRRES_PRINTF(STR_I2C_PI4IOE_PORT_VALUE, label, value);
    for (int bit = 7; bit >= 0; bit--) {
        diag_printf("%d", (value >> bit) & 1);
    }
    STRRES_PRINTF(STR_I2C_PI4IOE_BIT_ORDER);
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
            STRRES_ERROR(STR_I2C_PI4IOE_ADDRESS_RANGE, PI4IOE_ADDR_FIRST, PI4IOE_ADDR_LAST);
            return -1;
        }
        index++;
    }

    while (index < argc) {
        const char *token = argv[index];
        if (index + 1 >= argc) {
            STRRES_ERROR(STR_I2C_PI4IOE_OPTION_NEEDS_MASK, token);
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
            STRRES_ERROR(STR_I2C_PI4IOE_UNKNOWN_OPTION, token);
            return -1;
        }
        index += 2;
    }

    i2c_master_dev_handle_t dev = NULL;
    if (i2c_device_handle((uint8_t)address, &dev) != ESP_OK) {
        STRRES_ERROR(STR_I2C_PI4IOE_ADDRESSING_FAILED, address);
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
        STRRES_ERROR(STR_I2C_PI4IOE_STAGE_FAILED,
                     address, pi4ioe5v6408_stage_name(report.failed_stage),
                     esp_err_to_name(err), report.control_register);
        if (err == ESP_ERR_INVALID_RESPONSE || err == ESP_ERR_NOT_FOUND ||
            err == ESP_ERR_TIMEOUT) {
            /* The overwhelmingly common case on a bench: nothing is at this
             * address at all. The stage name alone reads as though a present
             * part misbehaved. */
            STRRES_ERROR(STR_I2C_PI4IOE_NO_ACKNOWLEDGE, address);
        }
        handle = NULL;
        return -1;
    }

    handle_address = (uint8_t)address;
    outputs_mask = outputs;

    STRRES_PRINTF(STR_I2C_PI4IOE_READY, address, report.control_register);
    print_bits("outputs:", outputs);
    print_bits("preset:", initial);
    print_bits("pulls on:", pull_enable);
    print_bits("pull up:", pull_up);
    print_bits("interrupt:", interrupt_on);
    STRRES_PRINTF(STR_I2C_PI4IOE_LEVELS_NOTE);
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
        STRRES_ERROR(STR_I2C_PI4IOE_PORT_READ_FAILED, esp_err_to_name(err));
        return -1;
    }

    print_bits("pins:", value);
    STRRES_PRINTF(STR_I2C_PI4IOE_LEVELS_NOT_REGISTER);
    return 0;
}

int cmd_pi4ioe_write(int argc, char **argv)
{
    if (!i2c_require_bus() || !require_init() || !reattach()) {
        return -1;
    }

    if (argc != 2) {
        STRRES_PRINTF(STR_I2C_PI4IOE_USAGE_WRITE);
        return -1;
    }

    int value = 0;
    if (take_byte(argv[1], &value) < 0) {
        return -1;
    }

    esp_err_t err = pi4ioe5v6408_write_port(handle, (uint8_t)value);
    if (err != ESP_OK) {
        STRRES_ERROR(STR_I2C_PI4IOE_PORT_WRITE_FAILED, esp_err_to_name(err));
        return -1;
    }

    STRRES_PRINTF(STR_I2C_PI4IOE_PORT_WRITTEN, value, outputs_mask);
    return 0;
}

int cmd_pi4ioe_set(int argc, char **argv)
{
    if (!i2c_require_bus() || !require_init() || !reattach()) {
        return -1;
    }

    if (argc != 3) {
        STRRES_PRINTF(STR_I2C_PI4IOE_USAGE_SET);
        return -1;
    }

    int pin = 0;
    if (cli_parse_int_arg(argv[1], &pin) < 0 || pin < 0 || pin > 7) {
        STRRES_ERROR(STR_I2C_PI4IOE_PIN_RANGE);
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
        STRRES_ERROR(STR_I2C_PI4IOE_STATE_INVALID);
        return -1;
    }

    esp_err_t err = pi4ioe5v6408_set_pin(handle, (uint8_t)pin, high);
    if (err != ESP_OK) {
        STRRES_ERROR(STR_I2C_PI4IOE_SET_FAILED, pin, esp_err_to_name(err));
        return -1;
    }

    if (!(outputs_mask & (1u << pin))) {
        STRRES_PRINTF(STR_I2C_PI4IOE_SET_BUT_INPUT, pin, high ? "high" : "low", pin);
        return 0;
    }

    STRRES_PRINTF(STR_I2C_PI4IOE_DRIVEN, pin, high ? "high" : "low");
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
        STRRES_ERROR(STR_I2C_PI4IOE_INTERRUPT_READ_FAILED, esp_err_to_name(err));
        return -1;
    }

    print_bits("changed:", status);
    if (status == 0) {
        STRRES_PRINTF(STR_I2C_PI4IOE_INTERRUPT_NONE);
    } else {
        STRRES_PRINTF(STR_I2C_PI4IOE_INTERRUPT_NOTE);
    }
    return 0;
}
