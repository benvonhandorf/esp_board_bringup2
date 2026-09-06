/*
 * NS4168 mono I2S class-D amplifier.
 *
 * There is no control bus. The part takes BCLK, LRCK and SDATA and drives a
 * speaker; everything configurable about it is set by resistors on the board.
 * That makes it the useful counterexample for the codec interface in audio.h:
 * if a part with one pin and no registers fits the same vtable as a codec with
 * sixty, the abstraction is at the right level. It implements set_mute and
 * nothing else, and `audio volume` says plainly that there is no volume to set.
 */
#include "app_bringup.h"
#include "codec_ns4168.h"

#include "driver/gpio.h"

/* Held low the amplifier is shut down; released high it plays. The datasheet
 * calls the pin SD, and it is the only input the part has. */
static int sd_pin = -1;
static bool enabled;
static bool attached;

static esp_err_t drive(bool on)
{
    if (sd_pin < 0) {
        return ESP_OK;
    }
    esp_err_t err = gpio_set_level((gpio_num_t)sd_pin, on ? 1 : 0);
    if (err == ESP_OK) {
        enabled = on;
    }
    return err;
}

static esp_err_t ns4168_set_mute(bool mute)
{
    if (sd_pin < 0) {
        STRRES_ERROR(STR_AUDIO_NS4168_NO_SD_PIN);
        STRRES_PRINTF(STR_AUDIO_NS4168_NO_SD_PIN_NOTE);
        return ESP_ERR_NOT_SUPPORTED;
    }
    return drive(!mute);
}

static void ns4168_status(void)
{
    if (sd_pin < 0) {
        STRRES_PRINTF(STR_AUDIO_NS4168_STATUS_NO_SD_PIN);
    } else {
        STRRES_PRINTF(STR_AUDIO_NS4168_STATUS_SD_PIN,
                      sd_pin, enabled ? "enabled" : "shut down");
    }
    /* Worth stating because it is the usual reason a mono amp is silent on a
     * board that is plainly clocking correctly. */
    STRRES_PRINTF(STR_AUDIO_NS4168_STATUS_NO_BUS);
}

static void ns4168_detach(void)
{
    /* Shut the amplifier down before anything stops its clocks, or it thumps
     * the speaker on the way out. */
    drive(false);
    if (sd_pin >= 0) {
        gpio_reset_pin((gpio_num_t)sd_pin);
    }
    sd_pin = -1;
    enabled = false;
    attached = false;
}

const audio_codec_t ns4168_codec = {
    .name = "ns4168",
    .description = STR_AUDIO_NS4168_GROUP_HELP,
    .directions = AUDIO_DIR_TX,
    .needs_mclk = false,
    .probe = NULL,
    .configure = NULL,
    .set_volume = NULL,   /* fixed gain, set by resistors on the board */
    .set_mute = ns4168_set_mute,
    .status = ns4168_status,
    .detach = ns4168_detach,
};

int cmd_ns4168_init(int argc, char **argv)
{
    /*
     * The clocks must already be running. An amplifier enabled into a dead
     * bus latches onto whatever the lines happen to be doing, and the ESP32
     * leaves them low, which some parts read as a DC input.
     */
    if (!audio_bus_require()) {
        return -1;
    }

    int pin = -1;
    int index = 1;
    while (index < argc) {
        if (strcasecmp(argv[index], "sd") == 0) {
            if (index + 1 >= argc) {
                STRRES_ERROR(STR_AUDIO_NS4168_SD_NEEDS_VALUE);
                return -1;
            }
            if (cli_parse_int_arg(argv[index + 1], &pin) < 0 ||
                !GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
                STRRES_ERROR(STR_AUDIO_NS4168_SD_INVALID, argv[index + 1]);
                return -1;
            }
            index += 2;
        } else {
            STRRES_ERROR(STR_AUDIO_NS4168_UNEXPECTED_ARGUMENT, argv[index]);
            return -1;
        }
    }

    if (attached) {
        ns4168_detach();
    }

    sd_pin = pin;
    if (sd_pin >= 0) {
        const gpio_config_t config = {
            .pin_bit_mask = BIT64(sd_pin),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&config);
        if (err != ESP_OK) {
            STRRES_ERROR(STR_AUDIO_NS4168_SD_CONFIG_FAILED,
                         sd_pin, esp_err_to_name(err));
            sd_pin = -1;
            return -1;
        }
        drive(true);
    }

    attached = true;
    audio_codec_attach(&ns4168_codec);

    STRRES_PRINTF(STR_AUDIO_NS4168_ATTACHED);
    if (sd_pin >= 0) {
        STRRES_PRINTF(STR_AUDIO_NS4168_ATTACHED_VIA_GPIO, sd_pin);
    } else {
        STRRES_PRINTF(STR_AUDIO_NS4168_ATTACHED_HARD_ENABLED);
    }

    /*
     * Which channel a mono amplifier takes is a board decision, not a software
     * one: the SD pin doubles as channel select through a resistor divider, so
     * the same "enable" that a GPIO provides may select left, right or the
     * average of the two depending on what is fitted. `audio tone 1000 3 left`
     * followed by `... right` is the quick way to find out which.
     */
    STRRES_PRINTF(STR_AUDIO_NS4168_MONO_NOTE);
    return 0;
}

int cmd_ns4168_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!attached) {
        STRRES_PRINTF(STR_AUDIO_NS4168_NOT_ATTACHED);
        return 0;
    }

    STRRES_PRINTF(STR_AUDIO_NS4168_ATTACHED_STATUS);
    ns4168_status();
    return 0;
}
