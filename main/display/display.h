#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "soc/soc_caps.h"
#include "strres.h"

/*
 * Display bring-up: an SPI transport, a panel registry, and test patterns.
 *
 * The layering mirrors audio.h: display.c owns the SPI host and the drawing
 * commands the way i2s_bus.c owns I2S, and a panel driver (panel_st7789.c) is
 * a small vtable it attaches -- see audio_codec_t for the pattern being
 * followed. Adding a panel is a new file exporting a display_panel_t, a row
 * in the registry in display.c, and a group in app_console.c.
 *
 * There is no framebuffer: a 480x320x3 buffer is 460 kB and none of the chips
 * this targets have PSRAM. Patterns are drawn a band of rows at a time into a
 * small DMA-capable buffer and sent with esp_lcd_panel_draw_bitmap().
 */

/*
 * SPI3 keeps a display and an SD card apart on chips that have it. The C3 has
 * only SPI2, so `spi`, `sd` and `display` share it and each refuses rather
 * than stealing it -- see display_owns_spi_host() and its mirrors,
 * spi_group_owns_host() and sd_owns_spi_host().
 *
 * A preprocessor conditional, not a ternary on SOC_SPI_PERIPH_NUM: the C3
 * build has no SPI3_HOST enumerator at all (spi_types.h only declares it
 * under `#if SOC_SPI_PERIPH_NUM > 2`), so a runtime ternary referencing it
 * fails to compile there regardless of which arm would run.
 */
#if SOC_SPI_PERIPH_NUM > 2
#define BP_DISPLAY_SPI_HOST SPI3_HOST
#else
#define BP_DISPLAY_SPI_HOST SPI2_HOST
#endif

typedef struct {
    int width, height;         /* visible area, in the panel's native orientation */
    int gap_x, gap_y;
    bool bgr, invert;
} display_panel_cfg_t;

typedef struct display_panel {
    const char *name;                /* identifier, not prose */
    strres_id_t description;         /* same id as the part group's help */
    uint8_t bytes_per_pixel;         /* 2 = RGB565 (ST7789), 3 = RGB666 (ILI9488 over SPI) */
    uint16_t frame_w, frame_h;       /* controller RAM: 240x320 ST7789, 320x480 ILI9488 */
    bool default_invert;             /* IPS glass usually wants INVON */

    /*
     * Every call below may return ESP_ERR_NOT_SUPPORTED to mean "I have
     * already told the user why this cannot work", mirroring audio_codec_t.
     */

    esp_err_t (*create)(esp_lcd_panel_io_handle_t io, const display_panel_cfg_t *cfg,
                        esp_lcd_panel_handle_t *out);
    /* One pixel, in the wire order this panel's create() configured the
     * controller to expect. The drawing code works in RGB888; this is what
     * lets the ILI9488's RGB666 and the ST7789's RGB565 share one set of
     * pattern commands. */
    void (*pack)(uint8_t r, uint8_t g, uint8_t b, uint8_t *out);
    void (*status)(void);            /* optional */
} display_panel_t;

/*
 * Make this panel the target of the generic commands.
 *
 * Deletes any panel already attached first, so a part's `init` command is
 * re-runnable. Refuses (ESP_ERR_INVALID_ARG, already reported) when
 * width + gap_x or height + gap_y would fall outside the panel's controller
 * RAM in either orientation, rather than drawing past it.
 */
esp_err_t display_panel_attach(const display_panel_t *p, const display_panel_cfg_t *cfg);

/* True if the bus is up; otherwise reports the usual error and returns false.
 * The mirror of audio_bus_require() / i2c_require_bus(). */
bool display_bus_require(void);

esp_lcd_panel_io_handle_t display_bus_io(void);
/* -1 when the bus was opened with no reset pin. */
int display_bus_rst_pin(void);

/* True while the display group holds BP_DISPLAY_SPI_HOST. Only meaningful
 * when BP_DISPLAY_SPI_HOST == BP_SPI_HOST_ID, i.e. on a chip with one SPI
 * host; on chips with SPI3 this is never asked. */
bool display_owns_spi_host(spi_host_device_t host);

/* GPIO on the bus's `bl` pin, if one was given. ESP_ERR_NOT_SUPPORTED (already
 * reported) when the bus has no backlight pin. */
esp_err_t display_set_backlight(bool on);

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

int cmd_display_bus(int argc, char **argv);
int cmd_display_panels(int argc, char **argv);
int cmd_display_info(int argc, char **argv);
int cmd_display_fill(int argc, char **argv);
int cmd_display_bars(int argc, char **argv);
int cmd_display_grid(int argc, char **argv);
int cmd_display_edges(int argc, char **argv);
int cmd_display_gap(int argc, char **argv);
int cmd_display_orient(int argc, char **argv);
int cmd_display_invert(int argc, char **argv);
int cmd_display_backlight(int argc, char **argv);
int cmd_display_close(int argc, char **argv);

#endif
