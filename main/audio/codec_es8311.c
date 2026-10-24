/*
 * Everest ES8311 mono codec: the console half. The driver is the es8311
 * component; this file is argument parsing, what the user has configured, and
 * every line of text.
 *
 * Two buses meet here, as with the NAU8822: audio is I2S, owned by the `audio`
 * group, and control is I2C, owned by the `i2c` group. The I2C device handle is
 * fetched through i2c_device_handle() before every use rather than kept,
 * because the i2c group recycles its handle cache and a kept handle would go
 * stale without any error to say so.
 *
 * The part is run as an I2S slave, with or without an MCLK pin. The Cardputer
 * Adv has none, and a bit clock of 32 x fs is turned into the 256 x fs the part
 * wants by the pre-multiplier -- see es8311_clock_plan().
 */
#include "app_bringup.h"
#include "codec_es8311.h"
#include "i2c.h"

#include "es8311.h"

#include <stdlib.h>

#define ES8311_ADDR_LOW  ES8311_I2C_ADDR_DEFAULT
#define ES8311_ADDR_HIGH ES8311_I2C_ADDR_ALT

/*
 * Percent maps onto the DAC volume in decibels, not in register codes. The
 * register covers 95.5 dB, so a linear code map would put 5 % at -90 dB, which
 * is silence; a speaker bench needs the bottom of the scale to be quiet rather
 * than absent. 1 % is VOLUME_FLOOR_DB, 100 % is 0 dB, and the gain above 0 dB
 * is not reachable from here.
 */
#define VOLUME_FLOOR_DB 50

/* A value the part will not play loudly: init leaves the volume here rather
 * than wherever the previous owner of the part left it. */
#define DEFAULT_VOLUME_PCT 10

#define PGA_STEP_DB   3
#define PGA_MAX_CODE  10

static es8311_handle_t codec;
static uint8_t address = ES8311_ADDR_LOW;
static bool initialized;
static bool muted;
static int volume_pct = DEFAULT_VOLUME_PCT;
static es8311_mic_t mic = {.digital = false, .pga_code = 6, .adc_scale = 0};

/* ------------------------------------------------------------------ */
/* Access                                                              */
/* ------------------------------------------------------------------ */

/* The component handle, pointed at the current I2C device. */
static es8311_handle_t part(void)
{
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_device_handle(address, &dev) != ESP_OK) {
        dev = NULL;
    }
    if (!codec && es8311_create(dev, &codec) != ESP_OK) {
        return NULL;
    }
    es8311_set_device(codec, dev);
    return codec;
}

static uint8_t volume_code(int pct)
{
    if (pct <= 0) {
        return ES8311_DAC_CODE_MIN;
    }
    /* Integer arithmetic throughout: tenths of a dB. */
    int db_x10 = -VOLUME_FLOOR_DB * 10 + (VOLUME_FLOOR_DB * 10 * (pct - 1)) / 99;
    return es8311_dac_code_from_db_x10(db_x10);
}

static esp_err_t apply_volume(void)
{
    es8311_handle_t h = part();
    if (!h) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = es8311_set_dac_volume(h, volume_code(volume_pct));
    if (err != ESP_OK) {
        return err;
    }
    return es8311_set_mute(h, muted || volume_pct <= 0);
}

/*
 * What the part's clock is made from, as a nominal frequency. The bus reports
 * the clock the divider actually produced, which is a few ppm off the ideal;
 * the planner wants the exact ratio, so round to the integer multiple of the
 * sample rate and rebuild it from the requested rate.
 */
static bool format_from_bus(const audio_format_t *fmt, es8311_format_t *out)
{
    uint32_t actual_rate = audio_bus_actual_rate();
    if (actual_rate == 0) {
        actual_rate = fmt->rate_hz;
    }

    bool has_mclk = fmt->mclk_pin >= 0;
    uint32_t clk = has_mclk ? audio_bus_mclk_hz() : audio_bus_bclk_hz();
    if (clk == 0 || actual_rate == 0) {
        return false;
    }

    uint32_t ratio = (clk + actual_rate / 2) / actual_rate;
    out->rate_hz = fmt->rate_hz;
    out->bits = fmt->bits;
    out->clk_src = has_mclk ? ES8311_CLK_MCLK_PIN : ES8311_CLK_BCLK;
    out->clk_hz = ratio * fmt->rate_hz;
    return true;
}

static void report_start_failure(esp_err_t err, const es8311_report_t *report,
                                 const es8311_format_t *f)
{
    if (err == ESP_ERR_ES8311_UNSUPPORTED_CLOCK) {
        if (f->clk_src == ES8311_CLK_BCLK) {
            STRRES_ERROR(STR_AUDIO_ES8311_CLOCK_UNREACHABLE_BCLK,
                         (unsigned long)f->clk_hz, (unsigned long)f->rate_hz);
        } else {
            STRRES_ERROR(STR_AUDIO_ES8311_CLOCK_UNREACHABLE_MCLK,
                         (unsigned long)f->clk_hz, (unsigned long)f->rate_hz);
        }
        STRRES_PRINTF(STR_AUDIO_ES8311_CLOCK_UNREACHABLE_NOTE);
        return;
    }

    switch (report->failed_stage) {
        case ES8311_STAGE_RESET:
            STRRES_ERROR(STR_AUDIO_ES8311_START_FAILED_RESET, report->reg, esp_err_to_name(err));
            break;
        case ES8311_STAGE_CLOCK:
            STRRES_ERROR(STR_AUDIO_ES8311_START_FAILED_CLOCK, report->reg, esp_err_to_name(err));
            break;
        case ES8311_STAGE_FORMAT:
            STRRES_ERROR(STR_AUDIO_ES8311_START_FAILED_FORMAT, report->reg, esp_err_to_name(err));
            break;
        default:
            STRRES_ERROR(STR_AUDIO_ES8311_START_FAILED_ANALOG, report->reg, esp_err_to_name(err));
            break;
    }
}

/*
 * Bring the part up for the current bus format, then restore what the user had
 * set. es8311_start() resets the part and leaves it muted at the bottom of the
 * scale, so every setting has to be written back -- which is also why this is
 * the one place that does it, for both `init` and a later `audio bus`.
 */
static esp_err_t bring_up(const audio_format_t *fmt)
{
    es8311_handle_t h = part();
    if (!h) {
        return ESP_ERR_INVALID_STATE;
    }

    es8311_format_t f;
    if (!format_from_bus(fmt, &f)) {
        STRRES_ERROR(STR_AUDIO_ES8311_NO_BUS_CLOCK);
        return ESP_ERR_NOT_SUPPORTED;
    }

    es8311_report_t report;
    esp_err_t err = es8311_start(h, &f, &report);
    if (err != ESP_OK) {
        report_start_failure(err, &report, &f);
        return ESP_ERR_NOT_SUPPORTED;   /* explained */
    }

    err = es8311_set_mic(h, &mic);
    if (err != ESP_OK) {
        return err;
    }
    return apply_volume();
}

/* ------------------------------------------------------------------ */
/* Codec interface                                                     */
/* ------------------------------------------------------------------ */

static esp_err_t es8311_codec_configure(const audio_format_t *fmt)
{
    if (!fmt) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Attached parts are reconfigured after `audio bus`; one that was never
     * initialized has nothing to reconfigure. */
    if (!initialized) {
        return ESP_OK;
    }
    return bring_up(fmt);
}

static esp_err_t es8311_codec_set_volume(int percent)
{
    if (!initialized) {
        STRRES_ERROR(STR_AUDIO_ES8311_NOT_INITIALIZED);
        return ESP_ERR_NOT_SUPPORTED;
    }
    volume_pct = percent;
    return apply_volume();
}

static esp_err_t es8311_codec_set_mute(bool mute)
{
    if (!initialized) {
        STRRES_ERROR(STR_AUDIO_ES8311_NOT_INITIALIZED);
        return ESP_ERR_NOT_SUPPORTED;
    }
    muted = mute;
    return apply_volume();
}

static esp_err_t es8311_codec_probe(void)
{
    es8311_handle_t h = part();
    if (!h) {
        return ESP_ERR_INVALID_STATE;
    }
    return es8311_identify(h, NULL, NULL);
}

static void es8311_codec_status(void);

static void es8311_codec_detach(void)
{
    if (initialized) {
        es8311_handle_t h = part();
        if (h) {
            es8311_stop(h, NULL);
        }
    }
    initialized = false;
}

const audio_codec_t es8311_codec = {
    .name = "es8311",
    .description = STR_AUDIO_ES8311_GROUP_HELP,
    .directions = AUDIO_DIR_TX | AUDIO_DIR_RX,
    .needs_mclk = false,
    .probe = es8311_codec_probe,
    .configure = es8311_codec_configure,
    .set_volume = es8311_codec_set_volume,
    .set_mute = es8311_codec_set_mute,
    .status = es8311_codec_status,
    .detach = es8311_codec_detach,
};

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

int cmd_es8311_init(int argc, char **argv)
{
    if (!i2c_require_bus()) {
        return -1;
    }
    if (!audio_bus_require()) {
        return -1;
    }

    uint8_t previous = address;
    if (argc > 1) {
        int value = 0;
        if (cli_parse_num_arg(argv[1], &value) < 0 ||
            (value != ES8311_ADDR_LOW && value != ES8311_ADDR_HIGH)) {
            STRRES_ERROR(STR_AUDIO_ES8311_ADDRESS_RANGE, ES8311_ADDR_LOW, ES8311_ADDR_HIGH);
            return -1;
        }
        address = (uint8_t)value;
    }

    es8311_handle_t h = part();
    if (!h) {
        address = previous;
        STRRES_ERROR(STR_AUDIO_ES8311_NO_DEVICE_HANDLE, address);
        return -1;
    }

    es8311_id_t id;
    es8311_report_t report;
    esp_err_t err = es8311_identify(h, &id, &report);
    if (err == ESP_ERR_ES8311_WRONG_PART) {
        STRRES_ERROR(STR_AUDIO_ES8311_WRONG_PART, address, id.chip_id1, id.chip_id2);
        STRRES_PRINTF(STR_AUDIO_ES8311_WRONG_PART_NOTE);
        address = previous;
        return -1;
    }
    if (err != ESP_OK) {
        STRRES_ERROR(STR_AUDIO_ES8311_NO_RESPONSE, address, esp_err_to_name(err));
        address = previous;
        return -1;
    }
    STRRES_PRINTF(STR_AUDIO_ES8311_PROBE_ID, address, id.chip_id1, id.chip_id2, id.version);

    initialized = true;           /* configure() is a no-op until this is set */
    volume_pct = DEFAULT_VOLUME_PCT;
    muted = false;

    err = bring_up(audio_bus_format());
    if (err != ESP_OK) {
        initialized = false;
        if (err != ESP_ERR_NOT_SUPPORTED) {
            STRRES_ERROR(STR_AUDIO_ES8311_CONFIG_FAILED, esp_err_to_name(err));
        }
        return -1;
    }

    audio_codec_attach(&es8311_codec);

    STRRES_PRINTF(STR_AUDIO_ES8311_INITIALIZED, address, volume_pct);
    return 0;
}

/* The microphone side reports as dB, which is what a datasheet gives. */
static int pga_db(void)
{
    return mic.pga_code * PGA_STEP_DB;
}

static void es8311_codec_status(void)
{
    es8311_handle_t h = part();
    if (!h) {
        return;
    }

    uint8_t volume = 0;
    uint8_t reg31 = 0;
    if (es8311_get_dac_volume(h, &volume) != ESP_OK ||
        es8311_read_reg(h, 0x31, &reg31) != ESP_OK) {
        STRRES_PRINTF(STR_AUDIO_ES8311_STATUS_NO_READBACK);
        return;
    }

    int db_x10 = es8311_dac_code_to_db_x10(volume);
    STRRES_PRINTF(STR_AUDIO_ES8311_STATUS_LINE, address, volume_pct, volume,
                  db_x10 / 10, abs(db_x10) % 10);
    if (reg31 & 0x60) {
        STRRES_PRINTF(STR_AUDIO_ES8311_STATUS_DAC_MUTED);
    } else {
        STRRES_PRINTF(STR_AUDIO_ES8311_STATUS_DAC_UNMUTED);
    }

    uint8_t reg01 = 0;
    uint8_t reg02 = 0;
    if (es8311_read_reg(h, 0x01, &reg01) == ESP_OK &&
        es8311_read_reg(h, 0x02, &reg02) == ESP_OK) {
        int pre_div = ((reg02 >> 5) & 0x7) + 1;
        int pre_mult = 1 << ((reg02 >> 3) & 0x3);
        if (reg01 & 0x80) {
            STRRES_PRINTF(STR_AUDIO_ES8311_STATUS_CLOCK_BCLK, pre_div, pre_mult);
        } else {
            STRRES_PRINTF(STR_AUDIO_ES8311_STATUS_CLOCK_MCLK, pre_div, pre_mult);
        }
    }

    if (mic.digital) {
        STRRES_PRINTF(STR_AUDIO_ES8311_STATUS_MIC_DIGITAL, pga_db());
    } else {
        STRRES_PRINTF(STR_AUDIO_ES8311_STATUS_MIC_ANALOG, pga_db());
    }
}

int cmd_es8311_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!initialized) {
        STRRES_PRINTF(STR_AUDIO_ES8311_NOT_INITIALIZED_STATUS);
        return 0;
    }
    STRRES_PRINTF(STR_AUDIO_ES8311_STATUS_ATTACHED);
    es8311_codec_status();
    return 0;
}

int cmd_es8311_reg(int argc, char **argv)
{
    if (!i2c_require_bus()) {
        return -1;
    }
    if (argc < 2) {
        STRRES_PRINTF(STR_AUDIO_ES8311_USAGE_REG);
        return -1;
    }

    int reg = 0;
    if (cli_parse_num_arg(argv[1], &reg) < 0 || reg < 0 || reg > 0xff) {
        STRRES_ERROR(STR_AUDIO_ES8311_REGISTER_RANGE);
        return -1;
    }

    es8311_handle_t h = part();
    if (!h) {
        STRRES_ERROR(STR_AUDIO_ES8311_NO_DEVICE_HANDLE, address);
        return -1;
    }

    if (argc > 2) {
        int value = 0;
        if (cli_parse_num_arg(argv[2], &value) < 0 || value < 0 || value > 0xff) {
            STRRES_ERROR(STR_AUDIO_ES8311_VALUE_RANGE);
            return -1;
        }
        esp_err_t err = es8311_write_reg(h, (uint8_t)reg, (uint8_t)value);
        if (err != ESP_OK) {
            STRRES_ERROR(STR_AUDIO_ES8311_REGISTER_WRITE_FAILED, reg, esp_err_to_name(err));
            return -1;
        }
        STRRES_PRINTF(STR_AUDIO_ES8311_REGISTER_WRITTEN, reg, value);
        return 0;
    }

    uint8_t value = 0;
    esp_err_t err = es8311_read_reg(h, (uint8_t)reg, &value);
    if (err != ESP_OK) {
        STRRES_ERROR(STR_AUDIO_ES8311_REGISTER_READ_FAILED, reg, esp_err_to_name(err));
        return -1;
    }
    STRRES_PRINTF(STR_AUDIO_ES8311_REGISTER_IS, reg, value);
    return 0;
}

int cmd_es8311_gain(int argc, char **argv)
{
    if (!initialized) {
        STRRES_ERROR(STR_AUDIO_ES8311_NOT_INITIALIZED);
        return -1;
    }

    if (argc < 2) {
        STRRES_PRINTF(STR_AUDIO_ES8311_GAIN_IS, pga_db());
        return 0;
    }

    int db = 0;
    if (cli_parse_int_arg(argv[1], &db) < 0 || db < 0 || db > PGA_MAX_CODE * PGA_STEP_DB) {
        STRRES_ERROR(STR_AUDIO_ES8311_GAIN_RANGE, PGA_MAX_CODE * PGA_STEP_DB, PGA_STEP_DB);
        return -1;
    }

    es8311_mic_t next = mic;
    next.pga_code = (uint8_t)((db + PGA_STEP_DB / 2) / PGA_STEP_DB);
    es8311_handle_t h = part();
    esp_err_t err = h ? es8311_set_mic(h, &next) : ESP_ERR_INVALID_STATE;
    if (err != ESP_OK) {
        STRRES_ERROR(STR_AUDIO_ES8311_GAIN_FAILED, esp_err_to_name(err));
        return -1;
    }
    mic = next;
    STRRES_PRINTF(STR_AUDIO_ES8311_GAIN_SET, pga_db());
    return 0;
}
