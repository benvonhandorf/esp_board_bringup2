/*
 * I2S transport for the audio group.
 *
 * This file owns the peripheral the way i2c.c owns the I2C bus: it is the only
 * place that talks to the I2S driver, and everything above it -- the signal
 * generator, the codec drivers -- works through the small API in audio.h. That
 * is also what keeps the chip guard to one file. A part with no I2S peripheral
 * still builds; the commands just report that it has none.
 *
 * There are two ways to receive, and they are genuinely different rather than
 * two spellings of the same thing. A standard-mode receiver shares BCLK and WS
 * with the transmitter and is the same peripheral running the other way, so it
 * is opened alongside the transmitter by `audio bus ... din <pin>`. A PDM
 * microphone brings a clock of its own in the megahertz and hands the
 * peripheral a one-bit stream to decimate, so it gets its own channel and its
 * own command. Everything above this file sees one receiver either way.
 */
#include "app_bringup.h"
#include "audio.h"
#include "esp_cpu.h"

#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if SOC_I2S_SUPPORTED

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#if SOC_I2S_SUPPORTS_PDM_RX
#include "driver/i2s_pdm.h"
#endif

/* Six descriptors of 240 frames is the driver's own default and buys about
 * 30 ms of buffering at 48 kHz -- enough that a console printf cannot starve
 * the output. 240 is divisible by 3, which the driver requires at 24 bits. */
#define DMA_DESC_NUM  6
#define DMA_FRAME_NUM 240

static i2s_chan_handle_t tx_chan;
static i2s_chan_handle_t rx_chan;
static bool bus_open;
static bool tx_running;
static bool rx_running;

static audio_rx_mode_t rx_mode;
static uint8_t rx_bits = 16;

static audio_format_t format;
static int pin_bclk = -1;
static int pin_ws = -1;
static int pin_dout = -1;
static int pin_din = -1;
static int pin_pdm_clk = -1;
static int pdm_raw_dins[AUDIO_PDM_MAX_LINES];
static int pdm_raw_line_count;

/* Software-decimated ultrasonic PDM: set while audio_bus_open_pdm_ultra()'s
 * receiver is open. The channel underneath is a raw one. */
static pdm_dec_t *ultra_dec;
static pdm_dec2_t *ultra_dec2;   /* set instead of ultra_dec for the fast filter */
static uint32_t ultra_max_hz;
static uint32_t ultra_rate;
static uint64_t ultra_cycles;
static uint64_t ultra_frames;
static uint16_t *ultra_scratch;
static size_t ultra_scratch_bytes;

/* 16-bit data sits in a 16-bit slot; 24- and 32-bit data both travel in a
 * 32-bit slot. Packing 24-bit data into three-byte slots is legal I2S and a
 * nuisance to generate, and most codecs are configured for 32-bit slots
 * anyway. */
static uint32_t slot_bits(void)
{
    return format.bits == 16 ? 16 : 32;
}

size_t audio_bus_frame_bytes(void)
{
    return 2 * (slot_bits() / 8);
}

int audio_bus_rx_channels(void)
{
    switch (rx_mode) {
    case AUDIO_RX_STD:
    case AUDIO_RX_PDM:
        return 2;
    case AUDIO_RX_PDM_RAW:
        return 2 * pdm_raw_line_count;
    default:
        return 0;
    }
}

size_t audio_bus_rx_frame_bytes(void)
{
    /* PDM (either mode) decimates to, or raw-samples at, 16-bit whatever the
     * transmit side is doing. Fall back to the old stereo assumption when
     * nothing is open yet, so callers that size a buffer before opening a
     * receiver keep getting the same answer as before this had a real
     * per-mode channel count. */
    int channels = audio_bus_rx_channels();
    if (channels <= 0) {
        channels = 2;
    }
    return (size_t)channels * ((rx_bits == 16 ? 16 : 32) / 8);
}

uint8_t audio_bus_rx_bits(void)
{
    return rx_bits;
}

audio_rx_mode_t audio_bus_rx_mode(void)
{
    return rx_mode;
}

bool audio_bus_rx_ready(void)
{
    return rx_chan != NULL;
}

bool audio_bus_rx_internal(void)
{
    return rx_mode == AUDIO_RX_STD && pin_din >= 0 && pin_din == pin_dout;
}

void audio_bus_rx_pins(int *clk, int *din)
{
    if (clk) *clk = (rx_mode == AUDIO_RX_PDM || rx_mode == AUDIO_RX_PDM_RAW) ? pin_pdm_clk : pin_bclk;
    if (din) *din = pin_din;
}

void audio_bus_rx_pdm_raw_pins(int *clk, int dins[AUDIO_PDM_MAX_LINES], int *line_count)
{
    if (clk) {
        *clk = (rx_mode == AUDIO_RX_PDM_RAW) ? pin_pdm_clk : -1;
    }
    if (dins) {
        for (int i = 0; i < AUDIO_PDM_MAX_LINES; i++) {
            dins[i] = (rx_mode == AUDIO_RX_PDM_RAW) ? pdm_raw_dins[i] : -1;
        }
    }
    if (line_count) {
        *line_count = (rx_mode == AUDIO_RX_PDM_RAW) ? pdm_raw_line_count : 0;
    }
}

bool audio_bus_rx_require(void)
{
    if (!rx_chan) {
        STRRES_ERROR(STR_AUDIO_NO_RECEIVER);
        STRRES_PRINTF(STR_AUDIO_NO_RECEIVER_NOTE);
        return false;
    }
    return true;
}

bool audio_bus_ready(void)
{
    return bus_open;
}

bool audio_bus_require(void)
{
    if (!bus_open) {
        STRRES_ERROR(STR_AUDIO_NOT_INITIALIZED);
        return false;
    }
    return true;
}

const audio_format_t *audio_bus_format(void)
{
    return bus_open ? &format : NULL;
}

void audio_bus_pins(int *bclk, int *ws, int *dout, int *din, int *mclk)
{
    if (bclk) *bclk = pin_bclk;
    if (ws)   *ws = pin_ws;
    if (dout) *dout = pin_dout;
    if (mclk) *mclk = format.mclk_pin;
    /* Only a standard-mode receiver is on this bus. A PDM microphone's data
     * pin belongs to a channel with its own clock and is reported by
     * audio_bus_rx_pins(); listing it here would put it on a bus it is not on. */
    if (din)  *din = (rx_mode == AUDIO_RX_STD) ? pin_din : -1;
}

/*
 * What the driver actually programmed, rather than what was asked for.
 *
 * The clock tree divides an integer ratio out of a PLL, so a requested rate is
 * rarely produced exactly. i2s_channel_get_info() reports the real BCLK, and
 * the frame is two slots, so the sample rate follows. Reporting achieved
 * alongside requested is the same habit as `gpio pwm set` and `sd spi`.
 */
static bool chan_info(i2s_chan_info_t *info)
{
    if (!bus_open || !tx_chan) {
        return false;
    }
    return i2s_channel_get_info(tx_chan, info) == ESP_OK;
}

static bool rx_chan_info(i2s_chan_info_t *info)
{
    if (!rx_chan) {
        return false;
    }
    return i2s_channel_get_info(rx_chan, info) == ESP_OK;
}

/*
 * The rate the receiver is really running at.
 *
 * A standard-mode receiver divides its own BCLK exactly as the transmitter
 * does. A PDM receiver's BCLK is the megahertz clock on the wire, not a frame
 * clock, and the decimator downstream of it produces the requested rate, so
 * asking the same question of both would give the wrong answer for one.
 */
uint32_t audio_bus_rx_rate(void)
{
    if (ultra_dec || ultra_dec2) {
        return ultra_rate;
    }
    if (rx_mode == AUDIO_RX_PDM || rx_mode == AUDIO_RX_PDM_RAW) {
        return format.rate_hz;
    }

    i2s_chan_info_t info;
    if (!rx_chan_info(&info) || info.bclk_hz == 0) {
        return 0;
    }
    return info.bclk_hz / (slot_bits() * 2);
}

/* The clock the microphone itself sees, which is the number its datasheet sets
 * limits on. Zero unless a PDM microphone is open. */
uint32_t audio_bus_pdm_clk_hz(void)
{
    if (ultra_dec || ultra_dec2) {
        return AUDIO_ULTRA_CLK_HZ;
    }
    i2s_chan_info_t info;
    if ((rx_mode != AUDIO_RX_PDM && rx_mode != AUDIO_RX_PDM_RAW) || !rx_chan_info(&info)) {
        return 0;
    }
    return info.bclk_hz;
}

uint32_t audio_bus_actual_rate(void)
{
    i2s_chan_info_t info;
    if (!chan_info(&info) || info.bclk_hz == 0) {
        return 0;
    }
    return info.bclk_hz / (slot_bits() * 2);
}

uint32_t audio_bus_bclk_hz(void)
{
    i2s_chan_info_t info;
    return chan_info(&info) ? info.bclk_hz : 0;
}

uint32_t audio_bus_mclk_hz(void)
{
    i2s_chan_info_t info;
    return chan_info(&info) ? info.mclk_hz : 0;
}

void audio_bus_rx_close(void)
{
    if (rx_chan) {
        if (rx_running) {
            i2s_channel_disable(rx_chan);
        }
        i2s_del_channel(rx_chan);
        rx_chan = NULL;
    }

    pdm_dec_free(ultra_dec);
    ultra_dec = NULL;
    pdm_dec2_free(ultra_dec2);
    ultra_dec2 = NULL;
    free(ultra_scratch);
    ultra_scratch = NULL;
    ultra_scratch_bytes = 0;
    ultra_max_hz = 0;
    ultra_rate = 0;

    rx_running = false;
    rx_mode = AUDIO_RX_NONE;
    rx_bits = 16;
    pin_din = -1;
    pin_pdm_clk = -1;
    pdm_raw_line_count = 0;
    for (int i = 0; i < AUDIO_PDM_MAX_LINES; i++) {
        pdm_raw_dins[i] = -1;
    }
}

void audio_bus_close(void)
{
    audio_bus_rx_close();

    if (tx_chan) {
        if (tx_running) {
            i2s_channel_disable(tx_chan);
        }
        i2s_del_channel(tx_chan);
        tx_chan = NULL;
    }

    tx_running = false;
    bus_open = false;
    pin_bclk = pin_ws = pin_dout = -1;
    format.mclk_pin = -1;
}

esp_err_t audio_bus_open(int bclk, int ws, int dout, int din,
                         const audio_format_t *fmt)
{
    /* Tearing down first is what makes `audio bus` re-runnable, for the same
     * reason create_bus() in i2c.c deletes before it creates. */
    audio_bus_close();

    format = *fmt;
    pin_bclk = bclk;
    pin_ws = ws;
    pin_dout = dout;
    pin_din = din;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = DMA_DESC_NUM;
    chan_cfg.dma_frame_num = DMA_FRAME_NUM;
    /* Send silence, not a stale buffer, if the generator ever falls behind. A
     * repeated buffer sounds like a perfectly good tone and would hide the
     * starvation that this tool exists to expose. */
    chan_cfg.auto_clear_after_cb = true;

    esp_err_t err = i2s_new_channel(&chan_cfg, &tx_chan, din >= 0 ? &rx_chan : NULL);
    if (err != ESP_OK) {
        audio_bus_close();
        return err;
    }

    i2s_mclk_multiple_t multiple;
    switch (format.mclk_multiple) {
    case 384: multiple = I2S_MCLK_MULTIPLE_384; break;
    case 512: multiple = I2S_MCLK_MULTIPLE_512; break;
    case 256: multiple = I2S_MCLK_MULTIPLE_256; break;
    default:
        /* 24-bit data needs a multiple of 3 or the rate comes out inaccurate,
         * which the driver documents on the mclk_multiple field. */
        multiple = (format.bits == 24) ? I2S_MCLK_MULTIPLE_384 : I2S_MCLK_MULTIPLE_256;
        format.mclk_multiple = (format.bits == 24) ? 384 : 256;
        break;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = format.rate_hz,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = multiple,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            (i2s_data_bit_width_t)format.bits, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = format.mclk_pin >= 0 ? (gpio_num_t)format.mclk_pin : I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)bclk,
            .ws = (gpio_num_t)ws,
            .dout = dout >= 0 ? (gpio_num_t)dout : I2S_GPIO_UNUSED,
            .din = din >= 0 ? (gpio_num_t)din : I2S_GPIO_UNUSED,
            .invert_flags = {0},
        },
    };
    std_cfg.slot_cfg.slot_bit_width = (i2s_slot_bit_width_t)slot_bits();

    err = i2s_channel_init_std_mode(tx_chan, &std_cfg);
    if (err != ESP_OK) {
        audio_bus_close();
        return err;
    }

    if (rx_chan) {
        err = i2s_channel_init_std_mode(rx_chan, &std_cfg);
        if (err != ESP_OK) {
            audio_bus_close();
            return err;
        }
        rx_mode = AUDIO_RX_STD;
        rx_bits = format.bits;
    }

    bus_open = true;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* PDM receive                                                         */
/* ------------------------------------------------------------------ */

#if SOC_I2S_SUPPORTS_PDM_RX && SOC_I2S_SUPPORTS_PDM2PCM

esp_err_t audio_bus_open_pdm(int clk, int din, const audio_format_t *fmt)
{
    audio_bus_rx_close();

    /*
     * The rate lives in the module's shared format so that a capture and a
     * playback are always described by one number. If the standard bus is
     * already open its rate wins -- reading at one rate while playing at
     * another would make every loopback measurement a lie.
     */
    if (!bus_open) {
        format = *fmt;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = DMA_DESC_NUM;
    chan_cfg.dma_frame_num = DMA_FRAME_NUM;

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &rx_chan);
    if (err != ESP_OK) {
        rx_chan = NULL;
        return err;
    }

    /*
     * Stereo, so both slots are captured and the caller can see which one the
     * microphone lands in. A single PDM part drives one half of the frame --
     * which half is set by a select pin on the board -- and the other half
     * stays at whatever the second microphone would have put there, which on a
     * mono board is silence. Reporting both is how that gets discovered.
     */
    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(format.rate_hz),
        .slot_cfg = I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                           I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .clk = (gpio_num_t)clk,
            .din = (gpio_num_t)din,
            .invert_flags = {0},
        },
    };

    err = i2s_channel_init_pdm_rx_mode(rx_chan, &pdm_cfg);
    if (err != ESP_OK) {
        audio_bus_rx_close();
        return err;
    }

    rx_mode = AUDIO_RX_PDM;
    rx_bits = 16;   /* the decimator's output width; PDM has no other option */
    pin_pdm_clk = clk;
    pin_din = din;
    return ESP_OK;
}

#else /* no PDM receive, or no hardware decimator */

esp_err_t audio_bus_open_pdm(int clk, int din, const audio_format_t *fmt)
{
    (void)clk; (void)din; (void)fmt;

#if !SOC_I2S_SUPPORTS_PDM_RX
    STRRES_ERROR(STR_AUDIO_NO_PDM, CONFIG_IDF_TARGET);
#else
    /*
     * The peripheral can clock a PDM microphone but has no PDM-to-PCM filter,
     * so what arrives is the raw one-bit stream. Every statistic this tool
     * computes -- RMS, peak, the spectrum -- would be measuring the modulator
     * rather than the sound, and would look like plausible numbers while
     * meaning nothing. Refusing is more useful than that.
     */
    STRRES_ERROR(STR_AUDIO_PDM_NO_FILTER, CONFIG_IDF_TARGET);
    STRRES_PRINTF(STR_AUDIO_PDM_NO_FILTER_NOTE);
#endif
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* SOC_I2S_SUPPORTS_PDM_RX && SOC_I2S_SUPPORTS_PDM2PCM */

/* ------------------------------------------------------------------ */
/* Raw multi-line PDM receive                                          */
/* ------------------------------------------------------------------ */

#if SOC_I2S_SUPPORTS_PDM_RX

/* Standard PDM oversampling ratio: 64 bit-clock cycles per decimated sample.
 * Every PDM part this project has met (see codec_sph0645.c) uses it; a part
 * that does not would need this taught a per-part ratio, the same way the
 * SPH0645 driver teaches its own bit-clock range on top of this. */
#define PDM_RAW_OVERSAMPLE 32

esp_err_t audio_bus_open_pdm_raw(int clk, const int *dins, int line_count,
                                 const audio_format_t *fmt)
{
    int max_lines = AUDIO_PDM_MAX_LINES;
#if SOC_I2S_PDM_MAX_RX_LINES < AUDIO_PDM_MAX_LINES
    max_lines = SOC_I2S_PDM_MAX_RX_LINES;
#endif
    if (line_count < 1 || line_count > max_lines) {
        return ESP_ERR_INVALID_ARG;
    }

    audio_bus_rx_close();

    /* Same reasoning as audio_bus_open_pdm(): one shared rate, so a capture
     * and a playback never disagree about what they are measuring. */
    if (!bus_open) {
        format = *fmt;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = DMA_DESC_NUM;
    chan_cfg.dma_frame_num = DMA_FRAME_NUM;

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &rx_chan);
    if (err != ESP_OK) {
        rx_chan = NULL;
        return err;
    }

    /*
     * One line/slot pair per active data pin, per the I2S_PDM_RX_LINEn_SLOT_*
     * bit layout in hal/i2s_types.h: line n owns bits (2n, 2n+1). Left and
     * right are always both wanted, for the same reason audio_bus_open_pdm()
     * always asks for stereo -- which physical half of each line's frame a
     * mono part lands in is exactly what this is for finding out.
     */
    uint32_t slot_mask = 0;
    for (int i = 0; i < line_count; i++) {
        slot_mask |= (uint32_t)(I2S_PDM_SLOT_LEFT | I2S_PDM_SLOT_RIGHT) << (2 * i);
    }

    i2s_pdm_rx_gpio_config_t gpio_cfg = {
        .clk = (gpio_num_t)clk,
        .invert_flags = {0},
    };
#if SOC_I2S_PDM_MAX_RX_LINES > 1
    /* gpio_cfg.dins[] is only SOC_I2S_PDM_MAX_RX_LINES entries wide -- looping
     * to AUDIO_PDM_MAX_LINES on a chip where that constant is smaller would
     * write past the array and into invert_flags, which shares the union. */
    for (int i = 0; i < SOC_I2S_PDM_MAX_RX_LINES; i++) {
        gpio_cfg.dins[i] = -1;
    }
    for (int i = 0; i < line_count; i++) {
        gpio_cfg.dins[i] = (gpio_num_t)dins[i];
    }
#else
    gpio_cfg.din = (gpio_num_t)dins[0];
#endif

    /*
     * RAW, not PCM: the one hardware PDM-to-PCM filter on this chip only ever
     * produces two channels, so more than one line has to skip it and read
     * the undecimated bitstream instead. The oversample multiplier gets the
     * bit clock into the megahertz range a real part expects, the same
     * distinction codec_sph0645.c has to make for the decimated path.
     */
    i2s_pdm_rx_slot_config_t slot_cfg = I2S_PDM_RX_SLOT_RAW_FMT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);

    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(format.rate_hz * PDM_RAW_OVERSAMPLE),
        .slot_cfg = slot_cfg,
        .gpio_cfg = gpio_cfg,
    };

    pdm_cfg.clk_cfg.dn_sample_mode = I2S_PDM_DSR_8S;
    pdm_cfg.clk_cfg.sample_rate_hz = format.rate_hz * 32;

    /*
     * NOT VERIFIED AGAINST REAL HARDWARE, and not fully verified against the
     * driver either: ESP-IDF 6.0.2's i2s_pdm_rx_set_slot() (i2s_pdm.c in the
     * pinned esp-idf) hardcodes total_slot/active_slot to 1-2 regardless of
     * how many bits are set in slot_mask above, which is what sizes the DMA
     * buffer i2s_channel_init_pdm_rx_mode() allocates below. The GPIO wiring
     * and slot_mask bits say the hardware clocks line_count * 2 slots per
     * frame; if the buffer stays sized for 2 regardless, reads will alias or
     * overrun rather than fail loudly. Confirming that -- and, if needed,
     * reaching into esp_private/i2s_private.h and hal/i2s_ll.h to resize the
     * buffer and set rx_tdm_tot_chan_num by hand after this call -- is the
     * experiment this function is a placeholder for.
     */
    err = i2s_channel_init_pdm_rx_mode(rx_chan, &pdm_cfg);
    if (err != ESP_OK) {
        audio_bus_rx_close();
        return err;
    }

    //Set some pull-ups and downs for testing
    for (int i = 0; i < line_count; i++) {
        gpio_pull_mode_t mode = ( i % 2 ) == 0 ? GPIO_PULLDOWN_ONLY : GPIO_PULLUP_ONLY;
        gpio_sleep_set_pull_mode(dins[i], mode);
    }


    rx_mode = AUDIO_RX_PDM_RAW;
    rx_bits = 16;
    pin_pdm_clk = clk;
    pin_din = dins[0];
    pdm_raw_line_count = line_count;
    for (int i = 0; i < line_count; i++) {
        pdm_raw_dins[i] = dins[i];
    }
    for (int i = line_count; i < AUDIO_PDM_MAX_LINES; i++) {
        pdm_raw_dins[i] = -1;
    }
    return ESP_OK;
}


uint32_t audio_bus_rx_band_limit_hz(void)
{
    return (ultra_dec || ultra_dec2) ? ultra_max_hz : 0;
}

bool audio_bus_pdm_ultra_load(uint64_t *cycles, uint64_t *frames)
{
    if (!ultra_dec && !ultra_dec2) {
        return false;
    }
    if (cycles) *cycles = ultra_cycles;
    if (frames) *frames = ultra_frames;
    return true;
}

esp_err_t audio_bus_open_pdm_ultra(int clk, int din, uint32_t max_hz, bool fast)
{
    if (max_hz == 0 || max_hz > AUDIO_ULTRA_MAX_HZ) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Largest R (a multiple of 16) whose Nyquist is 1.5 x max_hz or better. */
    int r = 16;
    for (int cand = 64; cand >= 16; cand -= 16) {
        if ((double)AUDIO_ULTRA_CLK_HZ / cand / 2.0 >= 1.5 * max_hz) {
            r = cand;
            break;
        }
    }

    /* Standard mode first: 2.048 MHz is inside the 1-2.475 MHz entry range. */
    audio_format_t fmt = {.rate_hz = 64000, .bits = 16, .mclk_pin = -1, .mclk_multiple = 0};
    esp_err_t err = audio_bus_open_pdm_raw(clk, &din, 1, &fmt);
    if (err != ESP_OK) {
        return err;
    }
    err = i2s_channel_enable(rx_chan);
    if (err != ESP_OK) {
        audio_bus_rx_close();
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(100));   /* let the part wake in standard mode */

    /* Raise the clock in place: the channel and its pins are never released. */
    i2s_pdm_rx_clk_config_t clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(AUDIO_ULTRA_CLK_HZ);
    clk_cfg.dn_sample_mode = I2S_PDM_DSR_8S;
    clk_cfg.sample_rate_hz = AUDIO_ULTRA_CLK_HZ;
    err = i2s_channel_disable(rx_chan);
    if (err == ESP_OK) {
        err = i2s_channel_reconfig_pdm_rx_clock(rx_chan, &clk_cfg);
    }
    if (err == ESP_OK) {
        err = i2s_channel_enable(rx_chan);
    }
    if (err != ESP_OK) {
        audio_bus_rx_close();
        return err;
    }
    rx_running = true;

    if (fast) {
        r = 16;   /* the two-stage filter is a fixed 4.8 MHz -> 300 kHz */
        ultra_dec2 = pdm_dec2_new(2, true);
    } else {
        ultra_dec = pdm_dec_new(2, r, false);
    }
    if (!ultra_dec && !ultra_dec2) {
        audio_bus_rx_close();
        return ESP_ERR_NO_MEM;
    }
    ultra_max_hz = max_hz;
    ultra_cycles = 0;
    ultra_frames = 0;
    ultra_rate = AUDIO_ULTRA_CLK_HZ / (uint32_t)r;
    format.rate_hz = ultra_rate;

    /* To the rest of the firmware this is an ordinary decimated PDM input. */
    rx_mode = AUDIO_RX_PDM;
    return ESP_OK;
}

#else /* no PDM receive at all */

esp_err_t audio_bus_open_pdm_raw(int clk, const int *dins, int line_count,
                                 const audio_format_t *fmt)
{
    (void)clk; (void)dins; (void)line_count; (void)fmt;
    STRRES_ERROR(STR_AUDIO_NO_PDM, CONFIG_IDF_TARGET);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_bus_open_pdm_ultra(int clk, int din, uint32_t max_hz, bool fast)
{
    (void)fast;
    (void)clk; (void)din; (void)max_hz;
    STRRES_ERROR(STR_AUDIO_NO_PDM, CONFIG_IDF_TARGET);
    return ESP_ERR_NOT_SUPPORTED;
}

uint32_t audio_bus_rx_band_limit_hz(void) { return 0; }
bool audio_bus_pdm_ultra_load(uint64_t *c, uint64_t *f) { (void)c; (void)f; return false; }

#endif /* SOC_I2S_SUPPORTS_PDM_RX */

uint64_t audio_bus_buffered_frames(void)
{
    return (uint64_t)DMA_DESC_NUM * DMA_FRAME_NUM;
}

size_t audio_bus_block_frames(void)
{
    return DMA_FRAME_NUM;
}

esp_err_t audio_bus_tx_enable(bool enable)
{
    if (!tx_chan) {
        return ESP_ERR_INVALID_STATE;
    }
    if (enable == tx_running) {
        return ESP_OK;
    }

    esp_err_t err = enable ? i2s_channel_enable(tx_chan) : i2s_channel_disable(tx_chan);
    if (err == ESP_OK) {
        tx_running = enable;
    }
    return err;
}

bool audio_bus_tx_enabled(void)
{
    return tx_running;
}

esp_err_t audio_bus_write(const void *data, size_t bytes, size_t *written,
                          uint32_t timeout_ms)
{
    if (!tx_chan || !tx_running) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2s_channel_write(tx_chan, data, bytes, written, timeout_ms);
}

esp_err_t audio_bus_rx_enable(bool enable)
{
    if (!rx_chan) {
        return ESP_ERR_INVALID_STATE;
    }
    if (enable == rx_running) {
        return ESP_OK;
    }

    esp_err_t err = enable ? i2s_channel_enable(rx_chan) : i2s_channel_disable(rx_chan);
    if (err == ESP_OK) {
        rx_running = enable;
    }
    return err;
}

bool audio_bus_rx_enabled(void)
{
    return rx_running;
}

esp_err_t audio_bus_read(void *data, size_t bytes, size_t *read,
                         uint32_t timeout_ms)
{
    if (!rx_chan || !rx_running) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ultra_dec || ultra_dec2) {
        /* Two words of raw bits per output frame per R/16, decimated here. */
        size_t out_frames = bytes / 4;
        size_t raw_frames = out_frames * (ultra_dec2 ? 1 : (size_t)pdm_dec_r_words(ultra_dec));
        size_t raw_bytes = raw_frames * 4;
        if (raw_bytes > ultra_scratch_bytes) {
            free(ultra_scratch);
            ultra_scratch = malloc(raw_bytes);
            ultra_scratch_bytes = ultra_scratch ? raw_bytes : 0;
            if (!ultra_scratch) {
                return ESP_ERR_NO_MEM;
            }
        }
        size_t got = 0;
        esp_err_t err = i2s_channel_read(rx_chan, ultra_scratch, raw_bytes, &got, timeout_ms);
        uint32_t t0 = esp_cpu_get_cycle_count();
        size_t produced = ultra_dec2
                              ? pdm_dec2_process(ultra_dec2, ultra_scratch, got / 4, data)
                              : pdm_dec_process(ultra_dec, ultra_scratch, got / 4, data);
        ultra_cycles += (uint32_t)(esp_cpu_get_cycle_count() - t0);
        ultra_frames += produced;
        if (read) {
            *read = produced * 4;
        }
        return err;
    }
    return i2s_channel_read(rx_chan, data, bytes, read, timeout_ms);
}

#else /* !SOC_I2S_SUPPORTED */

/*
 * No I2S peripheral on this chip. The commands stay in the group and report
 * why they cannot work, which is more useful during bring-up than a missing
 * command -- the same choice sd.c makes for `sd mmc` on the ESP32-C3.
 */

size_t audio_bus_frame_bytes(void) { return 4; }
bool audio_bus_ready(void) { return false; }

bool audio_bus_require(void)
{
    STRRES_ERROR(STR_AUDIO_NO_I2S, CONFIG_IDF_TARGET);
    return false;
}

const audio_format_t *audio_bus_format(void) { return NULL; }

void audio_bus_pins(int *bclk, int *ws, int *dout, int *din, int *mclk)
{
    if (bclk) *bclk = -1;
    if (ws)   *ws = -1;
    if (dout) *dout = -1;
    if (din)  *din = -1;
    if (mclk) *mclk = -1;
}

uint32_t audio_bus_actual_rate(void) { return 0; }
uint32_t audio_bus_bclk_hz(void) { return 0; }
uint32_t audio_bus_mclk_hz(void) { return 0; }
uint64_t audio_bus_buffered_frames(void) { return 0; }
size_t audio_bus_block_frames(void) { return 240; }
void audio_bus_close(void) {}

esp_err_t audio_bus_open(int bclk, int ws, int dout, int din,
                         const audio_format_t *fmt)
{
    (void)bclk; (void)ws; (void)dout; (void)din; (void)fmt;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_bus_tx_enable(bool enable) { (void)enable; return ESP_ERR_NOT_SUPPORTED; }
bool audio_bus_tx_enabled(void) { return false; }

esp_err_t audio_bus_write(const void *data, size_t bytes, size_t *written,
                          uint32_t timeout_ms)
{
    (void)data; (void)bytes; (void)timeout_ms;
    if (written) {
        *written = 0;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

size_t audio_bus_rx_frame_bytes(void) { return 4; }
uint8_t audio_bus_rx_bits(void) { return 16; }
audio_rx_mode_t audio_bus_rx_mode(void) { return AUDIO_RX_NONE; }
bool audio_bus_rx_ready(void) { return false; }
bool audio_bus_rx_internal(void) { return false; }
uint32_t audio_bus_rx_rate(void) { return 0; }
uint32_t audio_bus_pdm_clk_hz(void) { return 0; }
uint32_t audio_bus_rx_band_limit_hz(void) { return 0; }
bool audio_bus_pdm_ultra_load(uint64_t *c, uint64_t *f) { (void)c; (void)f; return false; }
void audio_bus_rx_close(void) {}

bool audio_bus_rx_require(void)
{
    return audio_bus_require();   /* same message: the chip has no I2S at all */
}

void audio_bus_rx_pins(int *clk, int *din)
{
    if (clk) *clk = -1;
    if (din) *din = -1;
}

int audio_bus_rx_channels(void) { return 0; }

void audio_bus_rx_pdm_raw_pins(int *clk, int dins[AUDIO_PDM_MAX_LINES], int *line_count)
{
    if (clk) *clk = -1;
    if (dins) {
        for (int i = 0; i < AUDIO_PDM_MAX_LINES; i++) {
            dins[i] = -1;
        }
    }
    if (line_count) *line_count = 0;
}

esp_err_t audio_bus_open_pdm(int clk, int din, const audio_format_t *fmt)
{
    (void)clk; (void)din; (void)fmt;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_bus_open_pdm_raw(int clk, const int *dins, int line_count,
                                 const audio_format_t *fmt)
{
    (void)clk; (void)dins; (void)line_count; (void)fmt;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_bus_rx_enable(bool enable) { (void)enable; return ESP_ERR_NOT_SUPPORTED; }
bool audio_bus_rx_enabled(void) { return false; }

esp_err_t audio_bus_read(void *data, size_t bytes, size_t *read,
                         uint32_t timeout_ms)
{
    (void)data; (void)bytes; (void)timeout_ms;
    if (read) {
        *read = 0;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* SOC_I2S_SUPPORTED */
