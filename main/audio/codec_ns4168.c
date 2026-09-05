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
        diag_error("No SD pin was given, so this amplifier cannot be muted from "
                 "the board");
        diag_printf("Re-attach with the pin ('audio-ns4168 init sd <pin>'), or "
                  "turn the signal down with 'audio tone <hz> <s> level 5'.\n");
        return ESP_ERR_NOT_SUPPORTED;
    }
    return drive(!mute);
}

static void ns4168_status(void)
{
    if (sd_pin < 0) {
        diag_printf("         SD pin not wired to a GPIO; the amplifier is "
                  "always enabled\n");
    } else {
        diag_printf("         SD GPIO %d, currently %s\n", sd_pin,
                  enabled ? "enabled" : "shut down");
    }
    /* Worth stating because it is the usual reason a mono amp is silent on a
     * board that is plainly clocking correctly. */
    diag_printf("         No control bus, so nothing here confirms the part is "
              "really an NS4168\n");
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
    .description = "NS4168 mono I2S class-D amplifier (no control bus)",
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
                diag_error("'sd' needs a pin number");
                return -1;
            }
            if (cli_parse_int_arg(argv[index + 1], &pin) < 0 ||
                !GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
                diag_error("SD must be an output-capable pin, not '%s'",
                         argv[index + 1]);
                return -1;
            }
            index += 2;
        } else {
            diag_error("Unexpected argument '%s'; the only option is 'sd <pin>'",
                     argv[index]);
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
            diag_error("Configuring GPIO %d as the SD pin: %s", sd_pin,
                     esp_err_to_name(err));
            sd_pin = -1;
            return -1;
        }
        drive(true);
    }

    attached = true;
    audio_codec_attach(&ns4168_codec);

    diag_printf("NS4168 attached");
    if (sd_pin >= 0) {
        diag_printf(" and enabled via GPIO %d\n", sd_pin);
    } else {
        diag_printf("; no SD pin given, so it is assumed hard-enabled\n");
    }

    /*
     * Which channel a mono amplifier takes is a board decision, not a software
     * one: the SD pin doubles as channel select through a resistor divider, so
     * the same "enable" that a GPIO provides may select left, right or the
     * average of the two depending on what is fitted. `audio tone 1000 3 left`
     * followed by `... right` is the quick way to find out which.
     */
    diag_printf("Mono part: use 'audio tone <hz> <s> left' and then 'right' to "
              "find which slot it plays.\n");
    return 0;
}

int cmd_ns4168_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!attached) {
        diag_printf("NS4168 is not attached. Run 'audio-ns4168 init [sd <pin>]'.\n");
        return 0;
    }

    diag_printf("NS4168 attached\n");
    ns4168_status();
    return 0;
}
