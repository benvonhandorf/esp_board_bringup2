#include "app_bringup.h"
#include "sd.h"
#include "spi.h"

#include "driver/gpio.h"
#include "esp_heap_caps.h"

#define SPI_HOST_ID BP_SPI_HOST_ID
#define SPI_CLOCK_HZ 1000000
#define MAX_TRANSFER_BYTES 255

static spi_device_handle_t device;
static bool bus_ready;

bool spi_group_owns_host(void)
{
    return bus_ready;
}

static bool require_bus(void)
{
    if (!device) {
        diag_error("SPI not initialized. Run 'spi bus <clk> <mosi> <miso> [cs]' first.");
        return false;
    }
    return true;
}

static void teardown(void)
{
    if (device) {
        spi_bus_remove_device(device);
        device = NULL;
    }
    if (bus_ready) {
        spi_bus_free(SPI_HOST_ID);
        bus_ready = false;
    }
}

int cmd_spi_bus(int argc, char **argv)
{
    if (argc < 4) {
        diag_printf("Usage: bus <clk> <mosi> <miso> [cs]\n");
        return -1;
    }

    int clk, mosi, miso;
    if (cli_parse_int_arg(argv[1], &clk) < 0 || cli_parse_int_arg(argv[2], &mosi) < 0 ||
        cli_parse_int_arg(argv[3], &miso) < 0) {
        diag_error("CLK, MOSI and MISO must be pin numbers");
        return -1;
    }

    /*
     * CS is an extension to the docs/spi.md syntax. Without it spics_io_num is -1
     * and no real peripheral can be selected, so every read returns bus noise.
     */
    int cs = -1;
    if (argc > 4 && cli_parse_int_arg(argv[4], &cs) < 0) {
        diag_error("CS must be a pin number");
        return -1;
    }

    if (!GPIO_IS_VALID_OUTPUT_GPIO(clk) || !GPIO_IS_VALID_OUTPUT_GPIO(mosi)) {
        diag_error("CLK and MOSI must be output-capable GPIOs");
        return -1;
    }
    if (!GPIO_IS_VALID_GPIO(miso)) {
        diag_error("GPIO %d does not exist on this chip", miso);
        return -1;
    }
    if (cs >= 0 && !GPIO_IS_VALID_OUTPUT_GPIO(cs)) {
        diag_error("CS must be an output-capable GPIO");
        return -1;
    }

    /*
     * There is only one SPI host in play, and the SD module may be holding it.
     * Taking it away would leave an initialized card talking to a bus that no
     * longer exists, so say so instead.
     */
    if (sd_owns_spi_host()) {
        diag_error("The SD card holds the SPI host. Release it with 'sd close' first.");
        return -1;
    }

    /* Free any previous bus so this command can be re-run with new pins. */
    teardown();

    const spi_bus_config_t bus_config = {
        .sclk_io_num = clk,
        .mosi_io_num = mosi,
        .miso_io_num = miso,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = MAX_TRANSFER_BYTES + 1,
    };

    esp_err_t err = spi_bus_initialize(SPI_HOST_ID, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        diag_error("Initializing SPI bus: %s", esp_err_to_name(err));
        return -1;
    }
    bus_ready = true;

    const spi_device_interface_config_t dev_config = {
        .mode = 0,
        .clock_speed_hz = SPI_CLOCK_HZ,
        .spics_io_num = cs,
        .queue_size = 1,
    };

    err = spi_bus_add_device(SPI_HOST_ID, &dev_config, &device);
    if (err != ESP_OK) {
        diag_error("Adding SPI device: %s", esp_err_to_name(err));
        device = NULL;
        teardown();
        return -1;
    }

    diag_printf("SPI ready: CLK=%d, MOSI=%d, MISO=%d, CS=", clk, mosi, miso);
    if (cs >= 0) {
        diag_printf("%d", cs);
    } else {
        diag_printf("none (drive chip select yourself with 'gpio set')");
    }
    diag_printf(", mode 0 at %d Hz\n", SPI_CLOCK_HZ);
    return 0;
}

/*
 * Hand the host back without a reset. Needed because the SD module competes for
 * the same host, and on single-host chips there is otherwise no way to move from
 * `spi bus` to `sd spi` in one session.
 */
int cmd_spi_free(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!bus_ready) {
        diag_printf("SPI bus is not initialized.\n");
        return 0;
    }
    teardown();
    diag_printf("SPI bus released.\n");
    return 0;
}

int cmd_spi_read(int argc, char **argv)
{
    if (!require_bus()) {
        return -1;
    }
    if (argc < 3) {
        diag_printf("Usage: read <addr> <len>\n");
        return -1;
    }

    int address, length;
    if (cli_parse_num_arg(argv[1], &address) < 0 || address < 0 || address > 0xFF) {
        diag_error("Address must be 0x00-0xFF");
        return -1;
    }
    if (cli_parse_int_arg(argv[2], &length) < 0 || length < 1 || length > MAX_TRANSFER_BYTES) {
        diag_error("Length must be 1-%d", MAX_TRANSFER_BYTES);
        return -1;
    }

    /*
     * One full-duplex transaction: send the address byte, then clock out
     * `length` dummy bytes while the device shifts data back. The previous
     * version put the address in .cmd while command_bits was 0, so the address
     * was silently never transmitted at all.
     *
     * Buffers must be DMA-capable now that the bus uses DMA.
     */
    size_t total = (size_t)length + 1;
    uint8_t *tx = heap_caps_calloc(1, total, MALLOC_CAP_DMA);
    uint8_t *rx = heap_caps_calloc(1, total, MALLOC_CAP_DMA);
    if (!tx || !rx) {
        diag_error("Out of DMA-capable memory");
        free(tx);
        free(rx);
        return -1;
    }

    tx[0] = (uint8_t)address;

    spi_transaction_t transaction = {
        .length = total * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };

    esp_err_t err = spi_device_transmit(device, &transaction);
    if (err != ESP_OK) {
        diag_error("SPI read: %s", esp_err_to_name(err));
        free(tx);
        free(rx);
        return -1;
    }

    diag_printf("0x%02X:", address);
    for (int i = 0; i < length; i++) {
        diag_printf(" %02X", rx[i + 1]); /* rx[0] is clocked in during the address */
    }
    diag_printf("\n");

    free(tx);
    free(rx);
    return 0;
}

int cmd_spi_write(int argc, char **argv)
{
    if (!require_bus()) {
        return -1;
    }
    if (argc < 3) {
        diag_printf("Usage: write <addr> <data> [data...]\n");
        return -1;
    }

    int address;
    if (cli_parse_num_arg(argv[1], &address) < 0 || address < 0 || address > 0xFF) {
        diag_error("Address must be 0x00-0xFF");
        return -1;
    }

    int data_count = argc - 2;
    if (data_count > MAX_TRANSFER_BYTES) {
        diag_error("At most %d data bytes per write", MAX_TRANSFER_BYTES);
        return -1;
    }

    size_t total = (size_t)data_count + 1;
    uint8_t *tx = heap_caps_calloc(1, total, MALLOC_CAP_DMA);
    if (!tx) {
        diag_error("Out of DMA-capable memory");
        return -1;
    }

    tx[0] = (uint8_t)address;
    for (int i = 0; i < data_count; i++) {
        int value;
        if (cli_parse_num_arg(argv[2 + i], &value) < 0 || value < 0 || value > 0xFF) {
            diag_error("Data byte '%s' is not 0x00-0xFF", argv[2 + i]);
            free(tx);
            return -1;
        }
        tx[i + 1] = (uint8_t)value;
    }

    spi_transaction_t transaction = {
        .length = total * 8,
        .tx_buffer = tx,
    };

    esp_err_t err = spi_device_transmit(device, &transaction);
    if (err != ESP_OK) {
        diag_error("SPI write: %s", esp_err_to_name(err));
        free(tx);
        return -1;
    }

    diag_printf("Wrote %d byte%s to 0x%02X\n",
              data_count, data_count == 1 ? "" : "s", address);

    free(tx);
    return 0;
}
