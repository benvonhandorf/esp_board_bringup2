/*
 * Display bring-up: SPI transport, panel registry, test patterns.
 *
 * A capability group, not a bus group, mirroring audio.c: this owns the SPI
 * host and the generic drawing commands, and delegates the panel itself to a
 * small vtable (display_panel_t) that a part driver -- panel_st7789.c so far
 * -- attaches. Adding a panel is a new file, a row in panel_registry[], and a
 * group in app_console.c.
 *
 * There is no framebuffer: at 3 bytes/pixel a 480x320 frame is 460 kB and none
 * of the chips this targets have PSRAM. Every pattern is drawn a band of rows
 * at a time into a small DMA-capable buffer and pushed with
 * esp_lcd_panel_draw_bitmap(), which queues the transfer rather than blocking
 * -- on_color_trans_done() gives a semaphore that is taken after every band, or
 * the buffer would be reused while DMA is still sending it.
 */
#include "app_bringup.h"
#include "display.h"
#include "panel_st7789.h"

#include "gpio.h"
#include "sd.h"
#include "spi.h"

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "freertos/semphr.h"

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

#define DISPLAY_DEFAULT_KHZ 20000
/* Widest future panel (ILI9488, RGB666) at the most rows this buffers at
 * once. Set once, when the bus is opened -- before any panel is attached and
 * its real width is known -- so it must cover whatever panel_registry can
 * hold. */
#define DISPLAY_BAND_MAX_ROWS 16
#define DISPLAY_MAX_ROW_BYTES (480 * 3)
#define DISPLAY_MAX_TRANSFER_BYTES (DISPLAY_BAND_MAX_ROWS * DISPLAY_MAX_ROW_BYTES)

static const display_panel_t *const panel_registry[] = {&st7789_panel};

/* ------------------------------------------------------------------ */
/* Transport state                                                     */
/* ------------------------------------------------------------------ */

static const spi_host_device_t host = BP_DISPLAY_SPI_HOST;
static esp_lcd_panel_io_handle_t io_handle;
static bool bus_ready;
static SemaphoreHandle_t color_done_sem;

static int pin_clk = -1, pin_mosi = -1, pin_miso = -1, pin_cs = -1;
static int pin_dc = -1, pin_rst = -1, pin_bl = -1;
static int bus_khz;

static const display_panel_t *attached_panel;
static esp_lcd_panel_handle_t attached_handle;
static display_panel_cfg_t attached_cfg;
static bool orient_swap_xy, orient_mirror_x, orient_mirror_y;

static bool color_trans_done_cb(esp_lcd_panel_io_handle_t panel_io,
                                esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    (void)user_ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(color_done_sem, &woken);
    return woken == pdTRUE;
}

bool display_bus_require(void)
{
    if (!bus_ready) {
        STRRES_ERROR(STR_DISPLAY_NOT_INITIALIZED);
        return false;
    }
    return true;
}

esp_lcd_panel_io_handle_t display_bus_io(void)
{
    return io_handle;
}

int display_bus_rst_pin(void)
{
    return pin_rst;
}

bool display_owns_spi_host(spi_host_device_t h)
{
    return bus_ready && h == host;
}

esp_err_t display_set_backlight(bool on)
{
    if (pin_bl < 0) {
        STRRES_ERROR(STR_DISPLAY_NO_BACKLIGHT_PIN);
        return ESP_ERR_NOT_SUPPORTED;
    }
    return gpio_set_level((gpio_num_t)pin_bl, on ? 1 : 0);
}

static void teardown_panel(void)
{
    if (attached_handle) {
        esp_lcd_panel_del(attached_handle);
        attached_handle = NULL;
    }
    attached_panel = NULL;
}

static void teardown_bus(void)
{
    teardown_panel();
    if (io_handle) {
        esp_lcd_panel_io_del(io_handle);
        io_handle = NULL;
    }
    if (bus_ready) {
        spi_bus_free(host);
        bus_ready = false;
    }
    if (pin_bl >= 0) {
        gpio_reset_pin((gpio_num_t)pin_bl);
    }
    if (pin_rst >= 0) {
        gpio_reset_pin((gpio_num_t)pin_rst);
    }
    pin_clk = pin_mosi = pin_miso = pin_cs = pin_dc = pin_rst = pin_bl = -1;
    bus_khz = 0;
}

/* ------------------------------------------------------------------ */
/* bus                                                                  */
/* ------------------------------------------------------------------ */

static bool check_drivable(const char *role, int pin)
{
    strres_id_t why;
    if (!app_pin_is_drivable(pin, &why)) {
        char reason[APP_STR_LEN];
        STRRES_ERROR(STR_DISPLAY_PIN_NOT_DRIVABLE, role, pin, app_str(why, reason, sizeof(reason)));
        return false;
    }
    return true;
}

int cmd_display_bus(int argc, char **argv)
{
    if (argc < 5) {
        STRRES_PRINTF(STR_DISPLAY_USAGE_BUS);
        return -1;
    }

    int clk, mosi, cs, dc;
    if (cli_parse_int_arg(argv[1], &clk) < 0 || cli_parse_int_arg(argv[2], &mosi) < 0 ||
        cli_parse_int_arg(argv[3], &cs) < 0 || cli_parse_int_arg(argv[4], &dc) < 0) {
        STRRES_ERROR(STR_DISPLAY_PINS_NUMERIC);
        return -1;
    }

    int rst = -1, bl = -1, miso = -1, khz = DISPLAY_DEFAULT_KHZ;
    int index = 5;
    while (index < argc) {
        int *target = NULL;
        const char *name = argv[index];
        if (strcasecmp(name, "rst") == 0) {
            target = &rst;
        } else if (strcasecmp(name, "bl") == 0) {
            target = &bl;
        } else if (strcasecmp(name, "miso") == 0) {
            target = &miso;
        } else if (strcasecmp(name, "khz") == 0) {
            target = &khz;
        } else {
            STRRES_ERROR(STR_DISPLAY_UNEXPECTED_ARGUMENT, name);
            return -1;
        }
        if (index + 1 >= argc || cli_parse_int_arg(argv[index + 1], target) < 0) {
            STRRES_ERROR(STR_DISPLAY_OPTION_NEEDS_VALUE, name);
            return -1;
        }
        index += 2;
    }
    if (khz <= 0) {
        STRRES_ERROR(STR_DISPLAY_OPTION_NEEDS_VALUE, "khz");
        return -1;
    }

    if (!check_drivable("clk", clk) || !check_drivable("mosi", mosi) ||
        !check_drivable("cs", cs) || !check_drivable("dc", dc)) {
        return -1;
    }
    if (rst >= 0 && !check_drivable("rst", rst)) {
        return -1;
    }
    if (bl >= 0 && !check_drivable("bl", bl)) {
        return -1;
    }
    if (miso >= 0 && !GPIO_IS_VALID_GPIO(miso)) {
        STRRES_ERROR(STR_DISPLAY_PIN_ABSENT, miso);
        return -1;
    }

    /* Only meaningful on a chip with one SPI host; on chips with SPI3 `host`
     * never equals BP_SPI_HOST_ID and neither check can fire. */
    if (host == BP_SPI_HOST_ID) {
        if (spi_group_owns_host()) {
            STRRES_ERROR(STR_DISPLAY_SPI_HOLDS_HOST);
            return -1;
        }
        if (sd_owns_spi_host()) {
            STRRES_ERROR(STR_DISPLAY_SD_HOLDS_HOST);
            return -1;
        }
    }

    teardown_bus(); /* re-runnable: close any previous bus first */

    const spi_bus_config_t bus_config = {
        .sclk_io_num = clk,
        .mosi_io_num = mosi,
        .miso_io_num = miso,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_MAX_TRANSFER_BYTES,
    };

    esp_err_t err = spi_bus_initialize(host, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        STRRES_ERROR(STR_DISPLAY_BUS_INIT_FAILED, esp_err_to_name(err));
        return -1;
    }
    bus_ready = true;

    if (!color_done_sem) {
        color_done_sem = xSemaphoreCreateBinary();
    }

    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = cs,
        .dc_gpio_num = dc,
        .spi_mode = 0,
        .pclk_hz = (unsigned int)khz * 1000,
        .trans_queue_depth = 10,
        .on_color_trans_done = color_trans_done_cb,
        .user_ctx = NULL,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };

    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)host, &io_config, &io_handle);
    if (err != ESP_OK) {
        STRRES_ERROR(STR_DISPLAY_IO_INIT_FAILED, esp_err_to_name(err));
        spi_bus_free(host);
        bus_ready = false;
        return -1;
    }

    pin_clk = clk;
    pin_mosi = mosi;
    pin_miso = miso;
    pin_cs = cs;
    pin_dc = dc;
    pin_rst = rst;
    pin_bl = bl;
    bus_khz = khz;

    if (pin_bl >= 0) {
        const gpio_config_t bl_config = {
            .pin_bit_mask = BIT64(pin_bl),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&bl_config);
        gpio_set_level((gpio_num_t)pin_bl, 0);
    }

    STRRES_PRINTF(STR_DISPLAY_BUS_READY, clk, mosi, cs, dc, khz);
    return 0;
}

int cmd_display_close(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!bus_ready) {
        STRRES_PRINTF(STR_DISPLAY_ALREADY_CLOSED);
        return 0;
    }
    teardown_bus();
    STRRES_PRINTF(STR_DISPLAY_CLOSED);
    return 0;
}

/* ------------------------------------------------------------------ */
/* panel attach (called by part drivers, e.g. cmd_st7789_init)          */
/* ------------------------------------------------------------------ */

static bool fits_frame(const display_panel_t *p, int w, int h, int gx, int gy)
{
    if (w + gx <= p->frame_w && h + gy <= p->frame_h) {
        return true;
    }
    /* Or the other way around: swapping x/y later also swaps which of these
     * is the frame's own width and height. */
    return w + gx <= p->frame_h && h + gy <= p->frame_w;
}

esp_err_t display_panel_attach(const display_panel_t *p, const display_panel_cfg_t *cfg)
{
    if (!display_bus_require()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!fits_frame(p, cfg->width, cfg->height, cfg->gap_x, cfg->gap_y)) {
        STRRES_ERROR(STR_DISPLAY_OUT_OF_FRAME, cfg->width, cfg->height, cfg->gap_x, cfg->gap_y,
                     p->frame_w, p->frame_h);
        return ESP_ERR_INVALID_ARG;
    }

    teardown_panel(); /* re-runnable: detach any previous panel first */

    esp_lcd_panel_handle_t panel = NULL;
    esp_err_t err = p->create(io_handle, cfg, &panel);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NOT_SUPPORTED) {
            STRRES_ERROR(STR_DISPLAY_CREATE_FAILED, esp_err_to_name(err));
        }
        return err;
    }

    attached_panel = p;
    attached_handle = panel;
    attached_cfg = *cfg;
    orient_swap_xy = false;
    orient_mirror_x = false;
    orient_mirror_y = false;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* panels, info                                                        */
/* ------------------------------------------------------------------ */

int cmd_display_panels(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    STRRES_PRINTF(STR_DISPLAY_PANELS_HEADER, "name", "part");
    for (size_t i = 0; i < ARRAY_COUNT(panel_registry); i++) {
        const display_panel_t *p = panel_registry[i];
        STRRES_PRINTF(STR_DISPLAY_PANELS_ROW, p->name);
        strres_printf(p->description);
        if (p == attached_panel) {
            STRRES_PRINTF(STR_DISPLAY_PANELS_ATTACHED);
        } else {
            diag_printf("\n");
        }
    }
    return 0;
}

int cmd_display_info(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!bus_ready) {
        STRRES_PRINTF(STR_DISPLAY_NOT_INITIALIZED);
        return 0;
    }

    /* A compile-time choice, not a comparison against SPI3_HOST: that
     * enumerator does not exist at all on a chip with one SPI host. */
#if SOC_SPI_PERIPH_NUM > 2
    const char *host_name = "SPI3";
#else
    const char *host_name = "SPI2";
#endif
    STRRES_PRINTF(STR_DISPLAY_INFO_HOST, host_name, pin_clk, pin_mosi, pin_cs, pin_dc, bus_khz);
    if (pin_miso >= 0) {
        STRRES_PRINTF(STR_DISPLAY_INFO_MISO, pin_miso);
    }
    if (pin_rst >= 0) {
        STRRES_PRINTF(STR_DISPLAY_INFO_RST, pin_rst);
    }
    if (pin_bl >= 0) {
        STRRES_PRINTF(STR_DISPLAY_INFO_BL, pin_bl);
    }

    if (!attached_panel) {
        STRRES_PRINTF(STR_DISPLAY_INFO_NO_PANEL);
        return 0;
    }

    STRRES_PRINTF(STR_DISPLAY_INFO_PANEL, attached_panel->name, attached_cfg.width,
                  attached_cfg.height, attached_cfg.gap_x, attached_cfg.gap_y,
                  (int)attached_panel->bytes_per_pixel * 8);
    STRRES_PRINTF(STR_DISPLAY_INFO_ORIENT, orient_swap_xy ? 1 : 0, orient_mirror_x ? 1 : 0,
                  orient_mirror_y ? 1 : 0);
    STRRES_PRINTF(STR_DISPLAY_INFO_COLOR, attached_cfg.bgr ? "bgr" : "rgb",
                  attached_cfg.invert ? "on" : "off");

    /*
     * Reproducible as a board preset. These are command syntax, not prose --
     * the same reasoning that leaves the preset lines in board.c as literals
     * rather than strres ids -- so they are composed directly.
     */
    diag_printf("\n> display bus %d %d %d %d", pin_clk, pin_mosi, pin_cs, pin_dc);
    if (pin_rst >= 0) {
        diag_printf(" rst %d", pin_rst);
    }
    if (pin_bl >= 0) {
        diag_printf(" bl %d", pin_bl);
    }
    if (pin_miso >= 0) {
        diag_printf(" miso %d", pin_miso);
    }
    diag_printf(" khz %d\n", bus_khz);

    diag_printf("> display-%s init %d %d gap %d %d%s invert %s\n", attached_panel->name,
                attached_cfg.width, attached_cfg.height, attached_cfg.gap_x, attached_cfg.gap_y,
                attached_cfg.bgr ? " bgr" : "", attached_cfg.invert ? "on" : "off");

    if (orient_swap_xy || orient_mirror_x || orient_mirror_y) {
        diag_printf("> display orient %d %d %d\n", orient_swap_xy ? 1 : 0,
                    orient_mirror_x ? 1 : 0, orient_mirror_y ? 1 : 0);
    }

    if (attached_panel->status) {
        attached_panel->status();
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Drawing: no framebuffer, a DMA band of rows at a time                */
/* ------------------------------------------------------------------ */

typedef void (*row_fill_fn)(int y, int width, uint8_t *rgb888_row, void *ctx);

static esp_err_t draw_pattern(row_fill_fn fill, void *ctx)
{
    if (!attached_panel) {
        STRRES_ERROR(STR_DISPLAY_NO_PANEL);
        STRRES_PRINTF(STR_DISPLAY_NO_PANEL_NOTE);
        return ESP_ERR_INVALID_STATE;
    }

    const int width = attached_cfg.width;
    const int height = attached_cfg.height;
    const int bpp = attached_panel->bytes_per_pixel;

    int rows_per_band = DISPLAY_MAX_TRANSFER_BYTES / (width * bpp);
    if (rows_per_band > DISPLAY_BAND_MAX_ROWS) {
        rows_per_band = DISPLAY_BAND_MAX_ROWS;
    }
    if (rows_per_band < 1) {
        rows_per_band = 1;
    }

    uint8_t *rgb_row = malloc((size_t)width * 3);
    uint8_t *band = heap_caps_malloc((size_t)rows_per_band * width * bpp, MALLOC_CAP_DMA);
    if (!rgb_row || !band) {
        STRRES_ERROR(STR_DISPLAY_OUT_OF_MEMORY);
        free(rgb_row);
        free(band);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    for (int y0 = 0; y0 < height && err == ESP_OK; y0 += rows_per_band) {
        int rows = rows_per_band;
        if (y0 + rows > height) {
            rows = height - y0;
        }

        for (int r = 0; r < rows; r++) {
            fill(y0 + r, width, rgb_row, ctx);
            uint8_t *out_row = band + (size_t)r * width * bpp;
            for (int x = 0; x < width; x++) {
                attached_panel->pack(rgb_row[x * 3], rgb_row[x * 3 + 1], rgb_row[x * 3 + 2],
                                     out_row + (size_t)x * bpp);
            }
        }

        err = esp_lcd_panel_draw_bitmap(attached_handle, 0, y0, width, y0 + rows, band);
        if (err == ESP_OK) {
            xSemaphoreTake(color_done_sem, portMAX_DELAY);
        }
    }

    free(rgb_row);
    free(band);
    if (err != ESP_OK) {
        STRRES_ERROR(STR_DISPLAY_DRAW_FAILED, esp_err_to_name(err));
    }
    return err;
}

static void fill_solid(int y, int width, uint8_t *row, void *ctx)
{
    (void)y;
    const uint8_t *rgb = ctx;
    for (int x = 0; x < width; x++) {
        row[x * 3 + 0] = rgb[0];
        row[x * 3 + 1] = rgb[1];
        row[x * 3 + 2] = rgb[2];
    }
}

static bool parse_color(const char *token, uint8_t *out)
{
    static const struct {
        const char *name;
        uint8_t r, g, b;
    } named[] = {
        {"red", 255, 0, 0},   {"green", 0, 255, 0}, {"blue", 0, 0, 255},
        {"white", 255, 255, 255}, {"black", 0, 0, 0},
    };
    for (size_t i = 0; i < ARRAY_COUNT(named); i++) {
        if (strcasecmp(token, named[i].name) == 0) {
            out[0] = named[i].r;
            out[1] = named[i].g;
            out[2] = named[i].b;
            return true;
        }
    }
    int value;
    if (cli_parse_num_arg(token, &value) < 0 || value < 0 || value > 0xFFFFFF) {
        return false;
    }
    out[0] = (uint8_t)(value >> 16);
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)value;
    return true;
}

int cmd_display_fill(int argc, char **argv)
{
    if (argc < 2) {
        STRRES_PRINTF(STR_DISPLAY_USAGE_FILL);
        return -1;
    }

    uint8_t rgb[3];
    if (!parse_color(argv[1], rgb)) {
        STRRES_ERROR(STR_DISPLAY_COLOR_INVALID, argv[1]);
        return -1;
    }

    if (draw_pattern(fill_solid, rgb) != ESP_OK) {
        return -1;
    }
    STRRES_PRINTF(STR_DISPLAY_FILL_DONE, argv[1]);
    return 0;
}

static const uint8_t bar_colors[8][3] = {
    {255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
    {255, 0, 255},   {255, 0, 0},   {0, 0, 255},   {0, 0, 0},
};

static void fill_bars(int y, int width, uint8_t *row, void *ctx)
{
    (void)y;
    (void)ctx;
    for (int x = 0; x < width; x++) {
        int bar = (x * 8) / width;
        if (bar > 7) {
            bar = 7;
        }
        row[x * 3 + 0] = bar_colors[bar][0];
        row[x * 3 + 1] = bar_colors[bar][1];
        row[x * 3 + 2] = bar_colors[bar][2];
    }
}

int cmd_display_bars(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (draw_pattern(fill_bars, NULL) != ESP_OK) {
        return -1;
    }
    STRRES_PRINTF(STR_DISPLAY_BARS_DONE);
    return 0;
}

typedef struct {
    int step;
    int height;
} grid_ctx_t;

static void fill_grid(int y, int width, uint8_t *row, void *ctx)
{
    const grid_ctx_t *g = ctx;
    const bool row_line = (y % g->step == 0) || y == 0 || y == g->height - 1;
    for (int x = 0; x < width; x++) {
        const bool col_line = (x % g->step == 0) || x == 0 || x == width - 1;
        const uint8_t v = (row_line || col_line) ? 255 : 0;
        row[x * 3 + 0] = row[x * 3 + 1] = row[x * 3 + 2] = v;
    }
}

int cmd_display_grid(int argc, char **argv)
{
    int step = 20;
    if (argc > 1 && (cli_parse_int_arg(argv[1], &step) < 0 || step < 2)) {
        STRRES_ERROR(STR_DISPLAY_GRID_STEP_INVALID, argv[1]);
        return -1;
    }

    if (!attached_panel) {
        /* draw_pattern() reports this, but the step check above must run
         * first so a bad argument is never masked by "no panel". */
        return draw_pattern(fill_grid, NULL) == ESP_OK ? 0 : -1;
    }

    grid_ctx_t ctx = {.step = step, .height = attached_cfg.height};
    if (draw_pattern(fill_grid, &ctx) != ESP_OK) {
        return -1;
    }
    STRRES_PRINTF(STR_DISPLAY_GRID_DONE, step);
    return 0;
}

typedef struct {
    int width;
    int height;
} edges_ctx_t;

static void fill_edges(int y, int width, uint8_t *row, void *ctx)
{
    const edges_ctx_t *e = ctx;
    for (int x = 0; x < width; x++) {
        uint8_t r = 0, g = 0, b = 0;
        if (y == 0) {
            r = 255; /* top: red */
        } else if (y == e->height - 1) {
            b = 255; /* bottom: blue */
        } else if (x == 0) {
            r = g = b = 255; /* left: white */
        } else if (x == width - 1) {
            g = 255; /* right: green */
        }
        if (x < 8 && y < 8) {
            /* filled square at logical (0,0) */
            r = 255;
            g = 255;
            b = 0;
        }
        if (y >= 10 && y < 14 && x >= 8 && x < 24) {
            /* a short tick along +X, so the square's own corner is unambiguous */
            r = 0;
            g = 255;
            b = 255;
        }
        row[x * 3 + 0] = r;
        row[x * 3 + 1] = g;
        row[x * 3 + 2] = b;
    }
}

int cmd_display_edges(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!attached_panel) {
        return draw_pattern(fill_edges, NULL) == ESP_OK ? 0 : -1;
    }

    edges_ctx_t ctx = {.width = attached_cfg.width, .height = attached_cfg.height};
    if (draw_pattern(fill_edges, &ctx) != ESP_OK) {
        return -1;
    }
    STRRES_PRINTF(STR_DISPLAY_EDGES_DONE);
    return 0;
}

/* ------------------------------------------------------------------ */
/* gap, orient, invert, backlight                                       */
/* ------------------------------------------------------------------ */

int cmd_display_gap(int argc, char **argv)
{
    if (!attached_panel) {
        STRRES_ERROR(STR_DISPLAY_NO_PANEL);
        STRRES_PRINTF(STR_DISPLAY_NO_PANEL_NOTE);
        return -1;
    }
    if (argc < 3) {
        STRRES_PRINTF(STR_DISPLAY_USAGE_GAP);
        return -1;
    }

    int x, y;
    if (cli_parse_int_arg(argv[1], &x) < 0 || cli_parse_int_arg(argv[2], &y) < 0) {
        STRRES_ERROR(STR_DISPLAY_GAP_INVALID);
        return -1;
    }

    esp_err_t err = esp_lcd_panel_set_gap(attached_handle, x, y);
    if (err != ESP_OK) {
        STRRES_ERROR(STR_DISPLAY_GAP_FAILED, esp_err_to_name(err));
        return -1;
    }
    attached_cfg.gap_x = x;
    attached_cfg.gap_y = y;

    STRRES_PRINTF(STR_DISPLAY_GAP_SET, x, y);
    return cmd_display_edges(0, NULL);
}

int cmd_display_orient(int argc, char **argv)
{
    if (!attached_panel) {
        STRRES_ERROR(STR_DISPLAY_NO_PANEL);
        STRRES_PRINTF(STR_DISPLAY_NO_PANEL_NOTE);
        return -1;
    }
    if (argc < 4) {
        STRRES_PRINTF(STR_DISPLAY_USAGE_ORIENT);
        return -1;
    }

    int sx, mx, my;
    if (cli_parse_int_arg(argv[1], &sx) < 0 || (sx != 0 && sx != 1) ||
        cli_parse_int_arg(argv[2], &mx) < 0 || (mx != 0 && mx != 1) ||
        cli_parse_int_arg(argv[3], &my) < 0 || (my != 0 && my != 1)) {
        STRRES_ERROR(STR_DISPLAY_ORIENT_INVALID);
        return -1;
    }

    esp_err_t err = esp_lcd_panel_swap_xy(attached_handle, sx != 0);
    if (err == ESP_OK) {
        err = esp_lcd_panel_mirror(attached_handle, mx != 0, my != 0);
    }
    if (err != ESP_OK) {
        STRRES_ERROR(STR_DISPLAY_ORIENT_FAILED, esp_err_to_name(err));
        return -1;
    }

    if ((sx != 0) != orient_swap_xy) {
        const int tmp = attached_cfg.width;
        attached_cfg.width = attached_cfg.height;
        attached_cfg.height = tmp;
    }
    orient_swap_xy = sx != 0;
    orient_mirror_x = mx != 0;
    orient_mirror_y = my != 0;

    STRRES_PRINTF(STR_DISPLAY_ORIENT_SET, sx, mx, my);
    return cmd_display_edges(0, NULL);
}

int cmd_display_invert(int argc, char **argv)
{
    if (!attached_panel) {
        STRRES_ERROR(STR_DISPLAY_NO_PANEL);
        STRRES_PRINTF(STR_DISPLAY_NO_PANEL_NOTE);
        return -1;
    }
    if (argc < 2) {
        STRRES_PRINTF(STR_DISPLAY_USAGE_INVERT);
        return -1;
    }

    bool on;
    if (strcasecmp(argv[1], "on") == 0) {
        on = true;
    } else if (strcasecmp(argv[1], "off") == 0) {
        on = false;
    } else {
        STRRES_ERROR(STR_DISPLAY_INVERT_INVALID, argv[1]);
        return -1;
    }

    esp_err_t err = esp_lcd_panel_invert_color(attached_handle, on);
    if (err != ESP_OK) {
        STRRES_ERROR(STR_DISPLAY_INVERT_FAILED, esp_err_to_name(err));
        return -1;
    }
    attached_cfg.invert = on;

    STRRES_PRINTF(STR_DISPLAY_INVERT_SET, on ? "on" : "off");
    return cmd_display_bars(0, NULL);
}

int cmd_display_backlight(int argc, char **argv)
{
    if (!display_bus_require()) {
        return -1;
    }
    if (argc < 2) {
        STRRES_PRINTF(STR_DISPLAY_USAGE_BACKLIGHT);
        return -1;
    }

    bool on;
    if (strcasecmp(argv[1], "on") == 0) {
        on = true;
    } else if (strcasecmp(argv[1], "off") == 0) {
        on = false;
    } else {
        STRRES_ERROR(STR_DISPLAY_BACKLIGHT_INVALID, argv[1]);
        return -1;
    }

    if (display_set_backlight(on) != ESP_OK) {
        return -1; /* already reported */
    }
    STRRES_PRINTF(STR_DISPLAY_BACKLIGHT_SET, on ? "on" : "off");
    return 0;
}
