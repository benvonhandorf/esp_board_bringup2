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

size_t audio_bus_rx_frame_bytes(void)
{
    /* PDM decimates to 16-bit whatever the transmit side is doing. */
    return 2 * ((rx_bits == 16 ? 16 : 32) / 8);
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
    if (clk) *clk = (rx_mode == AUDIO_RX_PDM) ? pin_pdm_clk : pin_bclk;
    if (din) *din = pin_din;
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
    if (rx_mode == AUDIO_RX_PDM) {
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
    i2s_chan_info_t info;
    if (rx_mode != AUDIO_RX_PDM || !rx_chan_info(&info)) {
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

    rx_running = false;
    rx_mode = AUDIO_RX_NONE;
    rx_bits = 16;
    pin_din = -1;
    pin_pdm_clk = -1;
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

esp_err_t audio_bus_open_pdm(int clk, int din, const audio_format_t *fmt)
{
    (void)clk; (void)din; (void)fmt;
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
