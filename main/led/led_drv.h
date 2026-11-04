#ifndef LED_DRV_H
#define LED_DRV_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Transport for a chain of addressable LEDs. A driver turns a buffer of RGB
 * triples into whatever its LED family wants on the wire, so led.c knows
 * nothing about timing, colour order or how many pins there are.
 *
 * To add a family (APA102: data + clock, 4-byte frames, start and end frames),
 * write an `led_drv_<family>_open()` that fills in this struct and add a row to
 * the type switch in cmd_led_config(). Nothing else changes.
 */
typedef struct {
    /* Send `count` pixels, 3 bytes each in R,G,B order. Blocks until the data
     * has left the chip and the strip has latched. */
    esp_err_t (*write)(void *ctx, const uint8_t *rgb, size_t count);
    void (*close)(void *ctx);
    void *ctx;
} led_drv_t;

/* WS2812 and compatibles (SK6812 RGB, WS2811 at 800 kHz): one data pin, RMT. */
esp_err_t led_drv_ws2812_open(int data_pin, size_t count, led_drv_t *out);

#endif
