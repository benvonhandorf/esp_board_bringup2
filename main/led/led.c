#include "app_bringup.h"
#include "led.h"
#include "led_drv.h"

#include "driver/gpio.h"
#include "freertos/semphr.h"

#define LED_MAX_COUNT 1024

/* 50 frames a second is smooth, and a 1024-LED chain (31 ms) still fits. */
#define CYCLE_FRAME_MS 20

typedef enum { MODE_STATIC, MODE_CYCLE } led_mode_t;

/*
 * Everything here belongs to the sender task once it exists: it is the only
 * code that calls drv.write(). Commands change `pixels`/`mode` under `lock` and
 * wake it. That is what lets `led cycle` run while the console stays usable.
 */
static struct {
    led_drv_t drv;
    bool configured;
    size_t count;
    uint8_t *pixels;            /* count * 3, R,G,B */
    SemaphoreHandle_t lock;
    SemaphoreHandle_t exited;
    TaskHandle_t task;
    volatile bool quit;
    led_mode_t mode;
    bool dirty;
    uint32_t period_ms;         /* one full trip round the colour wheel */
    TickType_t cycle_start;
} s;

/* Fully saturated, full-value hue, degrees 0-359. */
static void hue_to_rgb(int hue, uint8_t *rgb)
{
    hue %= 360;
    int f = (hue % 60) * 255 / 60;
    switch (hue / 60) {
    case 0:  rgb[0] = 255;     rgb[1] = f;       rgb[2] = 0;       break;
    case 1:  rgb[0] = 255 - f; rgb[1] = 255;     rgb[2] = 0;       break;
    case 2:  rgb[0] = 0;       rgb[1] = 255;     rgb[2] = f;       break;
    case 3:  rgb[0] = 0;       rgb[1] = 255 - f; rgb[2] = 255;     break;
    case 4:  rgb[0] = f;       rgb[1] = 0;       rgb[2] = 255;     break;
    default: rgb[0] = 255;     rgb[1] = 0;       rgb[2] = 255 - f; break;
    }
}

/* The whole chain spans one trip round the wheel, rotated by `phase` degrees. */
static void render_rainbow(int phase)
{
    for (size_t i = 0; i < s.count; i++) {
        hue_to_rgb((int)(i * 360 / s.count) + phase, &s.pixels[i * 3]);
    }
}

static void led_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, s.mode == MODE_CYCLE ? pdMS_TO_TICKS(CYCLE_FRAME_MS)
                                                      : portMAX_DELAY);
        xSemaphoreTake(s.lock, portMAX_DELAY);
        if (s.quit) {
            xSemaphoreGive(s.lock);
            break;
        }

        bool send = s.dirty;
        if (s.mode == MODE_CYCLE) {
            uint32_t elapsed_ms = (xTaskGetTickCount() - s.cycle_start) * portTICK_PERIOD_MS;
            render_rainbow((int)((uint64_t)(elapsed_ms % s.period_ms) * 360 / s.period_ms));
            send = true;
        }
        s.dirty = false;

        if (send) {
            esp_err_t err = s.drv.write(s.drv.ctx, s.pixels, s.count);
            if (err != ESP_OK) {
                ESP_LOGE("led", "write failed: %s", esp_err_to_name(err));
            }
        }
        xSemaphoreGive(s.lock);
    }
    xSemaphoreGive(s.exited);
    vTaskDelete(NULL);
}

/* Stop the task and release the pin; the caller then owns an unconfigured s. */
static void led_teardown(void)
{
    if (!s.configured) {
        return;
    }
    s.quit = true;
    xTaskNotifyGive(s.task);
    xSemaphoreTake(s.exited, portMAX_DELAY);
    s.drv.close(s.drv.ctx);
    free(s.pixels);
    s.pixels = NULL;
    s.configured = false;
}

static bool require_configured(void)
{
    if (!s.configured) {
        STRRES_ERROR(STR_LED_NOT_CONFIGURED);
    }
    return s.configured;
}

int cmd_led_config(int argc, char **argv)
{
    if (argc < 4) {
        STRRES_PRINTF(STR_LED_USAGE_CONFIG);
        return -1;
    }

    int count, pin;
    if (cli_parse_int_arg(argv[2], &count) < 0 || count < 1 || count > LED_MAX_COUNT) {
        STRRES_ERROR(STR_LED_COUNT_RANGE, LED_MAX_COUNT);
        return -1;
    }
    if (cli_parse_int_arg(argv[3], &pin) < 0) {
        STRRES_ERROR(STR_LED_PIN_INVALID, argv[3]);
        return -1;
    }
    if (!GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
        STRRES_ERROR(STR_LED_PIN_NOT_OUTPUT_CAPABLE, pin);
        return -1;
    }

    if (strcasecmp(argv[1], "ws2812") != 0) {
        STRRES_ERROR(STR_LED_TYPE_UNKNOWN, argv[1]);
        return -1;
    }

    if (!s.lock) {
        s.lock = xSemaphoreCreateMutex();
        s.exited = xSemaphoreCreateBinary();
        if (!s.lock || !s.exited) {
            STRRES_ERROR(STR_LED_NO_MEMORY);
            return -1;
        }
    }

    led_teardown();

    s.pixels = calloc((size_t)count, 3);
    if (!s.pixels) {
        STRRES_ERROR(STR_LED_NO_MEMORY);
        return -1;
    }
    s.count = (size_t)count;
    s.quit = false;
    s.mode = MODE_STATIC;
    s.dirty = false;

    esp_err_t err = led_drv_ws2812_open(pin, s.count, &s.drv);
    if (err != ESP_OK) {
        STRRES_ERROR(STR_LED_OPEN_FAILED, pin, esp_err_to_name(err));
        free(s.pixels);
        s.pixels = NULL;
        return -1;
    }

    if (xTaskCreate(led_task, "led", 3072, NULL, 4, &s.task) != pdPASS) {
        s.drv.close(s.drv.ctx);
        free(s.pixels);
        s.pixels = NULL;
        STRRES_ERROR(STR_LED_NO_MEMORY);
        return -1;
    }
    s.configured = true;

    /* Start from a known state: the strip powers up showing whatever noise the
     * data line picked up. */
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.dirty = true;
    xSemaphoreGive(s.lock);
    xTaskNotifyGive(s.task);

    STRRES_PRINTF(STR_LED_CONFIGURED, count, pin);
    return 0;
}

int cmd_led_fill(int argc, char **argv)
{
    if (argc != 2 && argc != 4) {
        STRRES_PRINTF(STR_LED_USAGE_FILL);
        return -1;
    }
    int rgb[3];
    if (argc == 2) {
        /* 0xRRGGBB */
        int packed;
        if (cli_parse_num_arg(argv[1], &packed) < 0 || packed < 0 || packed > 0xFFFFFF) {
            STRRES_ERROR(STR_LED_HEX_RANGE);
            return -1;
        }
        rgb[0] = (packed >> 16) & 0xFF;
        rgb[1] = (packed >> 8) & 0xFF;
        rgb[2] = packed & 0xFF;
    } else {
        for (int i = 0; i < 3; i++) {
            if (cli_parse_int_arg(argv[i + 1], &rgb[i]) < 0 || rgb[i] < 0 || rgb[i] > 255) {
                STRRES_ERROR(STR_LED_COLOUR_RANGE);
                return -1;
            }
        }
    }
    if (!require_configured()) {
        return -1;
    }

    xSemaphoreTake(s.lock, portMAX_DELAY);
    for (size_t i = 0; i < s.count; i++) {
        for (int c = 0; c < 3; c++) {
            s.pixels[i * 3 + c] = (uint8_t)rgb[c];
        }
    }
    s.mode = MODE_STATIC;
    s.dirty = true;
    xSemaphoreGive(s.lock);
    xTaskNotifyGive(s.task);

    STRRES_PRINTF(STR_LED_FILLED, (int)s.count, rgb[0], rgb[1], rgb[2]);
    return 0;
}

int cmd_led_rainbow(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!require_configured()) {
        return -1;
    }

    xSemaphoreTake(s.lock, portMAX_DELAY);
    render_rainbow(0);
    s.mode = MODE_STATIC;
    s.dirty = true;
    xSemaphoreGive(s.lock);
    xTaskNotifyGive(s.task);

    STRRES_PRINTF(STR_LED_RAINBOW, (int)s.count);
    return 0;
}

int cmd_led_cycle(int argc, char **argv)
{
    double seconds = 5.0;
    if (argc >= 2 && (cli_parse_double_arg(argv[1], &seconds) < 0 ||
                      seconds < 0.5 || seconds > 3600.0)) {
        STRRES_ERROR(STR_LED_PERIOD_RANGE);
        return -1;
    }
    if (!require_configured()) {
        return -1;
    }

    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.period_ms = (uint32_t)(seconds * 1000.0);
    s.cycle_start = xTaskGetTickCount();
    s.mode = MODE_CYCLE;
    xSemaphoreGive(s.lock);
    xTaskNotifyGive(s.task);

    STRRES_PRINTF(STR_LED_CYCLING, (int)s.count, seconds);
    return 0;
}

int cmd_led_stop(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!require_configured()) {
        return -1;
    }

    xSemaphoreTake(s.lock, portMAX_DELAY);
    bool was_cycling = s.mode == MODE_CYCLE;
    s.mode = MODE_STATIC;
    xSemaphoreGive(s.lock);
    xTaskNotifyGive(s.task);

    if (was_cycling) {
        STRRES_PRINTF(STR_LED_STOPPED);
    } else {
        STRRES_PRINTF(STR_LED_NOT_CYCLING);
    }
    return 0;
}
