/*
 * Knowles SPH0645LM4H-B I2S MEMS microphone.
 *
 * No control bus, no registers, no address -- the same shape as the NS4168 at
 * the other end of the wire. What earns it a driver rather than leaving it to
 * `audio bus ... din <pin>` is that the datasheet imposes three constraints
 * which are invisible from the command line and which fail *quietly*:
 *
 *   The oversampling ratio is fixed at 64, so WS must be BCLK/64. Two 32-bit
 *   slots give exactly that. Ask for 16-bit slots and the frame is 32 clocks,
 *   the part is fed a word-select at twice the rate it expects, and what comes
 *   back is not silence but plausible-looking rubbish.
 *
 *   The clock has to be 2.048 to 4.096 MHz, which with 64 clocks per frame
 *   means a sample rate of 32 to 64 kHz and nothing else. Below 900 kHz the
 *   part deliberately sleeps and tri-states its data pin, which reads as a
 *   dead microphone rather than as a misconfiguration.
 *
 *   SELECT decides *when* the part drives the shared data line, and therefore
 *   which slot it lands in. It is usually a board strap, so the software
 *   cannot read it and has to be told.
 *
 * All figures are from the Knowles SPH0645LM4H-B Rev B datasheet: Table 2
 * (Clock Frequency 2048/3072/4096 kHz, Sensitivity -26 dBFS at 94 dB SPL, SNR
 * 65 dB(A), AOP 120 dB SPL), Table 3 (Data Format: 24 bits, 18-bit precision,
 * LSBs filled with zeros) and the Interface Description on page 6.
 *
 * NOT VERIFIED AGAINST A REAL PART. There is no SPH0645 on the bench; this is
 * transcribed from the datasheet. The 32-bit capture path it depends on has
 * been exercised on hardware through the internal loopback, but the part
 * itself has not.
 */
#include "app_bringup.h"
#include "codec_sph0645.h"

#include "driver/gpio.h"

/* Datasheet Table 2: fCLOCK min 2.048 MHz, typ 3.072 MHz, max 4.096 MHz. */
#define BCLK_MIN_HZ 2048000
#define BCLK_MAX_HZ 4096000
/* Below this the part sleeps and tri-states DATA (Application Notes, p6). */
#define BCLK_SLEEP_HZ 900000

/* Two 32-bit slots per frame is the only framing that satisfies OSR 64. */
#define BCLK_PER_FRAME 64

/* Datasheet Table 2: 94 dB SPL in gives -26 dBFS out. */
#define SENSITIVITY_DBFS (-26.0)
#define SENSITIVITY_SPL  94.0

static bool attached;
static int sel_pin = -1;
static audio_channel_t slot = AUDIO_CHANNEL_LEFT;

static const char *slot_name(void)
{
    return slot == AUDIO_CHANNEL_RIGHT ? "right" : "left";
}

/*
 * Check the bus against what the part can actually do.
 *
 * Returns ESP_ERR_NOT_SUPPORTED once it has explained itself, per the
 * convention in audio.h -- the caller then stays quiet rather than appending
 * an error name that says less than this did.
 */
static esp_err_t sph0645_configure(const audio_format_t *fmt)
{
    if (!fmt) {
        return ESP_ERR_INVALID_STATE;
    }

    if (fmt->bits == 16) {
        STRRES_ERROR(STR_AUDIO_SPH0645_NEEDS_32_BIT);
        STRRES_PRINTF(STR_AUDIO_SPH0645_NEEDS_32_BIT_NOTE);
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint32_t bclk = audio_bus_bclk_hz();
    if (bclk == 0) {
        return ESP_OK;   /* clocks not up yet; nothing to check against */
    }

    if (bclk < BCLK_SLEEP_HZ) {
        STRRES_ERROR(STR_AUDIO_SPH0645_BCLK_TOO_SLOW, bclk / 1e6);
        STRRES_PRINTF(STR_AUDIO_SPH0645_BCLK_TOO_SLOW_NOTE,
                      BCLK_MIN_HZ / BCLK_PER_FRAME, BCLK_MIN_HZ / BCLK_PER_FRAME,
                      BCLK_MAX_HZ / BCLK_PER_FRAME);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (bclk < BCLK_MIN_HZ || bclk > BCLK_MAX_HZ) {
        STRRES_ERROR(STR_AUDIO_SPH0645_BCLK_OUT_OF_SPEC,
                     bclk / 1e6, BCLK_MIN_HZ / 1e6, BCLK_MAX_HZ / 1e6);
        STRRES_PRINTF(STR_AUDIO_SPH0645_BCLK_OUT_OF_SPEC_NOTE,
                      BCLK_MIN_HZ / BCLK_PER_FRAME, BCLK_MAX_HZ / BCLK_PER_FRAME);
        return ESP_ERR_NOT_SUPPORTED;
    }

    return ESP_OK;
}

static void sph0645_status(void)
{
    STRRES_PRINTF(STR_AUDIO_SPH0645_STATUS_SLOT, slot_name());
    if (sel_pin >= 0) {
        STRRES_PRINTF(STR_AUDIO_SPH0645_STATUS_SELECT_DRIVEN,
                      slot == AUDIO_CHANNEL_RIGHT ? "high" : "low", sel_pin);
    } else {
        STRRES_PRINTF(STR_AUDIO_SPH0645_STATUS_SELECT_STRAPPED);
    }

    uint32_t bclk = audio_bus_bclk_hz();
    if (bclk) {
        STRRES_PRINTF(STR_AUDIO_SPH0645_STATUS_BCLK,
                      bclk / 1e6, BCLK_MIN_HZ / 1e6, BCLK_MAX_HZ / 1e6);
    }

    /*
     * The precision is worth stating because it changes what the numbers in
     * `audio record` mean: the bottom 14 bits of a 32-bit slot are always
     * zero, so a reading of "0" in them is the part working as specified
     * rather than a truncation somewhere in this firmware.
     */
    STRRES_PRINTF(STR_AUDIO_SPH0645_STATUS_PRECISION);
    STRRES_PRINTF(STR_AUDIO_SPH0645_STATUS_SPL,
                  SENSITIVITY_SPL, SENSITIVITY_DBFS,
                  SENSITIVITY_DBFS - (SENSITIVITY_SPL - 70.0));

    /* No control bus, so the same caveat as the NS4168 applies in reverse. */
    STRRES_PRINTF(STR_AUDIO_SPH0645_STATUS_NO_BUS);

    if (slot == AUDIO_CHANNEL_LEFT) {
        STRRES_PRINTF(STR_AUDIO_SPH0645_STATUS_MIRROR_RIGHT);
    } else {
        STRRES_PRINTF(STR_AUDIO_SPH0645_STATUS_MIRROR_LEFT);
    }
}

static void sph0645_detach(void)
{
    if (sel_pin >= 0) {
        gpio_reset_pin((gpio_num_t)sel_pin);
    }
    sel_pin = -1;
    slot = AUDIO_CHANNEL_LEFT;
    attached = false;
}

const audio_codec_t sph0645_codec = {
    .name = "sph0645",
    .description = STR_AUDIO_SPH0645_GROUP_HELP,
    .directions = AUDIO_DIR_RX,
    .needs_mclk = false,
    .probe = NULL,          /* nothing to ask; the part only ever talks audio */
    .configure = sph0645_configure,
    .set_volume = NULL,     /* fixed sensitivity, set by the acoustics */
    .set_mute = NULL,
    .status = sph0645_status,
    .detach = sph0645_detach,
};

int cmd_sph0645_init(int argc, char **argv)
{
    /*
     * The receiver has to exist before the part is worth attaching, and
     * requiring it here is what makes the format checks below meaningful --
     * there is no bit clock to measure otherwise.
     */
    if (!audio_bus_rx_require()) {
        return -1;
    }
    if (audio_bus_rx_mode() != AUDIO_RX_STD) {
        STRRES_ERROR(STR_AUDIO_SPH0645_PDM_MODE_WRONG);
        STRRES_PRINTF(STR_AUDIO_SPH0645_PDM_MODE_NOTE);
        return -1;
    }

    int pin = -1;
    audio_channel_t which = AUDIO_CHANNEL_LEFT;
    bool slot_given = false;

    int index = 1;
    while (index < argc) {
        if (strcasecmp(argv[index], "left") == 0) {
            which = AUDIO_CHANNEL_LEFT;
            slot_given = true;
            index++;
        } else if (strcasecmp(argv[index], "right") == 0) {
            which = AUDIO_CHANNEL_RIGHT;
            slot_given = true;
            index++;
        } else if (strcasecmp(argv[index], "sel") == 0) {
            if (index + 1 >= argc) {
                STRRES_ERROR(STR_AUDIO_SPH0645_SEL_NEEDS_VALUE);
                return -1;
            }
            if (cli_parse_int_arg(argv[index + 1], &pin) < 0 ||
                !GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
                STRRES_ERROR(STR_AUDIO_SPH0645_SEL_INVALID, argv[index + 1]);
                return -1;
            }
            index += 2;
        } else {
            STRRES_ERROR(STR_AUDIO_SPH0645_UNEXPECTED_ARGUMENT, argv[index]);
            return -1;
        }
    }

    if (attached) {
        sph0645_detach();
    }

    slot = which;

    /*
     * SELECT low makes the part drive the data line while word-select is low,
     * which in I2S is the left slot; high puts it in the right. Driving it
     * from a GPIO is the useful case even though most boards strap it: it
     * turns "which slot is my microphone in" from a question into an
     * experiment you can run twice.
     */
    if (pin >= 0) {
        const gpio_config_t config = {
            .pin_bit_mask = BIT64(pin),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&config);
        if (err != ESP_OK) {
            STRRES_ERROR(STR_AUDIO_SPH0645_SEL_CONFIG_FAILED,
                         pin, esp_err_to_name(err));
            return -1;
        }
        sel_pin = pin;
        gpio_set_level((gpio_num_t)pin, slot == AUDIO_CHANNEL_RIGHT ? 1 : 0);
    }

    esp_err_t err = sph0645_configure(audio_bus_format());
    if (err != ESP_OK) {
        sph0645_detach();
        return -1;   /* configure() has explained, whatever the reason */
    }

    attached = true;
    audio_codec_attach(&sph0645_codec);

    STRRES_PRINTF(STR_AUDIO_SPH0645_ATTACHED, slot_name());
    if (!slot_given && sel_pin < 0) {
        STRRES_PRINTF(STR_AUDIO_SPH0645_ASSUMING_STRAPPED);
    }
    STRRES_PRINTF(STR_AUDIO_SPH0645_NEXT_STEP);
    return 0;
}

int cmd_sph0645_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!attached) {
        STRRES_PRINTF(STR_AUDIO_SPH0645_NOT_ATTACHED);
        return 0;
    }

    STRRES_PRINTF(STR_AUDIO_SPH0645_ATTACHED_STATUS);
    sph0645_status();
    return 0;
}
