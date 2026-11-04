#include "led_drv.h"

#include <stdlib.h>

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_rom_sys.h"
#include "soc/soc_caps.h"

/* 10 MHz: one tick is 100 ns, so the 350/900 ns WS2812 timings are 3 and 9. */
#define RMT_RESOLUTION_HZ 10000000

/* Reset/latch time. The original WS2812 wants 50 us, the WS2812B 280 us. */
#define LATCH_US 300

typedef struct {
    rmt_channel_handle_t channel;
    rmt_encoder_handle_t encoder;
    uint8_t *grb;
    size_t count;
} ws2812_t;

static esp_err_t ws2812_write(void *ctx, const uint8_t *rgb, size_t count)
{
    ws2812_t *w = ctx;
    if (count > w->count) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (size_t i = 0; i < count; i++) {
        w->grb[i * 3 + 0] = rgb[i * 3 + 1];
        w->grb[i * 3 + 1] = rgb[i * 3 + 0];
        w->grb[i * 3 + 2] = rgb[i * 3 + 2];
    }

    const rmt_transmit_config_t tx = {.loop_count = 0};
    esp_err_t err = rmt_transmit(w->channel, w->encoder, w->grb, count * 3, &tx);
    if (err == ESP_OK) {
        err = rmt_tx_wait_all_done(w->channel, 1000);
    }
    if (err == ESP_OK) {
        esp_rom_delay_us(LATCH_US);
    }
    return err;
}

static void ws2812_close(void *ctx)
{
    ws2812_t *w = ctx;
    if (w->encoder) {
        rmt_del_encoder(w->encoder);
    }
    if (w->channel) {
        rmt_disable(w->channel);
        rmt_del_channel(w->channel);
    }
    free(w->grb);
    free(w);
}

esp_err_t led_drv_ws2812_open(int data_pin, size_t count, led_drv_t *out)
{
    ws2812_t *w = calloc(1, sizeof(*w));
    if (!w) {
        return ESP_ERR_NO_MEM;
    }
    w->count = count;
    w->grb = malloc(count * 3);
    if (!w->grb) {
        free(w);
        return ESP_ERR_NO_MEM;
    }

    const rmt_tx_channel_config_t channel_config = {
        .gpio_num = data_pin,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL,
        .trans_queue_depth = 1,
    };
    const rmt_bytes_encoder_config_t encoder_config = {
        .bit0 = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9},
        .bit1 = {.level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3},
        .flags.msb_first = 1,
    };

    esp_err_t err = rmt_new_tx_channel(&channel_config, &w->channel);
    if (err == ESP_OK) {
        err = rmt_new_bytes_encoder(&encoder_config, &w->encoder);
    }
    if (err == ESP_OK) {
        err = rmt_enable(w->channel);
    }
    if (err != ESP_OK) {
        /* rmt_disable() on a channel that was never enabled just errors. */
        ws2812_close(w);
        return err;
    }

    out->write = ws2812_write;
    out->close = ws2812_close;
    out->ctx = w;
    return ESP_OK;
}
