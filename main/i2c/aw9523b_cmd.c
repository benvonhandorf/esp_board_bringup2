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
        STRRES_ERROR(STR_I2C_AW9523B_NOT_INITIALIZED);
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
        STRRES_ERROR(STR_I2C_AW9523B_ADDRESSING_FAILED, handle_address);
        return false;
    }
    aw9523b_set_device(handle, dev, NULL);
    return true;
}

static int take_port(const char *token, int *out)
{
    if (cli_parse_int_arg(token, out) < 0 || *out < 0 || *out > 1) {
        STRRES_ERROR(STR_I2C_AW9523B_PORT_INVALID);
        return -1;
    }
    return 0;
}

static int take_byte(const char *token, int *out)
{
    if (cli_parse_num_arg(token, out) < 0 || *out < 0 || *out > 0xff) {
        STRRES_ERROR(STR_I2C_AW9523B_BYTE_INVALID);
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
            STRRES_ERROR(STR_I2C_AW9523B_ADDRESS_RANGE, AW9523B_ADDR_FIRST, AW9523B_ADDR_LAST);
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
            STRRES_ERROR(STR_I2C_AW9523B_OPTION_NEEDS_VALUE, token);
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
        STRRES_ERROR(STR_I2C_AW9523B_UNKNOWN_OPTION, token);
        return -1;
    }

    i2c_master_dev_handle_t dev = NULL;
    if (i2c_device_handle((uint8_t)address, &dev) != ESP_OK) {
        STRRES_ERROR(STR_I2C_AW9523B_ADDRESSING_FAILED, address);
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
            STRRES_ERROR(STR_I2C_AW9523B_NOT_AN_AW9523B, address, report.chip_id);
        } else {
            STRRES_ERROR(STR_I2C_AW9523B_STAGE_FAILED,
                         address, aw9523b_stage_name(report.failed_stage),
                         esp_err_to_name(err));
        }
        handle = NULL;
        return -1;
    }

    handle_address = (uint8_t)address;
    port_inputs[0] = inputs[0];
    port_inputs[1] = inputs[1];

    STRRES_PRINTF(STR_I2C_AW9523B_READY, address, report.chip_id);
    if (push_pull) {
        STRRES_PRINTF(STR_I2C_AW9523B_PORT0_LINE_PUSH_PULL, inputs[0], initial[0]);
    } else {
        STRRES_PRINTF(STR_I2C_AW9523B_PORT0_LINE_OPEN_DRAIN, inputs[0], initial[0]);
    }
    STRRES_PRINTF(STR_I2C_AW9523B_PORT1_LINE, inputs[1], initial[1]);
    STRRES_PRINTF(STR_I2C_AW9523B_LEVELS_NOTE);
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
            STRRES_ERROR(STR_I2C_AW9523B_PORT_READ_FAILED, port, esp_err_to_name(err));
            return -1;
        }
        STRRES_PRINTF(STR_I2C_AW9523B_PORT_VALUE, port, value);
        for (int bit = 7; bit >= 0; bit--) {
            diag_printf("%d", (value >> bit) & 1);
        }
        STRRES_PRINTF(STR_I2C_AW9523B_PORT_BIT_ORDER, port, port_inputs[port]);
    }

    /*
     * The part has no readable output register, so what comes back is the pin
     * levels. On an output pin that is what the pin is actually at, which may
     * differ from what was driven if something else is holding it.
     */
    STRRES_PRINTF(STR_I2C_AW9523B_LEVELS_NOT_REGISTER);
    return 0;
}

int cmd_aw9523b_write(int argc, char **argv)
{
    if (!i2c_require_bus() || !require_init() || !reattach()) {
        return -1;
    }

    if (argc != 3) {
        STRRES_PRINTF(STR_I2C_AW9523B_USAGE_WRITE);
        return -1;
    }

    int port = 0, value = 0;
    if (take_port(argv[1], &port) < 0 || take_byte(argv[2], &value) < 0) {
        return -1;
    }

    esp_err_t err = aw9523b_write_port(handle, (uint8_t)port, (uint8_t)value);
    if (err != ESP_OK) {
        STRRES_ERROR(STR_I2C_AW9523B_PORT_WRITE_FAILED, port, esp_err_to_name(err));
        return -1;
    }

    STRRES_PRINTF(STR_I2C_AW9523B_PORT_WRITTEN, port, value, port_inputs[port]);
    return 0;
}

int cmd_aw9523b_set(int argc, char **argv)
{
    if (!i2c_require_bus() || !require_init() || !reattach()) {
        return -1;
    }

    if (argc != 4) {
        STRRES_PRINTF(STR_I2C_AW9523B_USAGE_SET);
        return -1;
    }

    int port = 0, pin = 0;
    if (take_port(argv[1], &port) < 0) {
        return -1;
    }
    if (cli_parse_int_arg(argv[2], &pin) < 0 || pin < 0 || pin > 7) {
        STRRES_ERROR(STR_I2C_AW9523B_PIN_RANGE);
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
        STRRES_ERROR(STR_I2C_AW9523B_STATE_INVALID);
        return -1;
    }

    /*
     * set_pin() works from the driver's shadow of the last value written, not
     * from a read-modify-write: reading a port gives input levels, so folding
     * them back would write pin state over the outputs.
     */
    esp_err_t err = aw9523b_set_pin(handle, (uint8_t)port, (uint8_t)pin, high);
    if (err != ESP_OK) {
        STRRES_ERROR(STR_I2C_AW9523B_SET_FAILED, port, pin, esp_err_to_name(err));
        return -1;
    }

    if (port_inputs[port] & (1u << pin)) {
        STRRES_PRINTF(STR_I2C_AW9523B_SET_BUT_INPUT,
                      port, pin, high ? "high" : "low", port, pin);
        return 0;
    }

    STRRES_PRINTF(STR_I2C_AW9523B_DRIVEN, port, pin, high ? "high" : "low");
    return 0;
}
