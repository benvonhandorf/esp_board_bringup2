#include "app_bringup.h"
#include "uart.h"

#include <ctype.h>

#include "driver/gpio.h"
#include "driver/uart.h"

#include "sdkconfig.h"

/*
 * The auxiliary UART must never be the console port: the previous version used
 * UART_NUM_0, so `uart init` reassigned the console's own pins and the shell
 * went dead. UART1 exists on every ESP target.
 */
#define AUX_UART_PORT UART_NUM_1

#define RX_BUFFER_SIZE 1024
#define RECEIVE_TIMEOUT_MS 200

static bool uart_ready;
static int uart_tx_pin = -1;
static int uart_rx_pin = -1;
static int uart_baud;

static bool require_uart(void)
{
    if (!uart_ready) {
        STRRES_ERROR(STR_UART_NOT_INITIALIZED);
        return false;
    }
    return true;
}

int cmd_uart_init(int argc, char **argv)
{
    if (argc < 4) {
        STRRES_PRINTF(STR_UART_USAGE_INIT);
        return -1;
    }

    int tx, rx, baud;
    if (cli_parse_int_arg(argv[1], &tx) < 0 || cli_parse_int_arg(argv[2], &rx) < 0) {
        STRRES_ERROR(STR_UART_PINS_NUMERIC);
        return -1;
    }
    if (cli_parse_int_arg(argv[3], &baud) < 0 || baud < 300 || baud > 5000000) {
        STRRES_ERROR(STR_UART_BAUD_RANGE);
        return -1;
    }

    if (!GPIO_IS_VALID_OUTPUT_GPIO(tx)) {
        STRRES_ERROR(STR_UART_TX_NOT_OUTPUT_CAPABLE, tx);
        return -1;
    }
    if (!GPIO_IS_VALID_GPIO(rx)) {
        STRRES_ERROR(STR_UART_PIN_ABSENT, rx);
        return -1;
    }
    if (tx == rx) {
        STRRES_ERROR(STR_UART_PINS_DISTINCT);
        return -1;
    }

#if CONFIG_ESP_CONSOLE_UART
    /* Guard the console's own pins when the console is a UART. */
    if (AUX_UART_PORT == CONFIG_ESP_CONSOLE_UART_NUM) {
        STRRES_ERROR(STR_UART_IS_CONSOLE, AUX_UART_PORT);
        return -1;
    }
#endif

    /* Reinstall from scratch so `init` can be re-run with different pins. */
    if (uart_is_driver_installed(AUX_UART_PORT)) {
        uart_driver_delete(AUX_UART_PORT);
        uart_ready = false;
    }

    const uart_config_t config = {
        .baud_rate = baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(AUX_UART_PORT, RX_BUFFER_SIZE, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        STRRES_ERROR(STR_UART_DRIVER_INSTALL_FAILED, AUX_UART_PORT, esp_err_to_name(err));
        return -1;
    }

    err = uart_param_config(AUX_UART_PORT, &config);
    if (err != ESP_OK) {
        STRRES_ERROR(STR_UART_CONFIG_FAILED, AUX_UART_PORT, esp_err_to_name(err));
        uart_driver_delete(AUX_UART_PORT);
        return -1;
    }

    err = uart_set_pin(AUX_UART_PORT, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        STRRES_ERROR(STR_UART_PIN_ASSIGN_FAILED, AUX_UART_PORT, esp_err_to_name(err));
        uart_driver_delete(AUX_UART_PORT);
        return -1;
    }

    uart_tx_pin = tx;
    uart_rx_pin = rx;
    uart_baud = baud;
    uart_ready = true;

    STRRES_PRINTF(STR_UART_READY, AUX_UART_PORT, tx, rx, baud);
    return 0;
}

/*
 * Expand backslash escapes in place. The console tokenizer already handled
 * quoting, so this only needs to turn \n, \r, \t, \0, \\ and \xNN into bytes --
 * which is what makes it possible to poke a device expecting CR terminators.
 */
static size_t expand_escapes(char *text)
{
    char *out = text;

    for (const char *in = text; *in; in++) {
        if (*in != '\\' || in[1] == '\0') {
            *out++ = *in;
            continue;
        }

        in++;
        switch (*in) {
        case 'n': *out++ = '\n'; break;
        case 'r': *out++ = '\r'; break;
        case 't': *out++ = '\t'; break;
        case '0': *out++ = '\0'; break;
        case '\\': *out++ = '\\'; break;
        case 'x': {
            char hex[3] = {0};
            int digits = 0;
            while (digits < 2 && isxdigit((unsigned char)in[1])) {
                hex[digits++] = *++in;
            }
            if (digits == 0) {
                *out++ = 'x'; /* a lone \x is not an escape */
            } else {
                *out++ = (char)strtol(hex, NULL, 16);
            }
            break;
        }
        default:
            /* Unknown escape: keep both characters so nothing is silently lost. */
            *out++ = '\\';
            *out++ = *in;
            break;
        }
    }

    return (size_t)(out - text);
}

int cmd_uart_send(int argc, char **argv)
{
    if (!require_uart()) {
        return -1;
    }
    if (argc < 2) {
        STRRES_PRINTF(STR_UART_USAGE_SEND);
        return -1;
    }

    /* Join the remaining arguments so `send hello world` works as well as the
     * quoted `send "hello world"`. */
    size_t len = 0;
    for (int i = 1; i < argc; i++) {
        len += strlen(argv[i]) + 1;
    }

    char *payload = malloc(len + 1);
    if (!payload) {
        STRRES_ERROR(STR_UART_OUT_OF_MEMORY);
        return -1;
    }

    payload[0] = '\0';
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            strcat(payload, " ");
        }
        strcat(payload, argv[i]);
    }

    size_t payload_len = expand_escapes(payload);

    int written = uart_write_bytes(AUX_UART_PORT, payload, payload_len);
    if (written < 0) {
        STRRES_ERROR(STR_UART_WRITE_FAILED, AUX_UART_PORT);
        free(payload);
        return -1;
    }

    STRRES_PRINTF(STR_UART_SENT, written, written == 1 ? "" : "s", AUX_UART_PORT, uart_tx_pin);
    free(payload);
    return 0;
}

int cmd_uart_receive(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!require_uart()) {
        return -1;
    }

    uint8_t buffer[RX_BUFFER_SIZE];
    int len = uart_read_bytes(AUX_UART_PORT, buffer, sizeof(buffer),
                              pdMS_TO_TICKS(RECEIVE_TIMEOUT_MS));

    if (len <= 0) {
        STRRES_PRINTF(STR_UART_NO_DATA, AUX_UART_PORT, uart_rx_pin, uart_baud);
        return 0;
    }

    STRRES_PRINTF(STR_UART_RECEIVED, len, len == 1 ? "" : "s", AUX_UART_PORT, uart_rx_pin);

    /* Hex plus ASCII, because bringup traffic is rarely printable. */
    for (int offset = 0; offset < len; offset += 16) {
        diag_printf("  %04x  ", offset);

        for (int i = 0; i < 16; i++) {
            if (offset + i < len) {
                diag_printf("%02x ", buffer[offset + i]);
            } else {
                diag_printf("   ");
            }
        }

        diag_printf(" |");
        for (int i = 0; i < 16 && offset + i < len; i++) {
            uint8_t c = buffer[offset + i];
            diag_printf("%c", (c >= 0x20 && c < 0x7f) ? c : '.');
        }
        diag_printf("|\n");
    }

    return 0;
}
