/*
 * ST7789 SPI TFT: 240x320 controller RAM, RGB565 over SPI.
 *
 * The V2 silicon on the M5Stack Cardputer is the same controller, so it needs
 * no driver of its own -- what differs per board is glass size, gap, inversion
 * and colour order, all of which are display_panel_cfg_t fields rather than
 * anything this file has to know about.
 */
#include "app_bringup.h"
#include "panel_st7789.h"

#include "esp_lcd_panel_st7789.h"

#define ST7789_FRAME_W 240
#define ST7789_FRAME_H 320

static esp_err_t st7789_create(esp_lcd_panel_io_handle_t io, const display_panel_cfg_t *cfg,
                               esp_lcd_panel_handle_t *out)
{
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = display_bus_rst_pin(),
        .rgb_ele_order = cfg->bgr ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
    };

    esp_lcd_panel_handle_t panel = NULL;
    esp_err_t err = esp_lcd_new_panel_st7789(io, &panel_config, &panel);
    if (err == ESP_OK) {
        err = esp_lcd_panel_reset(panel);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_init(panel);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_invert_color(panel, cfg->invert);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_set_gap(panel, cfg->gap_x, cfg->gap_y);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_disp_on_off(panel, true);
    }
    if (err != ESP_OK) {
        if (panel) {
            esp_lcd_panel_del(panel);
        }
        return err;
    }

    *out = panel;
    return ESP_OK;
}

static void st7789_pack(uint8_t r, uint8_t g, uint8_t b, uint8_t *out)
{
    /* RGB565, MSB first to match LCD_RGB_DATA_ENDIAN_BIG above. */
    const uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    out[0] = (uint8_t)(v >> 8);
    out[1] = (uint8_t)(v & 0xFF);
}

const display_panel_t st7789_panel = {
    .name = "st7789",
    .description = STR_DISPLAY_ST7789_GROUP_HELP,
    .bytes_per_pixel = 2,
    .frame_w = ST7789_FRAME_W,
    .frame_h = ST7789_FRAME_H,
    .default_invert = true, /* IPS glass usually wants INVON for correct colour */
    .create = st7789_create,
    .pack = st7789_pack,
    .status = NULL,
};

int cmd_st7789_init(int argc, char **argv)
{
    if (argc < 3) {
        STRRES_PRINTF(STR_DISPLAY_ST7789_USAGE_INIT);
        return -1;
    }
    if (!display_bus_require()) {
        return -1;
    }

    int width, height;
    if (cli_parse_int_arg(argv[1], &width) < 0 || width <= 0 ||
        cli_parse_int_arg(argv[2], &height) < 0 || height <= 0) {
        STRRES_ERROR(STR_DISPLAY_ST7789_DIMENSIONS_INVALID);
        return -1;
    }

    display_panel_cfg_t cfg = {
        .width = width,
        .height = height,
        .gap_x = 0,
        .gap_y = 0,
        .bgr = false,
        .invert = st7789_panel.default_invert,
    };

    int index = 3;
    while (index < argc) {
        if (strcasecmp(argv[index], "gap") == 0) {
            if (index + 2 >= argc ||
                cli_parse_int_arg(argv[index + 1], &cfg.gap_x) < 0 ||
                cli_parse_int_arg(argv[index + 2], &cfg.gap_y) < 0) {
                STRRES_ERROR(STR_DISPLAY_ST7789_GAP_NEEDS_TWO);
                return -1;
            }
            index += 3;
        } else if (strcasecmp(argv[index], "bgr") == 0) {
            cfg.bgr = true;
            index += 1;
        } else if (strcasecmp(argv[index], "invert") == 0) {
            if (index + 1 >= argc) {
                STRRES_ERROR(STR_DISPLAY_ST7789_INVERT_NEEDS_VALUE);
                return -1;
            }
            if (strcasecmp(argv[index + 1], "on") == 0) {
                cfg.invert = true;
            } else if (strcasecmp(argv[index + 1], "off") == 0) {
                cfg.invert = false;
            } else {
                STRRES_ERROR(STR_DISPLAY_ST7789_INVERT_INVALID, argv[index + 1]);
                return -1;
            }
            index += 2;
        } else {
            STRRES_ERROR(STR_DISPLAY_ST7789_UNEXPECTED_ARGUMENT, argv[index]);
            return -1;
        }
    }

    esp_err_t err = display_panel_attach(&st7789_panel, &cfg);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NOT_SUPPORTED) {
            STRRES_ERROR(STR_DISPLAY_ST7789_ATTACH_FAILED, esp_err_to_name(err));
        }
        return -1;
    }

    STRRES_PRINTF(STR_DISPLAY_ST7789_ATTACHED, width, height);

    /* A successful init is visible immediately, rather than left for a
     * separate `display backlight on` and `display edges`. */
    display_set_backlight(true);
    return cmd_display_edges(0, NULL);
}
