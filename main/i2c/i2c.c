#include "app_bringup.h"
#include "i2c.h"
#include "i2c_parts.h"

#include "driver/gpio.h"

/* Addresses outside 0x08..0x77 are reserved by the I2C specification. */
#define I2C_ADDR_FIRST 0x08
#define I2C_ADDR_LAST  0x77

#define I2C_PROBE_TIMEOUT_MS 50
#define I2C_XFER_TIMEOUT_MS  1000

static i2c_master_bus_handle_t bus_handle = NULL;
static int bus_scl_pin = -1;
static int bus_sda_pin = -1;

/* Device handles are created lazily per address and cached, so repeated reads
 * from the same address do not churn the driver's device list. */
#define MAX_CACHED_DEVICES 8
static struct {
    uint8_t address;
    i2c_master_dev_handle_t handle;
} dev_cache[MAX_CACHED_DEVICES];
static size_t dev_cache_count;

static void forget_devices(void)
{
    for (size_t i = 0; i < dev_cache_count; i++) {
        i2c_master_bus_rm_device(dev_cache[i].handle);
    }
    dev_cache_count = 0;
}

esp_err_t i2c_device_handle(uint8_t address, i2c_master_dev_handle_t *out)
{
    for (size_t i = 0; i < dev_cache_count; i++) {
        if (dev_cache[i].address == address) {
            *out = dev_cache[i].handle;
            return ESP_OK;
        }
    }

    if (dev_cache_count == MAX_CACHED_DEVICES) {
        /* Cache is full; recycle it rather than failing the command. */
        forget_devices();
    }

    const i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 100000,
    };

    i2c_master_dev_handle_t handle = NULL;
    esp_err_t err = i2c_master_bus_add_device(bus_handle, &dev_config, &handle);
    if (err != ESP_OK) {
        return err;
    }

    dev_cache[dev_cache_count].address = address;
    dev_cache[dev_cache_count].handle = handle;
    dev_cache_count++;

    *out = handle;
    return ESP_OK;
}

bool i2c_require_bus(void)
{
    if (!bus_handle) {
        diag_error("I2C not initialized. Run 'i2c bus <scl> <sda>' first.");
        return false;
    }
    return true;
}

/*
 * (Re)create the master bus on the given pins.
 *
 * Tearing down first is what makes `i2c bus` re-runnable: leaving the old bus
 * in place makes every later attempt fail with ESP_ERR_INVALID_STATE forever.
 * The scan also calls this to restore the I2C function after it has borrowed
 * the pins as plain GPIOs.
 */
static esp_err_t create_bus(int scl, int sda)
{
    if (bus_handle) {
        forget_devices();
        i2c_del_master_bus(bus_handle);
        bus_handle = NULL;
    }

    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .scl_io_num = scl,
        .sda_io_num = sda,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&bus_config, &bus_handle);
    if (err != ESP_OK) {
        bus_handle = NULL;
        return err;
    }

    bus_scl_pin = scl;
    bus_sda_pin = sda;
    return ESP_OK;
}

int cmd_i2c_bus(int argc, char **argv)
{
    if (argc < 3) {
        diag_printf("Usage: bus <scl> <sda>\n");
        return -1;
    }

    int scl = 0;
    int sda = 0;
    if (cli_parse_int_arg(argv[1], &scl) < 0 || cli_parse_int_arg(argv[2], &sda) < 0) {
        diag_error("SCL and SDA must be pin numbers");
        return -1;
    }

    if (!GPIO_IS_VALID_OUTPUT_GPIO(scl) || !GPIO_IS_VALID_OUTPUT_GPIO(sda)) {
        diag_error("SCL and SDA must both be output-capable GPIOs");
        return -1;
    }
    if (scl == sda) {
        diag_error("SCL and SDA cannot be the same pin");
        return -1;
    }

    esp_err_t err = create_bus(scl, sda);
    if (err != ESP_OK) {
        diag_error("Creating I2C bus: %s", esp_err_to_name(err));
        return -1;
    }

    diag_printf("I2C bus initialized on SCL=%d, SDA=%d\n", scl, sda);
    return 0;
}

/*
 * Both I2C lines idle high through their pull-ups. A line stuck low means the
 * bus cannot be driven at all, and every probe would simply time out -- so the
 * docs/i2c.md asks for the bus state to be reported instead of an empty table.
 *
 * Checking costs a brief detour through plain GPIO reads; the bus is restored
 * afterwards by the caller re-creating it.
 */
static bool bus_lines_idle_high(void)
{
    const gpio_config_t input_config = {
        .pin_bit_mask = BIT64(bus_scl_pin) | BIT64(bus_sda_pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    if (gpio_config(&input_config) != ESP_OK) {
        return true; /* cannot tell; let the scan proceed */
    }

    /* Let the pull-ups charge the bus capacitance. */
    vTaskDelay(pdMS_TO_TICKS(2));

    int scl = gpio_get_level(bus_scl_pin);
    int sda = gpio_get_level(bus_sda_pin);

    if (scl && sda) {
        return true;
    }

    diag_error("I2C bus is not idle: SCL=%d SDA=%d (both should read 1)", scl, sda);

    if (!scl && !sda) {
        diag_printf("Both lines are held low. Likely causes: no pull-up resistors\n"
                  "fitted, the bus is not powered, or SCL/SDA are shorted to ground.\n");
    } else if (!sda) {
        diag_printf("SDA is held low. A device is probably mid-transfer and stuck;\n"
                  "power-cycle the peripheral, or clock SCL manually to free it.\n");
    } else {
        diag_printf("SCL is held low. A device is holding the clock, or SCL is\n"
                  "shorted to ground.\n");
    }

    diag_printf("Internal pull-ups are weak (tens of kOhm); most buses need\n"
              "external 2.2k-10k resistors to 3V3.\n");
    return false;
}

int cmd_i2c_scan(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!i2c_require_bus()) {
        return -1;
    }

    bool healthy = bus_lines_idle_high();

    /* The check borrowed the pins as plain GPIOs; give them back to I2C. */
    esp_err_t err = create_bus(bus_scl_pin, bus_sda_pin);
    if (err != ESP_OK) {
        diag_error("Restoring the I2C bus after the line check: %s",
                 esp_err_to_name(err));
        return -1;
    }

    if (!healthy) {
        return -1;
    }

    diag_printf("Scanning 0x%02X-0x%02X on SCL=%d, SDA=%d\n",
           I2C_ADDR_FIRST, I2C_ADDR_LAST, bus_scl_pin, bus_sda_pin);

    diag_printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");

    /* Every address in the scanned range could answer at once. */
    uint8_t responded[I2C_ADDR_LAST - I2C_ADDR_FIRST + 1];
    int found = 0;

    for (int row = 0; row <= 0x70; row += 0x10) {
        diag_printf("%02x:", row);
        for (int col = 0; col < 16; col++) {
            uint8_t address = (uint8_t)(row + col);

            if (address < I2C_ADDR_FIRST || address > I2C_ADDR_LAST) {
                diag_printf("   ");
                continue;
            }

            esp_err_t err = i2c_master_probe(bus_handle, address, I2C_PROBE_TIMEOUT_MS);
            if (err == ESP_OK) {
                diag_printf(" %02x", address);
                responded[found++] = address;
            } else {
                /* docs/i2c.md: leave the intersection blank when nothing answers. */
                diag_printf("   ");
            }
        }
        diag_printf("\n");
    }

    diag_printf("%d device%s found\n", found, found == 1 ? "" : "s");

    /*
     * What might be there. Address alone cannot identify a part -- three
     * current monitors share 0x40-0x4f -- so every candidate is listed with
     * the command that would settle it, and the driver's own ID check does the
     * settling. See i2c_parts.c.
     */
    if (found > 0) {
        diag_printf("\n");
        i2c_parts_print_header();
        for (int i = 0; i < found; i++) {
            i2c_parts_print_address(responded[i]);
        }
    }

    return 0;
}

int cmd_i2c_identify(int argc, char **argv)
{
    /* No i2c_require_bus() on purpose: this is a lookup in a static table, not
     * a probe, so it answers while the wiring is still being decided. */
    if (argc < 2) {
        i2c_parts_print_header();
        i2c_parts_print_all();
        return 0;
    }

    int address = 0;
    if (cli_parse_num_arg(argv[1], &address) < 0 || address < 0 || address > 0x7F) {
        diag_error("Address must be 0x00-0x7F");
        return -1;
    }

    i2c_parts_print_header();
    i2c_parts_print_address((uint8_t)address);
    return 0;
}

int cmd_i2c_read(int argc, char **argv)
{
    if (!i2c_require_bus()) {
        return -1;
    }

    if (argc < 2) {
        diag_printf("Usage: read <address> [bytes]\n");
        return -1;
    }

    int address = 0;
    if (cli_parse_num_arg(argv[1], &address) < 0 || address < 0 || address > 0x7F) {
        diag_error("Address must be 0x00-0x7F");
        return -1;
    }

    int bytes = 1;
    if (argc > 2 && (cli_parse_int_arg(argv[2], &bytes) < 0 || bytes < 1 || bytes > 255)) {
        diag_error("Byte count must be 1-255");
        return -1;
    }

    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = i2c_device_handle((uint8_t)address, &dev);
    if (err != ESP_OK) {
        diag_error("Addressing device 0x%02X: %s", address, esp_err_to_name(err));
        return -1;
    }

    uint8_t *data = malloc((size_t)bytes);
    if (!data) {
        diag_error("Out of memory");
        return -1;
    }

    err = i2c_master_receive(dev, data, (size_t)bytes, I2C_XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        diag_error("Reading from 0x%02X: %s", address, esp_err_to_name(err));
        free(data);
        return -1;
    }

    diag_printf("0x%02X:", address);
    for (int i = 0; i < bytes; i++) {
        diag_printf(" %02X", data[i]);
    }
    diag_printf("\n");

    free(data);
    return 0;
}
