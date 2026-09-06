/*
 * Nuvoton NAU8822 stereo codec with speaker driver.
 *
 * Two buses meet in this part: audio travels over I2S, which the `audio` group
 * owns, and control travels over I2C, which the `i2c` group owns. This driver
 * therefore borrows the I2C bus through i2c_require_bus() and
 * i2c_device_handle() rather than opening one of its own, the same way
 * ina237_cmd.c and sht4x_cmd.c do.
 *
 * Register map and bit positions follow the register-compatible layout used by
 * the mainline Linux driver (sound/soc/codecs/nau8822.{c,h}): a 7-bit register
 * address and a 9-bit value packed into two bytes. The names below are that
 * driver's names, so the two can be compared directly.
 *
 * The default addresses are 0x1a with CSB low and 0x1b with CSB high.
 */
#include "app_bringup.h"
#include "codec_nau8822.h"
#include "i2c.h"

#include <math.h>

#define NAU8822_ADDR_LOW  0x1a
#define NAU8822_ADDR_HIGH 0x1b

#define XFER_TIMEOUT_MS 1000
#define RESET_WAIT_MS   20

/* Registers used here. The part has 64; only the ones this driver programs are
 * named, and `audio nau8822 reg` reaches the rest. */
#define REG_RESET              0x00
#define REG_POWER_MANAGEMENT_1 0x01
#define REG_POWER_MANAGEMENT_2 0x02
#define REG_POWER_MANAGEMENT_3 0x03
#define REG_AUDIO_INTERFACE    0x04
#define REG_CLOCKING           0x06
#define REG_ADDITIONAL_CONTROL 0x07
#define REG_DAC_CONTROL        0x0a
#define REG_LEFT_DAC_VOLUME    0x0b
#define REG_RIGHT_DAC_VOLUME   0x0c
#define REG_ADC_CONTROL        0x0e
#define REG_LEFT_ADC_VOLUME    0x0f
#define REG_RIGHT_ADC_VOLUME   0x10
#define REG_INPUT_CONTROL      0x2c
#define REG_LEFT_INPPGA        0x2d
#define REG_RIGHT_INPPGA       0x2e
#define REG_LEFT_ADC_BOOST     0x2f
#define REG_RIGHT_ADC_BOOST    0x30
#define REG_OUTPUT_CONTROL     0x31
#define REG_LEFT_MIXER         0x32
#define REG_RIGHT_MIXER        0x33
#define REG_LHP_VOLUME         0x34
#define REG_RHP_VOLUME         0x35
#define REG_LSPKOUT_VOLUME     0x36
#define REG_RSPKOUT_VOLUME     0x37
#define REG_DEVICE_REVISION    0x3e
#define REG_DEVICE_ID          0x3f
#define REG_COUNT              0x40

/* Power Management 1 (0x01) */
#define PM1_REFIMP_80K   0x01
#define PM1_IOBUF_EN     (1 << 2)
#define PM1_ABIAS_EN     (1 << 3)
#define PM1_MICBIAS_EN   (1 << 4)

/* Power Management 2 (0x02): the capture chain at the bottom of the register,
 * headphone drivers at the top. Every stage powers up separately, and the
 * capture path is only as alive as its least-powered stage. */
#define PM2_ADCL_EN      (1 << 0)
#define PM2_ADCR_EN      (1 << 1)
#define PM2_INPGAL_EN    (1 << 2)
#define PM2_INPGAR_EN    (1 << 3)
#define PM2_BOOSTL_EN    (1 << 4)
#define PM2_BOOSTR_EN    (1 << 5)
#define PM2_LHP_EN       (1 << 7)
#define PM2_RHP_EN       (1 << 8)

/*
 * ADC Control (0x0e). Only the high-pass filter is touched.
 *
 * Bit 8 enables it and it is on at reset, which is the right default: a MEMS
 * or electret input carries a DC offset that is not signal, and leaving it in
 * makes a quiet channel look busy -- exactly the confusion `audio record`
 * sidesteps by quoting RMS about the mean. The cut-off bits are deliberately
 * left alone: the mainline Linux driver defines the cut-off as bits 6:4 and
 * the ADC oversampling switch as bit 5, which cannot both be right, and
 * nothing here needs either.
 */
#define ADC_HPF_EN       (1 << 8)

/* Input Control (0x2c): what reaches each channel's input mixer. */
#define IN_LMICP         (1 << 0)
#define IN_LMICN         (1 << 1)
#define IN_L2            (1 << 2)
#define IN_RMICP         (1 << 4)
#define IN_RMICN         (1 << 5)
#define IN_R2            (1 << 6)

/*
 * Input PGA (0x2d/0x2e). Six gain bits from -12 dB in 0.75 dB steps, so code
 * 16 is 0 dB and code 63 is +35.25 dB. Bit 8 latches the pair, as with the
 * output volumes.
 */
#define PGA_MUTE         (1 << 6)
#define PGA_CODE_MAX     63
#define PGA_MIN_DB       (-12.0)
#define PGA_STEP_DB      0.75

/*
 * ADC Boost (0x2f/0x30). Bit 8 is a fixed +20 dB ahead of the ADC for the
 * microphone path. Bits 6:4 are the line input's own way into the boost mixer,
 * bypassing the PGA entirely: code 0 mutes, code 1 is -15 dB, 3 dB a step, so
 * code 6 is 0 dB and code 7 is +3 dB.
 */
#define BOOST_PGA_20DB   (1 << 8)
#define BOOST_L2_SHIFT   4
#define BOOST_CODE_MAX   7
#define BOOST_MIN_DB     (-15.0)
#define BOOST_STEP_DB    3.0
#define BOOST_CODE_0DB   6

/* Power Management 3 (0x03) */
#define PM3_DACL_EN      (1 << 0)
#define PM3_DACR_EN      (1 << 1)
#define PM3_LMIX_EN      (1 << 2)
#define PM3_RMIX_EN      (1 << 3)
#define PM3_LSPK_EN      (1 << 5)
#define PM3_RSPK_EN      (1 << 6)

/* Audio Interface (0x04) */
#define AIF_WLEN_SHIFT   5
#define AIF_FMT_I2S      (0x2 << 3)

/* Clocking (0x06). CLKM selects MCLK or the PLL as the system clock; MCLKSEL
 * divides it down to the internal 256*fs. CLKIOEN 0 keeps the part a slave,
 * which is what it must be with the ESP32 mastering the bus. */
#define CLK_MCLKSEL_SHIFT 5
#define CLK_CLKM_PLL      (1 << 8)
#define CLK_MASTER        0x01

/* DAC Control (0x0a) */
#define DAC_SOFTMUTE     (1 << 6)

/* Mixer control (0x32/0x33): route the DAC into the output mixer, nothing else. */
#define MIX_DAC2MIX      (1 << 0)

/* Volume registers latch on a write with bit 8 set, which updates the left and
 * right of a pair together so the two channels never step apart. */
#define VOL_UPDATE       (1 << 8)
#define VOL_MUTE         (1 << 6)
/* 0x00 is -57 dB and 0x39 is 0 dB; the six values above that are gain, which
 * this driver does not use -- clipping into an amplifier during bring-up is
 * not a useful default. */
#define VOL_0DB          0x39

typedef enum {
    ROUTE_HEADPHONE,
    ROUTE_SPEAKER,
    ROUTE_BOTH,
} route_t;

/*
 * Which analog input reaches the ADC.
 *
 * The part offers two routes and they are not interchangeable. A microphone
 * goes in differentially on MICP/MICN, through the input mixer and the PGA,
 * and wants mic bias and usually the +20 dB boost. A line signal goes in on
 * L2/R2 straight to the boost mixer, skipping the PGA, and wants neither --
 * bias on a line output is at best pointless and 20 dB of gain on one clips
 * immediately.
 *
 * There is no default worth guessing. Which is fitted is a board fact, so
 * `init` powers the ADC down and waits to be told.
 */
typedef enum {
    INPUT_OFF,
    INPUT_MIC,
    INPUT_LINE,
} input_t;

static uint8_t address = NAU8822_ADDR_LOW;
static uint16_t shadow[REG_COUNT];
static bool initialized;
static bool readback_works;
static uint16_t device_id_seen;
static route_t route = ROUTE_BOTH;
static int volume_pct = 60;

static input_t input_path = INPUT_OFF;
static bool input_boost;            /* the +20 dB stage, microphone path only */
static int pga_code = 16;           /* 0 dB */
static int line_code = BOOST_CODE_0DB;

/* ------------------------------------------------------------------ */
/* Register access                                                     */
/* ------------------------------------------------------------------ */

/*
 * A write is one 16-bit word: seven address bits, then nine data bits. The
 * ninth data bit is therefore the low bit of the first byte.
 */
static esp_err_t write_reg(uint8_t reg, uint16_t value)
{
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = i2c_device_handle(address, &dev);
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t bytes[2] = {
        (uint8_t)((reg << 1) | ((value >> 8) & 0x01)),
        (uint8_t)(value & 0xff),
    };

    err = i2c_master_transmit(dev, bytes, sizeof(bytes), XFER_TIMEOUT_MS);
    if (err == ESP_OK && reg < REG_COUNT) {
        shadow[reg] = value & 0x1ff;
    }
    return err;
}

/*
 * Reads use the same framing in reverse: the register is addressed in one
 * byte, and two bytes come back with the ninth data bit at the bottom of the
 * first.
 *
 * Not every part in this family answers reads at all -- the WM8978 this is
 * register-compatible with is write-only -- so a failure here is reported as
 * "cannot read" rather than "no device", and the shadow copy carries on.
 */
static esp_err_t read_reg(uint8_t reg, uint16_t *value)
{
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = i2c_device_handle(address, &dev);
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t pointer = (uint8_t)(reg << 1);
    uint8_t bytes[2] = {0, 0};

    err = i2c_master_transmit_receive(dev, &pointer, 1, bytes, sizeof(bytes),
                                      XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    *value = (uint16_t)(((bytes[0] & 0x01) << 8) | bytes[1]);
    return ESP_OK;
}

/*
 * Read-modify-write through the shadow.
 *
 * The shadow is not an optimisation. Several registers pack unrelated fields,
 * and if the part turns out not to support reads there is no other way to
 * change one field without destroying its neighbours. Every register this
 * driver touches during init is written whole, so the shadow starts correct
 * rather than assuming the reset defaults.
 */
static esp_err_t update_reg(uint8_t reg, uint16_t mask, uint16_t value)
{
    uint16_t merged = (uint16_t)((shadow[reg] & ~mask) | (value & mask));
    return write_reg(reg, merged);
}

/* ------------------------------------------------------------------ */
/* Format and clocking                                                 */
/* ------------------------------------------------------------------ */

/* Word length field: 16, 20, 24 and 32 bits are codes 0 to 3. */
static int wlen_code(uint8_t bits)
{
    switch (bits) {
    case 20: return 1;
    case 24: return 2;
    case 32: return 3;
    default: return 0;
    }
}

/*
 * MCLK divider. The part wants an internal clock of 256*fs, and the divider
 * choices are 1, 1.5, 2, 3, 4, 6, 8 and 12 -- held here in tenths so the 1.5
 * is exact. So a 256x MCLK divides by 1 and a 384x MCLK divides by 1.5.
 */
static const int mclk_scaler_tenths[] = {10, 15, 20, 30, 40, 60, 80, 120};

static int mclk_divider_code(int mclk_multiple)
{
    int wanted = mclk_multiple * 10 / 256;
    for (size_t i = 0; i < sizeof(mclk_scaler_tenths) / sizeof(mclk_scaler_tenths[0]); i++) {
        if (mclk_scaler_tenths[i] == wanted) {
            return (int)i;
        }
    }
    return -1;
}

/* The sample-rate field only tunes internal filters, so the nearest entry is
 * the right answer rather than an exact match being required. */
static uint16_t samplerate_code(uint32_t rate)
{
    static const uint32_t rates[] = {48000, 32000, 24000, 16000, 12000, 8000};
    size_t best = 0;
    uint32_t best_delta = UINT32_MAX;

    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        uint32_t delta = rate > rates[i] ? rate - rates[i] : rates[i] - rate;
        if (delta < best_delta) {
            best_delta = delta;
            best = i;
        }
    }
    return (uint16_t)(best << 1);
}

static esp_err_t nau8822_configure(const audio_format_t *fmt)
{
    if (!fmt) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * In slave mode the part still needs a master clock; it has no way to
     * synthesise one from BCLK alone. Without it every register write succeeds
     * and nothing comes out, which is a miserable thing to debug, so refuse
     * up front and say what to do.
     */
    if (fmt->mclk_pin < 0) {
        diag_error("The NAU8822 needs MCLK, and the bus was opened without it");
        diag_printf("Re-open the bus with the pin, for example "
                  "'audio bus <bclk> <ws> <dout> mclk <pin>'.\n");
        return ESP_ERR_NOT_SUPPORTED;
    }

    int divider = mclk_divider_code(fmt->mclk_multiple);
    if (divider < 0) {
        diag_error("An MCLK of %dx the sample rate cannot be divided to the 256x "
                 "the codec needs", fmt->mclk_multiple);
        diag_printf("Use 'mclkmult 256', 384, 512 or 768 on 'audio bus'.\n");
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t err = write_reg(REG_AUDIO_INTERFACE,
                              (uint16_t)(AIF_FMT_I2S |
                                         (wlen_code(fmt->bits) << AIF_WLEN_SHIFT)));
    if (err != ESP_OK) {
        return err;
    }

    /* Slave, clocked from MCLK rather than the PLL. */
    err = write_reg(REG_CLOCKING, (uint16_t)(divider << CLK_MCLKSEL_SHIFT));
    if (err != ESP_OK) {
        return err;
    }

    return write_reg(REG_ADDITIONAL_CONTROL, samplerate_code(fmt->rate_hz));
}

/* ------------------------------------------------------------------ */
/* Volume and routing                                                  */
/* ------------------------------------------------------------------ */

/*
 * Percent maps onto the analog output attenuator, 0 dB at the top. The digital
 * DAC volume stays at full scale: attenuating in the digital domain throws
 * away bits, and on a bring-up bench the question is usually whether the
 * analog path works at all.
 */
static esp_err_t apply_volume(void)
{
    uint16_t level = (volume_pct <= 0)
        ? VOL_MUTE
        : (uint16_t)((volume_pct * VOL_0DB + 50) / 100);

    struct { uint8_t left, right; bool active; } pairs[] = {
        {REG_LHP_VOLUME, REG_RHP_VOLUME, route != ROUTE_SPEAKER},
        {REG_LSPKOUT_VOLUME, REG_RSPKOUT_VOLUME, route != ROUTE_HEADPHONE},
    };

    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        uint16_t value = pairs[i].active ? level : VOL_MUTE;
        esp_err_t err = write_reg(pairs[i].left, value);
        if (err != ESP_OK) {
            return err;
        }
        /* Bit 8 on the second write latches both channels at once. */
        err = write_reg(pairs[i].right, (uint16_t)(value | VOL_UPDATE));
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

/*
 * Analog gain on the selected input path.
 *
 * The two paths take their gain from different registers, so this follows
 * whichever is selected rather than pretending there is one control. Nothing
 * is written for INPUT_OFF: the stages are unpowered and the values are
 * restored when a path is chosen again.
 */
static esp_err_t apply_input_gain(void)
{
    if (input_path == INPUT_MIC) {
        uint16_t value = (uint16_t)pga_code;
        esp_err_t err = write_reg(REG_LEFT_INPPGA, value);
        if (err != ESP_OK) {
            return err;
        }
        /* Bit 8 latches both channels together, as for the output volumes. */
        return write_reg(REG_RIGHT_INPPGA, (uint16_t)(value | VOL_UPDATE));
    }

    if (input_path == INPUT_LINE) {
        uint16_t value = (uint16_t)(line_code << BOOST_L2_SHIFT);
        esp_err_t err = write_reg(REG_LEFT_ADC_BOOST, value);
        if (err != ESP_OK) {
            return err;
        }
        return write_reg(REG_RIGHT_ADC_BOOST, value);
    }

    return ESP_OK;
}

/*
 * Everything from the input pins to the ADC.
 *
 * Written as a whole rather than as incremental edits because these registers
 * pack unrelated fields, and a half-configured analog chain is the kind of
 * state that produces a plausible signal from the wrong place.
 */
static esp_err_t apply_input(void)
{
    uint16_t control = 0;
    uint16_t boost = 0;

    switch (input_path) {
    case INPUT_MIC:
        control = IN_LMICP | IN_LMICN | IN_RMICP | IN_RMICN;
        if (input_boost) {
            boost = BOOST_PGA_20DB;
        }
        break;
    case INPUT_LINE:
        /* L2/R2 reach the boost mixer directly; the input mixer stays empty so
         * the PGA contributes nothing to what the ADC sees. */
        control = 0;
        boost = (uint16_t)(line_code << BOOST_L2_SHIFT);
        break;
    case INPUT_OFF:
        break;
    }

    esp_err_t err = write_reg(REG_INPUT_CONTROL, control);
    if (err != ESP_OK) {
        return err;
    }
    err = write_reg(REG_LEFT_ADC_BOOST, boost);
    if (err != ESP_OK) {
        return err;
    }
    err = write_reg(REG_RIGHT_ADC_BOOST, boost);
    if (err != ESP_OK) {
        return err;
    }

    /* Unmute the PGA only on the path that uses it. */
    uint16_t pga = (input_path == INPUT_MIC) ? (uint16_t)pga_code
                                             : (uint16_t)(PGA_MUTE);
    err = write_reg(REG_LEFT_INPPGA, pga);
    if (err != ESP_OK) {
        return err;
    }
    err = write_reg(REG_RIGHT_INPPGA, (uint16_t)(pga | VOL_UPDATE));
    if (err != ESP_OK) {
        return err;
    }

    /* High-pass on, and the digital ADC volume left at full scale for the same
     * reason the DAC's is: attenuating in the digital domain throws away bits
     * that the analog gain above should have set correctly instead. */
    err = write_reg(REG_ADC_CONTROL, ADC_HPF_EN);
    if (err != ESP_OK) {
        return err;
    }
    err = write_reg(REG_LEFT_ADC_VOLUME, 0xff);
    if (err != ESP_OK) {
        return err;
    }
    return write_reg(REG_RIGHT_ADC_VOLUME, 0xff | VOL_UPDATE);
}

/*
 * Power management for both directions at once.
 *
 * These bits share registers across the playback and capture chains -- the
 * headphone drivers sit in the same register as the ADC and its front end --
 * so the whole word has to be computed from the whole of the driver's state.
 * Writing the output bits alone, as an earlier version did, silently powered
 * the capture path back down every time the route changed.
 */
static esp_err_t apply_power(void)
{
    uint16_t pm1 = PM1_REFIMP_80K | PM1_IOBUF_EN | PM1_ABIAS_EN;
    uint16_t pm2 = 0;
    uint16_t pm3 = PM3_DACL_EN | PM3_DACR_EN | PM3_LMIX_EN | PM3_RMIX_EN;

    if (route != ROUTE_SPEAKER) {
        pm2 |= PM2_LHP_EN | PM2_RHP_EN;
    }
    if (route != ROUTE_HEADPHONE) {
        pm3 |= PM3_LSPK_EN | PM3_RSPK_EN;
    }

    if (input_path != INPUT_OFF) {
        pm2 |= PM2_ADCL_EN | PM2_ADCR_EN | PM2_BOOSTL_EN | PM2_BOOSTR_EN;
    }
    if (input_path == INPUT_MIC) {
        /* Only the microphone path runs through the input mixer and the PGA,
         * and only it needs bias for an electret capsule. */
        pm2 |= PM2_INPGAL_EN | PM2_INPGAR_EN;
        pm1 |= PM1_MICBIAS_EN;
    }

    esp_err_t err = write_reg(REG_POWER_MANAGEMENT_1, pm1);
    if (err != ESP_OK) {
        return err;
    }
    err = write_reg(REG_POWER_MANAGEMENT_2, pm2);
    if (err != ESP_OK) {
        return err;
    }
    err = write_reg(REG_POWER_MANAGEMENT_3, pm3);
    if (err != ESP_OK) {
        return err;
    }

    return apply_volume();
}

static esp_err_t nau8822_set_volume(int percent)
{
    if (!initialized) {
        diag_error("The NAU8822 has not been initialized; run "
                 "'audio-nau8822 init' first");
        return ESP_ERR_NOT_SUPPORTED;
    }
    volume_pct = percent;
    return apply_volume();
}

static esp_err_t nau8822_set_mute(bool mute)
{
    if (!initialized) {
        diag_error("The NAU8822 has not been initialized; run "
                 "'audio-nau8822 init' first");
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* The DAC's own soft mute ramps rather than cutting, so it does not click,
     * and it silences every output at once whatever the routing is. */
    return update_reg(REG_DAC_CONTROL, DAC_SOFTMUTE, mute ? DAC_SOFTMUTE : 0);
}

/* ------------------------------------------------------------------ */
/* Identity                                                            */
/* ------------------------------------------------------------------ */

/*
 * What counts as evidence a real part answered.
 *
 * An ACK at the address proves something is there; the device ID register
 * proves what. Reads are attempted but not required, because the family this
 * part is register-compatible with is write-only -- so a part that will not
 * read back is reported as unverified rather than as absent, and the status
 * output always says which of the two happened.
 */
static esp_err_t nau8822_probe(void)
{
    uint16_t id = 0;
    if (read_reg(REG_DEVICE_ID, &id) == ESP_OK && id != 0x000 && id != 0x1ff) {
        readback_works = true;
        device_id_seen = id;
        return ESP_OK;
    }

    readback_works = false;
    device_id_seen = 0;

    /*
     * No readback, so the only remaining evidence is whether a write is
     * acknowledged. The reset register is the one safe place to send it:
     * every other register would leave the part in a state nobody chose, and
     * the caller resets it immediately afterwards regardless.
     */
    return write_reg(REG_RESET, 0x000);
}

/* ------------------------------------------------------------------ */
/* Bring-up                                                            */
/* ------------------------------------------------------------------ */

static esp_err_t nau8822_reset(void)
{
    /* Any write to register 0 resets the part. */
    esp_err_t err = write_reg(REG_RESET, 0x000);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(RESET_WAIT_MS) + 1);

    /* Reset invalidates every cached value. Init writes whole registers from
     * here, so a cleared shadow is accurate rather than assumed. */
    memset(shadow, 0, sizeof(shadow));
    return ESP_OK;
}

static void nau8822_status(void);

static void nau8822_detach(void)
{
    if (initialized) {
        /* Mute before powering down, then drop the analog blocks, so the
         * outputs do not thump on the way out. */
        update_reg(REG_DAC_CONTROL, DAC_SOFTMUTE, DAC_SOFTMUTE);
        write_reg(REG_POWER_MANAGEMENT_3, 0);
        write_reg(REG_POWER_MANAGEMENT_2, 0);
        write_reg(REG_POWER_MANAGEMENT_1, 0);
    }
    initialized = false;
}

const audio_codec_t nau8822_codec = {
    .name = "nau8822",
    .description = "Nuvoton NAU8822 stereo codec with speaker driver, I2C at 0x1a/0x1b",
    .directions = AUDIO_DIR_TX | AUDIO_DIR_RX,
    .needs_mclk = true,
    .probe = nau8822_probe,
    .configure = nau8822_configure,
    .set_volume = nau8822_set_volume,
    .set_mute = nau8822_set_mute,
    .status = nau8822_status,
    .detach = nau8822_detach,
};

int cmd_nau8822_init(int argc, char **argv)
{
    /* Control is I2C and audio is I2S, so both have to be up. Name the fix in
     * each case: the dependency crosses groups and is not obvious from here. */
    if (!i2c_require_bus()) {
        return -1;
    }
    if (!audio_bus_require()) {
        return -1;
    }

    if (argc > 1) {
        int value = 0;
        if (cli_parse_num_arg(argv[1], &value) < 0 ||
            (value != NAU8822_ADDR_LOW && value != NAU8822_ADDR_HIGH)) {
            diag_error("Address must be 0x%02x (CSB low) or 0x%02x (CSB high)",
                     NAU8822_ADDR_LOW, NAU8822_ADDR_HIGH);
            return -1;
        }
        address = (uint8_t)value;
    }

    esp_err_t err = nau8822_probe();
    if (err != ESP_OK) {
        diag_error("Nothing responded at 0x%02x: %s", address, esp_err_to_name(err));
        return -1;
    }

    if (readback_works) {
        diag_printf("0x%02x device ID 0x%03x\n", address, device_id_seen);
    } else {
        diag_printf("0x%02x acknowledges writes but does not read back, so it "
                  "cannot be identified further.\n", address);
    }

    if (nau8822_reset() != ESP_OK) {
        diag_error("Resetting the codec at 0x%02x failed", address);
        return -1;
    }

    /* Reference impedance, the I/O buffer and the analog bias: nothing analog
     * works until these are on. The PLL stays off -- the part is a slave and
     * takes its clock straight from MCLK. */
    err = write_reg(REG_POWER_MANAGEMENT_1,
                    PM1_REFIMP_80K | PM1_IOBUF_EN | PM1_ABIAS_EN);
    if (err != ESP_OK) {
        diag_error("Powering up the codec: %s", esp_err_to_name(err));
        return -1;
    }

    err = nau8822_configure(audio_bus_format());
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return -1; /* already explained */
    }
    if (err != ESP_OK) {
        diag_error("Configuring the audio interface: %s", esp_err_to_name(err));
        return -1;
    }

    /* Full-scale DAC, unmuted; the analog attenuator carries the volume. */
    if (write_reg(REG_DAC_CONTROL, 0) != ESP_OK ||
        write_reg(REG_LEFT_DAC_VOLUME, 0xff) != ESP_OK ||
        write_reg(REG_RIGHT_DAC_VOLUME, 0xff | VOL_UPDATE) != ESP_OK) {
        diag_error("Configuring the DAC failed");
        return -1;
    }

    /* Only the DAC reaches the output mixers; the bypass and aux paths stay
     * off so a tone that comes out came from the I2S input and nowhere else. */
    if (write_reg(REG_LEFT_MIXER, MIX_DAC2MIX) != ESP_OK ||
        write_reg(REG_RIGHT_MIXER, MIX_DAC2MIX) != ESP_OK) {
        diag_error("Configuring the output mixers failed");
        return -1;
    }

    /* Thermal shutdown on: the speaker driver can be asked for more than the
     * package will take, and a bring-up bench is exactly where that happens. */
    if (write_reg(REG_OUTPUT_CONTROL, 0x002) != ESP_OK) {
        diag_error("Configuring the output stage failed");
        return -1;
    }

    /*
     * The capture chain comes up configured but unpowered and disconnected.
     * Which input is fitted -- a microphone on MICP/MICN or a line signal on
     * L2/R2 -- is a board fact this driver cannot read, and guessing wrong is
     * not harmless: mic bias onto a line output, or the +20 dB boost on one,
     * clips instantly. `audio nau8822 input` makes the choice explicit.
     */
    input_path = INPUT_OFF;
    input_boost = false;
    pga_code = 16;
    line_code = BOOST_CODE_0DB;
    err = apply_input();
    if (err != ESP_OK) {
        diag_error("Configuring the capture path: %s", esp_err_to_name(err));
        return -1;
    }

    route = ROUTE_BOTH;
    err = apply_power();
    if (err != ESP_OK) {
        diag_error("Enabling the outputs: %s", esp_err_to_name(err));
        return -1;
    }

    initialized = true;
    audio_codec_attach(&nau8822_codec);

    diag_printf("NAU8822 initialized at 0x%02x, headphone and speaker outputs "
              "live at %d%%\n", address, volume_pct);
    diag_printf("The ADC is off. Run 'audio-nau8822 input mic' or '... input "
              "line' to record.\n");
    return 0;
}

static const char *route_name(void)
{
    switch (route) {
    case ROUTE_HEADPHONE: return "headphone";
    case ROUTE_SPEAKER:   return "speaker";
    default:              return "headphone + speaker";
    }
}

static const char *input_name(void)
{
    switch (input_path) {
    case INPUT_MIC:  return input_boost
                            ? "microphone (MICP/MICN, PGA, +20 dB boost)"
                            : "microphone (MICP/MICN, through the PGA)";
    case INPUT_LINE: return "line (L2/R2, straight to the boost mixer)";
    default:         return "off";
    }
}

/* The gain actually programmed, which is not what was asked for: both paths
 * quantise, one to 0.75 dB and the other to 3. */
static double input_gain_db(void)
{
    if (input_path == INPUT_MIC) {
        double db = PGA_MIN_DB + pga_code * PGA_STEP_DB;
        return input_boost ? db + 20.0 : db;
    }
    if (input_path == INPUT_LINE) {
        return BOOST_MIN_DB + (line_code - 1) * BOOST_STEP_DB;
    }
    return 0.0;
}

static void nau8822_status(void)
{
    diag_printf("         I2C 0x%02x, route %s, volume %d%%\n", address,
              route_name(), volume_pct);
    if (input_path == INPUT_OFF) {
        diag_printf("         Input off; 'audio-nau8822 input mic' or "
                  "'... input line' powers the ADC\n");
    } else {
        diag_printf("         Input %s at %+.2f dB\n", input_name(),
                  input_gain_db());
    }

    if (readback_works) {
        uint16_t id = 0;
        uint16_t revision = 0;
        read_reg(REG_DEVICE_ID, &id);
        read_reg(REG_DEVICE_REVISION, &revision);
        diag_printf("         Device ID 0x%03x, revision 0x%03x, read back live\n",
                  id, revision);
    } else {
        diag_printf("         Registers do not read back; values shown are the "
                  "driver's shadow copy\n");
    }

    diag_printf("         DAC %s\n",
              (shadow[REG_DAC_CONTROL] & DAC_SOFTMUTE) ? "muted" : "unmuted");
}

int cmd_nau8822_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!initialized) {
        diag_printf("NAU8822 is not initialized. Run 'audio-nau8822 init "
                  "[address]'.\n");
        return 0;
    }

    diag_printf("NAU8822 initialized\n");
    nau8822_status();
    return 0;
}

int cmd_nau8822_reg(int argc, char **argv)
{
    if (!i2c_require_bus()) {
        return -1;
    }
    if (argc < 2) {
        diag_printf("Usage: reg <n> [value]\n");
        return -1;
    }

    int reg = 0;
    if (cli_parse_num_arg(argv[1], &reg) < 0 || reg < 0 || reg >= REG_COUNT) {
        diag_error("Register must be 0x00-0x%02x", REG_COUNT - 1);
        return -1;
    }

    if (argc > 2) {
        int value = 0;
        if (cli_parse_num_arg(argv[2], &value) < 0 || value < 0 || value > 0x1ff) {
            diag_error("Value must be 0x000-0x1ff (the registers are 9 bits wide)");
            return -1;
        }
        esp_err_t err = write_reg((uint8_t)reg, (uint16_t)value);
        if (err != ESP_OK) {
            diag_error("Writing register 0x%02x: %s", reg, esp_err_to_name(err));
            return -1;
        }
        diag_printf("R%d (0x%02x) <- 0x%03x\n", reg, reg, value);
        return 0;
    }

    diag_printf("R%d (0x%02x) shadow 0x%03x", reg, reg, shadow[reg]);

    uint16_t live = 0;
    if (read_reg((uint8_t)reg, &live) == ESP_OK) {
        diag_printf(", device 0x%03x%s\n", live,
                  live == shadow[reg] ? "" : "  <- differs");
    } else {
        diag_printf(", device does not read back\n");
    }
    return 0;
}

int cmd_nau8822_route(int argc, char **argv)
{
    if (!initialized) {
        diag_error("The NAU8822 has not been initialized; run "
                 "'audio-nau8822 init' first");
        return -1;
    }
    if (argc < 2) {
        diag_printf("Usage: route <hp|speaker|both>   (currently %s)\n", route_name());
        return -1;
    }

    if (strcasecmp(argv[1], "hp") == 0 || strcasecmp(argv[1], "headphone") == 0) {
        route = ROUTE_HEADPHONE;
    } else if (strcasecmp(argv[1], "speaker") == 0 || strcasecmp(argv[1], "spk") == 0) {
        route = ROUTE_SPEAKER;
    } else if (strcasecmp(argv[1], "both") == 0) {
        route = ROUTE_BOTH;
    } else {
        diag_error("Route must be hp, speaker or both, not '%s'", argv[1]);
        return -1;
    }

    esp_err_t err = apply_power();
    if (err != ESP_OK) {
        diag_error("Switching the output route: %s", esp_err_to_name(err));
        return -1;
    }

    diag_printf("Route %s\n", route_name());
    return 0;
}

/* ------------------------------------------------------------------ */
/* Capture                                                             */
/* ------------------------------------------------------------------ */

int cmd_nau8822_input(int argc, char **argv)
{
    if (!initialized) {
        diag_error("The NAU8822 has not been initialized; run "
                 "'audio-nau8822 init' first");
        return -1;
    }

    if (argc < 2) {
        diag_printf("Input %s\n", input_name());
        if (input_path != INPUT_OFF) {
            diag_printf("Gain %+.2f dB\n", input_gain_db());
        }
        diag_printf("Usage: input <mic|line|off> [boost]\n");
        return 0;
    }

    input_t wanted;
    if (strcasecmp(argv[1], "mic") == 0) {
        wanted = INPUT_MIC;
    } else if (strcasecmp(argv[1], "line") == 0) {
        wanted = INPUT_LINE;
    } else if (strcasecmp(argv[1], "off") == 0) {
        wanted = INPUT_OFF;
    } else {
        diag_error("Input must be mic, line or off, not '%s'", argv[1]);
        return -1;
    }

    bool boost = false;
    if (argc > 2) {
        if (strcasecmp(argv[2], "boost") != 0) {
            diag_error("Unexpected argument '%s'; the only option is 'boost'",
                     argv[2]);
            return -1;
        }
        if (wanted != INPUT_MIC) {
            /* The +20 dB stage sits between the PGA and the ADC, and the line
             * path does not go through it. Silently ignoring the word would
             * leave the user believing in gain that is not there. */
            diag_error("'boost' is the +20 dB stage on the microphone path; the "
                     "line input does not pass through it");
            diag_printf("Use 'audio-nau8822 input line' then 'audio-nau8822 gain "
                      "<db>', or 'audio-nau8822 input mic boost' for a real "
                      "microphone.\n");
            return -1;
        }
        boost = true;
    }

    input_t previous = input_path;
    bool previous_boost = input_boost;
    input_path = wanted;
    input_boost = boost;

    esp_err_t err = apply_input();
    if (err == ESP_OK) {
        err = apply_power();
    }
    if (err != ESP_OK) {
        input_path = previous;
        input_boost = previous_boost;
        diag_error("Switching the input: %s", esp_err_to_name(err));
        return -1;
    }

    diag_printf("Input %s\n", input_name());
    if (input_path != INPUT_OFF) {
        diag_printf("Gain %+.2f dB\n", input_gain_db());

        /*
         * The ADC now has somewhere to send samples only if the transport was
         * given a receive line. Worth saying here rather than letting `audio
         * record` report a dead input on a codec that is working perfectly.
         */
        if (audio_bus_rx_mode() != AUDIO_RX_STD) {
            diag_printf("The I2S bus has no receive line, so nothing will reach "
                      "'audio record'. Re-run 'audio bus <bclk> <ws> <dout> "
                      "din <pin> mclk <pin>' with the codec's ADCOUT.\n");
        }
    }
    return 0;
}

int cmd_nau8822_gain(int argc, char **argv)
{
    if (!initialized) {
        diag_error("The NAU8822 has not been initialized; run "
                 "'audio-nau8822 init' first");
        return -1;
    }
    if (input_path == INPUT_OFF) {
        diag_error("No input is selected, so there is no gain to set");
        diag_printf("Run 'audio-nau8822 input mic' or '... input line' first.\n");
        return -1;
    }

    if (argc < 2) {
        diag_printf("Gain %+.2f dB on the %s\n", input_gain_db(),
                  input_path == INPUT_MIC ? "microphone path" : "line path");
        return 0;
    }

    double db = 0.0;
    if (cli_parse_double_arg(argv[1], &db) < 0) {
        diag_error("Gain must be a number of decibels, not '%s'", argv[1]);
        return -1;
    }

    /*
     * The two paths have different ranges and different step sizes because
     * they are different amplifiers. Reporting the achieved value rather than
     * the requested one is the same habit as `gpio pwm set` and `sd spi`.
     */
    if (input_path == INPUT_MIC) {
        double max_db = PGA_MIN_DB + PGA_CODE_MAX * PGA_STEP_DB;
        if (db < PGA_MIN_DB || db > max_db) {
            diag_error("The microphone PGA covers %+.2f to %+.2f dB", PGA_MIN_DB,
                     max_db);
            diag_printf("The +20 dB boost stage is separate: 'audio-nau8822 "
                      "input mic boost'.\n");
            return -1;
        }
        pga_code = (int)lrint((db - PGA_MIN_DB) / PGA_STEP_DB);
    } else {
        double max_db = BOOST_MIN_DB + (BOOST_CODE_MAX - 1) * BOOST_STEP_DB;
        if (db < BOOST_MIN_DB || db > max_db) {
            diag_error("The line input covers %+.0f to %+.0f dB in 3 dB steps",
                     BOOST_MIN_DB, max_db);
            return -1;
        }
        line_code = (int)lrint((db - BOOST_MIN_DB) / BOOST_STEP_DB) + 1;
    }

    esp_err_t err = apply_input_gain();
    if (err != ESP_OK) {
        diag_error("Setting the input gain: %s", esp_err_to_name(err));
        return -1;
    }

    diag_printf("Gain %+.2f dB\n", input_gain_db());
    return 0;
}
