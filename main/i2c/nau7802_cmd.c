/*
 * Console commands for the Nuvoton NAU7802 24-bit bridge ADC.
 *
 * The device driver is the nau7802 shared component: it owns the register map, the
 * power-up sequencing and the scale arithmetic, and it formats no text. This
 * file owns the words. Every call below hands the driver parsed arguments and
 * turns the facts it returns back into the output documented in docs/i2c.md --
 * so the prose, which is most of what makes this part usable during bringup,
 * stays here where diag_printf() lives, and the device knowledge stays in a
 * component another project can depend on.
 */
#include "app_bringup.h"
#include "nau7802_cmd.h"
#include "i2c.h"

#include "nau7802.h"

#include <math.h>

#define MAX_SAMPLES     200
#define DEFAULT_SAMPLES 10

static nau7802_handle_t nau;

/* ------------------------------------------------------------------ */
/* Guards                                                             */
/* ------------------------------------------------------------------ */

/*
 * Create the driver handle on first use.
 *
 * Needs no bus and no device: `drdy` is a statement about how the board is
 * wired and is useful before either exists.
 */
static bool ensure_handle(void)
{
    if (nau) {
        return true;
    }

    const nau7802_config_t config = {
        .dev = NULL,
        .drdy_gpio = -1,
    };
    esp_err_t err = nau7802_create(&config, &nau);
    if (err != ESP_OK) {
        STRRES_ERROR(STR_I2C_NAU7802_DRIVER_ALLOC_FAILED, esp_err_to_name(err));
        return false;
    }
    return true;
}

/*
 * Require a bus, and hand the driver a fresh device handle.
 *
 * The handle is re-fetched on every command rather than kept, because i2c.c
 * owns it: `i2c bus` deletes every cached handle when it re-creates the bus,
 * and the cache recycles wholesale when it fills. A handle held across either
 * would dangle.
 */
static bool require_device(void)
{
    if (!ensure_handle() || !i2c_require_bus()) {
        return false;
    }

    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = i2c_device_handle(NAU7802_I2C_ADDRESS, &dev);
    if (err != ESP_OK) {
        STRRES_ERROR(STR_I2C_NAU7802_ADDRESSING_FAILED,
                     NAU7802_I2C_ADDRESS, esp_err_to_name(err));
        return false;
    }

    return nau7802_set_device(nau, dev) == ESP_OK;
}

/* As require_device(), but the converter must also have been brought up. The
 * readiness check comes first so that an un-run `init` is named before an
 * un-run `i2c bus`. */
static bool require_init(void)
{
    if (!ensure_handle() || !nau7802_is_ready(nau)) {
        STRRES_ERROR(STR_I2C_NAU7802_NOT_INITIALIZED);
        return false;
    }
    return require_device();
}

/* ------------------------------------------------------------------ */
/* Failure reporting                                                  */
/* ------------------------------------------------------------------ */

/*
 * Every path that reads a conversion can fail this way, and a DRDY timeout is
 * the one worth naming: a pin that is not really wired to DRDY times out here
 * rather than returning bad numbers, which is exactly what the pull-down on it
 * is for.
 */
static bool report_drdy_timeout(esp_err_t err)
{
    const int pin = nau7802_drdy_gpio(nau);

    if (err == ESP_ERR_TIMEOUT && pin >= 0) {
        STRRES_ERROR(STR_I2C_NAU7802_DRDY_TIMEOUT, pin);
        return true;
    }
    return false;
}

static void report_calibration_error(esp_err_t err)
{
    if (err == ESP_ERR_NAU7802_CAL_FAILED) {
        STRRES_ERROR(STR_I2C_NAU7802_CALIBRATION_CAL_ERR);
    } else if (err == ESP_ERR_TIMEOUT) {
        STRRES_ERROR(STR_I2C_NAU7802_CALIBRATION_UNFINISHED, esp_err_to_name(err));
    } else {
        STRRES_ERROR(STR_I2C_NAU7802_CALIBRATION_START_FAILED, esp_err_to_name(err));
    }
}

/* Report a failed batch read, naming how far it got. */
static void report_batch_error(esp_err_t err, const nau7802_stats_t *stats, int wanted)
{
    if (!report_drdy_timeout(err)) {
        STRRES_ERROR(STR_I2C_NAU7802_CONVERSION_FAILED, stats->samples + 1, wanted,
                     esp_err_to_name(err));
    }
}

/*
 * A converter pinned at either rail is not measuring anything. During bringup
 * this usually means the bridge is disconnected or miswired, the excitation is
 * missing, or the gain is too high for the signal -- all of which otherwise
 * show up as a large, confident-looking number. Advisory, like the driver's
 * own flag: the data is still reported.
 */
static void warn_if_saturated(const nau7802_stats_t *stats)
{
    if (stats->saturated) {
        STRRES_ERROR(STR_I2C_NAU7802_ADC_SATURATED, stats->saturation_percent);
    }
}

/* Take an averaged batch and report any failure. Returns 0 or -1. */
static int take_batch(int samples, nau7802_stats_t *stats)
{
    esp_err_t err = nau7802_read_average(nau, samples, stats);
    if (err != ESP_OK) {
        report_batch_error(err, stats, samples);
        return -1;
    }
    warn_if_saturated(stats);
    return 0;
}

/*
 * Say what a change to the analog path cost.
 *
 * The driver recalibrates, restarts conversions, flushes the stale and
 * unsettled ones and drops the tare and scale; all of that is invisible unless
 * it is reported, and the dropped scale in particular is the one a user needs
 * to hear about.
 */
static int report_change(esp_err_t err, const nau7802_change_report_t *change,
                         strres_id_t what)
{
    if (change->scale_invalidated && change->scale_was_supplied) {
        /*
         * "Run calibrate again" is the wrong advice for a factor that was never
         * measured here: it came from a bench run at a different gain, and a
         * scale factor is counts per unit at one gain only.
         */
        STRRES_PRINTF(STR_I2C_NAU7802_TARE_SCALE_DROPPED_SUPPLIED);
    } else if (change->scale_invalidated) {
        STRRES_PRINTF(STR_I2C_NAU7802_TARE_SCALE_DROPPED);
    }

    if (err != ESP_OK) {
        switch (change->failed_stage) {
        case NAU7802_STAGE_CALIBRATE:
            report_calibration_error(err);
            break;
        case NAU7802_STAGE_START:
            STRRES_ERROR(STR_I2C_NAU7802_CONVERSIONS_RESTART_FAILED, esp_err_to_name(err));
            break;
        case NAU7802_STAGE_SETTLE:
            if (!report_drdy_timeout(err)) {
                STRRES_ERROR(STR_I2C_NAU7802_SETTLE_FAILED, esp_err_to_name(err));
            }
            break;
        default:
            /* The register write itself failed, so nothing was applied. The id
             * is a variable here, so this is the one call in the file the
             * compiler cannot check; every STR_..._SET_* it is given takes
             * exactly one %s. */
            strres_error(what, esp_err_to_name(err));
            break;
        }
        return -1;
    }

    STRRES_PRINTF(STR_I2C_NAU7802_SETTLING, change->discards);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Argument parsing                                                   */
/* ------------------------------------------------------------------ */

static int take_sample_count(int argc, char **argv, int index, int *samples)
{
    *samples = DEFAULT_SAMPLES;

    if (argc <= index) {
        return 0;
    }
    if (cli_parse_int_arg(argv[index], samples) < 0 ||
        *samples < 1 || *samples > MAX_SAMPLES) {
        STRRES_ERROR(STR_I2C_NAU7802_SAMPLE_COUNT_RANGE, MAX_SAMPLES);
        return -1;
    }
    return 0;
}

/* Map a gain like "128" onto an encoding. Prints the legal set itself. */
static int parse_gain_arg(const char *token, nau7802_gain_t *gain)
{
    int requested = 0;
    if (cli_parse_int_arg(token, &requested) < 0) {
        STRRES_ERROR(STR_I2C_NAU7802_GAIN_NOT_A_NUMBER);
        return -1;
    }

    if (nau7802_gain_from_value(requested, gain) == ESP_OK) {
        return 0;
    }

    STRRES_PRINTF(STR_I2C_NAU7802_GAIN_CHOICES);
    for (size_t i = 0; i < sizeof(nau7802_gains) / sizeof(nau7802_gains[0]); i++) {
        diag_printf(" %d", nau7802_gain_value(nau7802_gains[i]));
    }
    diag_printf("\n");
    return -1;
}

/*
 * Read a scale factor a user typed or pasted back in.
 *
 * Zero is refused because it divides every subsequent weight to infinity, and a
 * negative factor is *not*: a cell wired the other way round genuinely
 * calibrates to one, and the driver handles the sign throughout.
 * cli_parse_double_arg() has already rejected trailing garbage and non-finite
 * values, so this only has to speak to the one case it allows through.
 */
static int parse_scale_arg(const char *token, double *counts_per_unit)
{
    if (cli_parse_double_arg(token, counts_per_unit) < 0) {
        STRRES_ERROR(STR_I2C_NAU7802_SCALE_NOT_A_NUMBER);
        return -1;
    }
    if (*counts_per_unit == 0.0) {
        STRRES_ERROR(STR_I2C_NAU7802_SCALE_ZERO);
        return -1;
    }
    return 0;
}

/* Map a voltage like "3.0" onto an encoding. Prints the legal set itself. */
static int parse_ldo_arg(const char *token, nau7802_ldo_t *ldo)
{
    double volts = 0.0;
    if (cli_parse_double_arg(token, &volts) < 0) {
        STRRES_ERROR(STR_I2C_NAU7802_LDO_NOT_A_NUMBER);
        return -1;
    }

    const int millivolts = (int)(volts * 1000.0 + 0.5);
    if (nau7802_ldo_from_millivolts(millivolts, ldo) == ESP_OK) {
        return 0;
    }

    STRRES_PRINTF(STR_I2C_NAU7802_LDO_CHOICES);
    for (size_t i = 0; i < sizeof(nau7802_ldos) / sizeof(nau7802_ldos[0]); i++) {
        diag_printf(" %.1f", nau7802_ldo_millivolts(nau7802_ldos[i]) / 1000.0);
    }
    diag_printf("\n");
    return -1;
}

static void print_init_usage(void)
{
    STRRES_PRINTF(STR_I2C_NAU7802_USAGE_INIT);
}

/* ------------------------------------------------------------------ */
/* init                                                               */
/* ------------------------------------------------------------------ */

static void report_bringup_failure(const nau7802_bringup_report_t *report,
                                   esp_err_t err)
{
    switch (report->failed_stage) {
    case NAU7802_STAGE_PROBE:
        STRRES_ERROR(STR_I2C_NAU7802_NO_RESPONSE,
                     NAU7802_I2C_ADDRESS, esp_err_to_name(err));
        break;
    case NAU7802_STAGE_RESET:
        STRRES_ERROR(STR_I2C_NAU7802_RESET_FAILED);
        break;
    case NAU7802_STAGE_POWER_DIGITAL:
        STRRES_ERROR(STR_I2C_NAU7802_DIGITAL_POWER_FAILED);
        break;
    case NAU7802_STAGE_POWER_READY:
        STRRES_ERROR(STR_I2C_NAU7802_POWER_UP_TIMEOUT, esp_err_to_name(err));
        break;
    case NAU7802_STAGE_SET_LDO:
        STRRES_ERROR(STR_I2C_NAU7802_LDO_SET_FAILED);
        break;
    case NAU7802_STAGE_SET_GAIN:
        STRRES_ERROR(STR_I2C_NAU7802_GAIN_SET_FAILED);
        break;
    case NAU7802_STAGE_POWER_ANALOG:
        STRRES_ERROR(STR_I2C_NAU7802_ANALOG_POWER_FAILED);
        break;
    case NAU7802_STAGE_ADC_CTRL:
        STRRES_ERROR(STR_I2C_NAU7802_REG15_WRITE_FAILED);
        break;
    case NAU7802_STAGE_CALIBRATE:
        report_calibration_error(err);
        break;
    case NAU7802_STAGE_START:
        STRRES_ERROR(STR_I2C_NAU7802_CONVERSIONS_START_FAILED);
        break;
    case NAU7802_STAGE_SETTLE:
        if (!report_drdy_timeout(err)) {
            STRRES_ERROR(STR_I2C_NAU7802_SETTLE_FAILED, esp_err_to_name(err));
        }
        break;
    default:
        STRRES_ERROR(STR_I2C_NAU7802_INIT_FAILED, esp_err_to_name(err));
        break;
    }
}

static void report_bringup_success(const nau7802_bringup_report_t *report)
{
    STRRES_PRINTF(STR_I2C_NAU7802_READY, NAU7802_I2C_ADDRESS, report->revision);

    if (report->drdy_gpio >= 0) {
        STRRES_PRINTF(STR_I2C_NAU7802_DRDY_CONFIGURED, report->drdy_gpio);
    } else {
        STRRES_PRINTF(STR_I2C_NAU7802_DRDY_ABSENT);
    }

    if (report->ldo_enabled) {
        STRRES_PRINTF(STR_I2C_NAU7802_AVDD_LDO, nau7802_ldo_millivolts(report->ldo) / 1000.0);
    } else {
        STRRES_PRINTF(STR_I2C_NAU7802_AVDD_PIN);
    }

    /*
     * The gain is read back by the driver rather than assumed, because `init`
     * opens with a register reset which returns GAINS to the chip default of
     * x1 -- silently undoing an earlier `gain 128`. That is not a visible
     * failure: the converter still works, the offset calibration still passes,
     * and readings still look plausible. They are just 128 times smaller, which
     * puts a load cell's few millivolts down among the noise and makes
     * `calibrate` refuse for what looks like no reason.
     */
    const int gain = nau7802_gain_value(report->gain);
    STRRES_PRINTF(STR_I2C_NAU7802_GAIN_REPORT, gain);
    if (!report->gain_was_requested && gain < 128) {
        STRRES_PRINTF(STR_I2C_NAU7802_GAIN_RESET_DEFAULT);
    }

    /* Read back like the gain above, but only the failure is worth a line:
     * the write is unconditional and takes no argument, so a successful one
     * tells the user nothing they could act on. What it buys and why every
     * other REG_CHPS encoding is Reserved is in docs/i2c.md. */
    if (report->adc_ctrl_valid && !report->chopper_off) {
        STRRES_ERROR(STR_I2C_NAU7802_REG15_MISMATCH, report->adc_ctrl);
    }

    /*
     * A supplied factor changes what is left to do: the span is already known,
     * and only the zero is outstanding. Telling someone to calibrate against a
     * known mass when they have just handed over a bench-measured factor is
     * advice for the wrong task.
     */
    const nau7802_scale_t *scale = nau7802_get_scale(nau);
    if (scale->supplied) {
        STRRES_PRINTF(STR_I2C_NAU7802_OFFSET_CAL_OK_SCALE, scale->counts_per_unit);
        return;
    }

    STRRES_PRINTF(STR_I2C_NAU7802_OFFSET_CAL_OK);
}

int cmd_nau7802_init(int argc, char **argv)
{
    if (!require_device()) {
        return -1;
    }

    /*
     * AVDD source. The chip default is the external AVDD pin, and that is kept
     * here: switching the internal regulator on while a board already drives
     * AVDD would put two sources on one net. Boards that rely on the internal
     * LDO (many load cell breakouts do) need 'i2c-nau7802 init ldo <volts>'.
     */
    nau7802_bringup_opts_t opts = {0};
    int requested_drdy = -1;

    /* All three options describe how the board is wired, so any order is fine
     * and none is required. */
    for (int i = 1; i < argc; i++) {
        if (strcasecmp(argv[i], "ldo") == 0) {
            if (++i >= argc) {
                STRRES_ERROR(STR_I2C_NAU7802_INIT_LDO_MISSING);
                return -1;
            }
            if (parse_ldo_arg(argv[i], &opts.ldo) < 0) {
                return -1;
            }
            opts.use_internal_ldo = true;
        } else if (strcasecmp(argv[i], "drdy") == 0) {
            if (++i >= argc) {
                STRRES_ERROR(STR_I2C_NAU7802_INIT_DRDY_MISSING);
                return -1;
            }
            if (cli_parse_int_arg(argv[i], &requested_drdy) < 0) {
                STRRES_ERROR(STR_I2C_NAU7802_DRDY_NOT_A_NUMBER);
                return -1;
            }
        } else if (strcasecmp(argv[i], "gain") == 0) {
            if (++i >= argc) {
                STRRES_ERROR(STR_I2C_NAU7802_INIT_GAIN_MISSING);
                return -1;
            }
            if (parse_gain_arg(argv[i], &opts.gain) < 0) {
                return -1;
            }
            opts.set_gain = true;
        } else if (strcasecmp(argv[i], "scale") == 0) {
            if (++i >= argc) {
                STRRES_ERROR(STR_I2C_NAU7802_INIT_SCALE_MISSING);
                return -1;
            }
            if (parse_scale_arg(argv[i], &opts.counts_per_unit) < 0) {
                return -1;
            }
            opts.set_scale = true;
        } else {
            print_init_usage();
            return -1;
        }
    }

    /* Claim the pin before touching the device, so a bad pin number fails
     * without having half-configured the converter. */
    if (requested_drdy >= 0 && nau7802_set_drdy(nau, requested_drdy) != ESP_OK) {
        STRRES_ERROR(STR_I2C_NAU7802_DRDY_CONFIG_FAILED, requested_drdy);
        return -1;
    }

    nau7802_bringup_report_t report = {0};
    esp_err_t err = nau7802_bring_up(nau, &opts, &report);
    if (err != ESP_OK) {
        report_bringup_failure(&report, err);
        return -1;
    }

    report_bringup_success(&report);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Configuration                                                      */
/* ------------------------------------------------------------------ */

int cmd_nau7802_gain(int argc, char **argv)
{
    if (!require_init()) {
        return -1;
    }

    if (argc < 2) {
        nau7802_gain_t gain;
        if (nau7802_get_gain(nau, &gain) != ESP_OK) {
            STRRES_ERROR(STR_I2C_NAU7802_CTRL1_READ_FAILED);
            return -1;
        }
        STRRES_PRINTF(STR_I2C_NAU7802_GAIN_IS, nau7802_gain_value(gain));
        return 0;
    }

    nau7802_gain_t gain;
    if (parse_gain_arg(argv[1], &gain) < 0) {
        return -1;
    }

    /* Changing the analog path invalidates the offset calibration. */
    STRRES_PRINTF(STR_I2C_NAU7802_GAIN_SET, nau7802_gain_value(gain));

    nau7802_change_report_t change = {0};
    esp_err_t err = nau7802_set_gain(nau, gain, &change);
    return report_change(err, &change, STR_I2C_NAU7802_SET_GAIN_FAILED);
}

int cmd_nau7802_rate(int argc, char **argv)
{
    if (!require_init()) {
        return -1;
    }

    if (argc < 2) {
        nau7802_rate_t rate;
        if (nau7802_get_rate(nau, &rate) != ESP_OK) {
            STRRES_ERROR(STR_I2C_NAU7802_CTRL2_READ_FAILED);
            return -1;
        }
        const int sps = nau7802_rate_sps(rate);
        if (sps < 0) {
            STRRES_PRINTF(STR_I2C_NAU7802_RATE_UNDEFINED);
        } else {
            STRRES_PRINTF(STR_I2C_NAU7802_RATE_IS, sps);
        }
        return 0;
    }

    int requested = 0;
    if (cli_parse_int_arg(argv[1], &requested) < 0) {
        STRRES_ERROR(STR_I2C_NAU7802_RATE_NOT_A_NUMBER);
        return -1;
    }

    nau7802_rate_t rate;
    if (nau7802_rate_from_sps(requested, &rate) != ESP_OK) {
        STRRES_PRINTF(STR_I2C_NAU7802_RATE_CHOICES);
        return -1;
    }

    /* The conversion rate changes the modulator and filter settings, so the
     * existing offset calibration no longer applies. Skipping this leaves the
     * ADC returning values that swing across most of the full-scale range. */
    STRRES_PRINTF(STR_I2C_NAU7802_RATE_SET, requested);

    /* Measured on the board this was developed against: 10-80 SPS are rock
     * steady, while 320 SPS returns values swinging across the whole range.
     * Likely an oscillator limitation -- 320 SPS may need an external crystal
     * (PU_CTRL.OSCS) rather than the internal RC. */
    if (requested == 320) {
        STRRES_PRINTF(STR_I2C_NAU7802_RATE_320_WARNING);
    }

    nau7802_change_report_t change = {0};
    esp_err_t err = nau7802_set_rate(nau, rate, &change);
    return report_change(err, &change, STR_I2C_NAU7802_SET_RATE_FAILED);
}

int cmd_nau7802_input(int argc, char **argv)
{
    if (!require_init()) {
        return -1;
    }

    if (argc < 2) {
        nau7802_channel_t channel;
        if (nau7802_get_channel(nau, &channel) != ESP_OK) {
            STRRES_ERROR(STR_I2C_NAU7802_CTRL2_READ_FAILED);
            return -1;
        }
        STRRES_PRINTF(STR_I2C_NAU7802_INPUT_CHANNEL, channel == NAU7802_CHANNEL_B ? "B" : "A");
        return 0;
    }

    nau7802_channel_t channel;
    if (strcasecmp(argv[1], "a") == 0) {
        channel = NAU7802_CHANNEL_A;
    } else if (strcasecmp(argv[1], "b") == 0) {
        channel = NAU7802_CHANNEL_B;
    } else {
        STRRES_ERROR(STR_I2C_NAU7802_INPUT_INVALID);
        return -1;
    }

    const char *name = (channel == NAU7802_CHANNEL_B) ? "B" : "A";
    STRRES_PRINTF(STR_I2C_NAU7802_INPUT_CHANNEL_SET, name);

    nau7802_change_report_t change = {0};
    esp_err_t err = nau7802_set_channel(nau, channel, &change);
    return report_change(err, &change, STR_I2C_NAU7802_SET_CHANNEL_FAILED);
}

/*
 * Deliberately does not require `init`, or even a bus: this is a statement
 * about how the board is wired, and it is useful to be able to declare or
 * revoke it without disturbing a converter that is already running.
 */
int cmd_nau7802_drdy(int argc, char **argv)
{
    if (!ensure_handle()) {
        return -1;
    }

    const int pin = nau7802_drdy_gpio(nau);

    if (argc < 2) {
        if (pin < 0) {
            STRRES_PRINTF(STR_I2C_NAU7802_DRDY_POLLING);
        } else if (nau7802_drdy_level(nau)) {
            STRRES_PRINTF(STR_I2C_NAU7802_DRDY_STATE_HIGH, pin);
        } else {
            STRRES_PRINTF(STR_I2C_NAU7802_DRDY_STATE_LOW, pin);
        }
        return 0;
    }

    if (strcasecmp(argv[1], "off") == 0) {
        nau7802_set_drdy(nau, -1);
        STRRES_PRINTF(STR_I2C_NAU7802_DRDY_RELEASED);
        return 0;
    }

    int requested = 0;
    if (cli_parse_int_arg(argv[1], &requested) < 0) {
        STRRES_PRINTF(STR_I2C_NAU7802_USAGE_DRDY);
        return -1;
    }

    esp_err_t err = nau7802_set_drdy(nau, requested);
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_ARG) {
            STRRES_ERROR(STR_I2C_NAU7802_GPIO_ABSENT, requested);
        }
        STRRES_ERROR(STR_I2C_NAU7802_DRDY_CONFIG_FAILED, requested);
        return -1;
    }

    STRRES_PRINTF(STR_I2C_NAU7802_DRDY_SET, requested);

    /*
     * A pin that is low here is the normal case -- the last result was read --
     * so silence is not evidence either way. Saying nothing at all about it
     * would leave a typo to surface later as a timeout mid-measurement.
     */
    STRRES_PRINTF(STR_I2C_NAU7802_DRDY_CONFIRM);
    return 0;
}

int cmd_nau7802_ldomode(int argc, char **argv)
{
    if (!require_init()) {
        return -1;
    }

    if (argc < 2) {
        bool stable = false;
        if (nau7802_get_ldomode(nau, &stable) != ESP_OK) {
            STRRES_ERROR(STR_I2C_NAU7802_PGA_READ_FAILED);
            return -1;
        }
        if (stable) {
            STRRES_PRINTF(STR_I2C_NAU7802_LDOMODE_IS_TOLERANT);
        } else {
            STRRES_PRINTF(STR_I2C_NAU7802_LDOMODE_IS_STRICT);
        }
        return 0;
    }

    int mode = 0;
    if (cli_parse_int_arg(argv[1], &mode) < 0 || (mode != 0 && mode != 1)) {
        STRRES_PRINTF(STR_I2C_NAU7802_USAGE_LDOMODE);
        STRRES_PRINTF(STR_I2C_NAU7802_LDOMODE_HELP_0);
        STRRES_PRINTF(STR_I2C_NAU7802_LDOMODE_HELP_1);
        return -1;
    }

    /* The two modes settle AVDD at slightly different levels, and AVDD is the
     * reference, so the existing offset calibration no longer applies. */
    STRRES_PRINTF(STR_I2C_NAU7802_LDOMODE_SET, mode);

    nau7802_change_report_t change = {0};
    esp_err_t err = nau7802_set_ldomode(nau, mode != 0, &change);
    return report_change(err, &change, STR_I2C_NAU7802_SET_LDOMODE_FAILED);
}

int cmd_nau7802_pgacap(int argc, char **argv)
{
    if (!require_init()) {
        return -1;
    }

    if (argc < 2) {
        bool enabled = false;
        if (nau7802_get_pga_cap(nau, &enabled) != ESP_OK) {
            STRRES_ERROR(STR_I2C_NAU7802_POWER_READ_FAILED);
            return -1;
        }
        STRRES_PRINTF(STR_I2C_NAU7802_PGACAP_IS, enabled ? "enabled" : "disabled");
        return 0;
    }

    bool enable;
    if (strcasecmp(argv[1], "on") == 0) {
        enable = true;
    } else if (strcasecmp(argv[1], "off") == 0) {
        enable = false;
    } else {
        STRRES_PRINTF(STR_I2C_NAU7802_USAGE_PGACAP);
        return -1;
    }

    if (enable) {
        /*
         * Worth saying every time rather than once in the docs: with no
         * capacitor fitted this quietly does nothing, and it takes channel 2
         * away whether or not it helps.
         */
        STRRES_PRINTF(STR_I2C_NAU7802_PGACAP_ENABLED);
    } else {
        STRRES_PRINTF(STR_I2C_NAU7802_PGACAP_DISABLED);
    }

    STRRES_PRINTF(STR_I2C_NAU7802_RECALIBRATING);

    nau7802_change_report_t change = {0};
    esp_err_t err = nau7802_set_pga_cap(nau, enable, &change);
    return report_change(err, &change,
                         STR_I2C_NAU7802_SET_PGACAP_FAILED);
}

/* ------------------------------------------------------------------ */
/* Reading                                                            */
/* ------------------------------------------------------------------ */

int cmd_nau7802_raw(int argc, char **argv)
{
    if (!require_init()) {
        return -1;
    }

    int arg_idx = 1;
    int samples = 1;
    if (arg_idx < argc) {
        if (cli_parse_int_arg(argv[arg_idx], &samples) < 0) {
            STRRES_ERROR(STR_I2C_NAU7802_SAMPLE_COUNT_INVALID, argv[arg_idx]);
            return -1;
        }
        arg_idx++;
    }

    const char *channel = "a";
    if (arg_idx < argc) {
        if (strcasecmp(argv[arg_idx], "a") != 0 &&
            strcasecmp(argv[arg_idx], "b") != 0) {
            STRRES_ERROR(STR_I2C_NAU7802_CHANNEL_INVALID, argv[arg_idx]);
            return -1;
        }
        channel = argv[arg_idx];
    }

    /*
     * Only switch if the device is not already on the requested channel.
     *
     * This used to rewrite CHS and recalibrate on every invocation, which made
     * `raw` cost a calibration plus its settling discards even when nothing
     * changed -- and, because it recalibrated, silently threw away the tare and
     * scale that `raw` has no business touching. Reading the current channel
     * back is a transaction; it beats assuming, and it beats a static that can
     * drift away from the register it claims to mirror.
     */
    const nau7802_channel_t want =
        (strcasecmp(channel, "b") == 0) ? NAU7802_CHANNEL_B : NAU7802_CHANNEL_A;

    nau7802_channel_t current;
    if (nau7802_get_channel(nau, &current) != ESP_OK) {
        STRRES_ERROR(STR_I2C_NAU7802_CTRL2_READ_FAILED);
        return -1;
    }

    if (want != current) {
        STRRES_PRINTF(STR_I2C_NAU7802_INPUT_CHANNEL_SET, want == NAU7802_CHANNEL_B ? "B" : "A");

        nau7802_change_report_t change = {0};
        esp_err_t err = nau7802_set_channel(nau, want, &change);
        if (report_change(err, &change, STR_I2C_NAU7802_SET_CHANNEL_FAILED) < 0) {
            return -1;
        }
    }

    STRRES_PRINTF(STR_I2C_NAU7802_RAW_HEADER, channel);

    for (int i = 0; i < samples; i++) {
        int32_t value = 0;
        esp_err_t err = nau7802_read_raw(nau, &value);
        if (err != ESP_OK) {
            if (!report_drdy_timeout(err)) {
                STRRES_ERROR(STR_I2C_NAU7802_SAMPLE_FAILED, i + 1, esp_err_to_name(err));
            }
            return -1;
        }

        const double percent = value * 100.0 / NAU7802_FULL_SCALE;
        STRRES_PRINTF(STR_I2C_NAU7802_RAW_ROW, (long)value, percent);
    }

    return 0;
}

int cmd_nau7802_read(int argc, char **argv)
{
    if (!require_init()) {
        return -1;
    }

    int samples = 0;
    if (take_sample_count(argc, argv, 1, &samples) < 0) {
        return -1;
    }

    nau7802_stats_t stats = {0};
    if (take_batch(samples, &stats) < 0) {
        return -1;
    }

    /* Percent of full scale is wiring-independent; an absolute voltage would
     * depend on REFP-REFN, which this driver has no way to know. */
    STRRES_PRINTF(STR_I2C_NAU7802_READ_RAW,
                  stats.mean, samples, (long)(stats.max - stats.min),
                  stats.mean * 100.0 / NAU7802_FULL_SCALE);

    const nau7802_scale_t *scale = nau7802_get_scale(nau);

    if (scale->tare_samples > 0) {
        STRRES_PRINTF(STR_I2C_NAU7802_READ_NET, stats.mean - scale->tare_counts);
    }

    /*
     * Once a scale factor exists, counts stop being the answer anyone wants
     * from a load cell -- but `read` is the command that shows the raw number,
     * so the converted figure goes alongside it rather than replacing it.
     *
     * The +/- is the one `weight` quotes, for the same reason: it belongs to
     * the mean actually printed, so it is this batch's standard error combined
     * in quadrature with the tare's, not the peak-to-peak spread.
     */
    if (scale->calibrated) {
        const double net = stats.mean - scale->tare_counts;
        if (samples < 2) {
            /* One conversion has no spread, so quote no error bar rather than
             * a zero one. */
            STRRES_PRINTF(STR_I2C_NAU7802_READ_UNITS, net / scale->counts_per_unit);
        } else {
            STRRES_PRINTF(STR_I2C_NAU7802_READ_UNITS_PM,
                          net / scale->counts_per_unit,
                          hypot(stats.stderr_mean, scale->tare_stderr) / fabs(scale->counts_per_unit));
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Scale                                                              */
/* ------------------------------------------------------------------ */

/*
 * Print the factor a second time, in a form that can be pasted into a
 * consumer's source or back into 'scale'.
 *
 * %.17g rather than the %.1f the human line uses. Seventeen significant digits
 * is the width at which double -> text -> double is exact for every IEEE-754
 * double, and exactness is the whole promise here: at %.1f, pasting the number
 * back and asking for it again prints something different, and nothing tells a
 * user whether that is a rounding artefact or a driver bug.
 *
 * In practice the output is usually much shorter than seventeen digits. This
 * build's picolibc emits the shortest string that still round-trips, so a
 * factor of 214.7 prints as "214.7" rather than "214.69999999999999" -- fewer
 * digits, same double. Measured on the sensor board, not assumed: the ELF links
 * __d_vfprintf and __dtoa_engine, the double-capable printf, so the precision
 * is really there when a value needs it.
 *
 * The gain rides along in the comment because a factor is counts per unit at
 * one gain and is meaningless without it, and the precision because this is the
 * only moment the firmware knows it -- nothing downstream of 'i2c-nau7802 calibrate' can
 * recover how good the number was.
 */
static void print_scale_for_consumer(double counts_per_unit, nau7802_gain_t gain,
                                     bool gain_valid, double precision_percent,
                                     bool precision_valid)
{
    STRRES_PRINTF(STR_I2C_NAU7802_SCALE_FACTOR_COMMENT, counts_per_unit);
    if (precision_valid) {
        diag_printf(" +/-%.2f%%,", precision_percent);
    }
    if (gain_valid) {
        diag_printf(" gain x%d,", nau7802_gain_value(gain));
    }
    STRRES_PRINTF(STR_I2C_NAU7802_SCALE_FACTOR_COMMENT_END);
}

/*
 * Report a scale factor and where it came from.
 *
 * Two whole sentences rather than one with a "%s" for the provenance: the
 * clause is prose, and prose passed as an argument stays in the image while the
 * sentence around it does not.
 */
static void print_scale_provenance(double counts_per_unit, bool supplied)
{
    if (supplied) {
        STRRES_PRINTF(STR_I2C_NAU7802_SCALE_IS_SUPPLIED, counts_per_unit);
    } else {
        STRRES_PRINTF(STR_I2C_NAU7802_SCALE_IS_MEASURED, counts_per_unit);
    }
}

int cmd_nau7802_tare(int argc, char **argv)
{
    if (!require_init()) {
        return -1;
    }

    int samples = 0;
    if (take_sample_count(argc, argv, 1, &samples) < 0) {
        return -1;
    }

    nau7802_stats_t stats = {0};
    esp_err_t err = nau7802_tare(nau, samples, &stats);
    if (err != ESP_OK) {
        report_batch_error(err, &stats, samples);
        return -1;
    }
    warn_if_saturated(&stats);

    STRRES_PRINTF(STR_I2C_NAU7802_TARE_SET,
                  (long)stats.mean, samples, (long)(stats.max - stats.min), stats.stderr_mean);
    return 0;
}

int cmd_nau7802_calibrate(int argc, char **argv)
{
    if (!require_init()) {
        return -1;
    }

    if (argc < 2) {
        STRRES_PRINTF(STR_I2C_NAU7802_USAGE_CALIBRATE);
        STRRES_PRINTF(STR_I2C_NAU7802_CALIBRATE_HOW);
        return -1;
    }

    double known = 0.0;
    if (cli_parse_double_arg(argv[1], &known) < 0 || known == 0.0) {
        STRRES_ERROR(STR_I2C_NAU7802_MASS_INVALID);
        return -1;
    }

    int samples = 0;
    if (take_sample_count(argc, argv, 2, &samples) < 0) {
        return -1;
    }

    nau7802_stats_t stats = {0};
    nau7802_calibration_t result = {0};
    esp_err_t err = nau7802_calibrate(nau, known, samples, &stats, &result);

    if (err != ESP_OK && err != ESP_ERR_NAU7802_WITHIN_NOISE &&
        err != ESP_ERR_NAU7802_TOO_FEW_SAMPLES) {
        report_batch_error(err, &stats, samples);
        return -1;
    }
    warn_if_saturated(&stats);

    /*
     * One conversion yields no spread, so neither batch can report an honest
     * uncertainty and the guard would be comparing against zero -- which
     * accepts anything. Refuse rather than calibrate on a number that cannot be
     * checked.
     */
    if (err == ESP_ERR_NAU7802_TOO_FEW_SAMPLES) {
        STRRES_ERROR(STR_I2C_NAU7802_NEED_TWO_SAMPLES, known);
        return -1;
    }

    if (err == ESP_ERR_NAU7802_WITHIN_NOISE) {
        STRRES_ERROR(STR_I2C_NAU7802_TARE_DRIFT, result.net_counts, result.uncertainty, known);

        /*
         * Averaging is the wrong advice when the signal is small because the
         * PGA is turned down -- no amount of it recovers a factor of 128. Say
         * so, because a gain of x1 is what `init` leaves behind and nothing
         * about the numbers points at it.
         */
        if (result.gain_valid) {
            const int gain = nau7802_gain_value(result.gain);
            if (gain > 0 && gain < 128) {
                STRRES_PRINTF(STR_I2C_NAU7802_GAIN_HEADROOM, gain, 128 / gain);
            }
        }
        return -1;
    }

    /*
     * Report the precision rather than only the number. The scale factor is a
     * ratio of a measured difference to a stated mass, so its relative error is
     * the relative error of that difference -- and every weight from here
     * inherits it.
     */
    STRRES_PRINTF(STR_I2C_NAU7802_CALIBRATED, result.counts_per_unit, result.net_counts, known);
    STRRES_PRINTF(STR_I2C_NAU7802_CALIBRATE_PRECISION,
                  result.precision_percent, result.uncertainty);
    if (result.precision_percent > 1.0) {
        STRRES_PRINTF(STR_I2C_NAU7802_CALIBRATE_MORE_SAMPLES,
                      result.precision_percent / sqrt(100.0 / samples), known);
    }
    print_scale_for_consumer(result.counts_per_unit, result.gain, result.gain_valid,
                             result.precision_percent, true);
    return 0;
}

/*
 * Show or set the scale factor without measuring one.
 *
 * The setting half is what closes the factory-calibration loop: calibrate once
 * on the bench, keep the number, and hand it back to every board afterwards
 * instead of putting a known mass on each. It sets only the factor -- the tare
 * is the bridge's own zero and still has to be measured here.
 */
int cmd_nau7802_scale(int argc, char **argv)
{
    if (!require_init()) {
        return -1;
    }

    const nau7802_scale_t *scale = nau7802_get_scale(nau);

    if (argc < 2) {
        if (!scale->calibrated) {
            STRRES_PRINTF(STR_I2C_NAU7802_NO_SCALE);
            return 0;
        }
        print_scale_provenance(scale->counts_per_unit, scale->supplied);
        /* The gain is half of what a factor means, so the pasteable line is
         * worth little without it. Read it back rather than assuming. */
        nau7802_gain_t gain = NAU7802_GAIN_1;
        const bool gain_valid = (nau7802_get_gain(nau, &gain) == ESP_OK);
        print_scale_for_consumer(scale->counts_per_unit, gain, gain_valid,
                                 0.0, false);
        return 0;
    }

    double counts_per_unit = 0.0;
    if (parse_scale_arg(argv[1], &counts_per_unit) < 0) {
        return -1;
    }

    esp_err_t err = nau7802_set_scale(nau, counts_per_unit);
    if (err != ESP_OK) {
        STRRES_ERROR(STR_I2C_NAU7802_SCALE_SET_FAILED, esp_err_to_name(err));
        return -1;
    }

    STRRES_PRINTF(STR_I2C_NAU7802_SCALE_SET, counts_per_unit);

    /*
     * The factor says nothing about where zero is, and the two are separate
     * measurements. Saying so here is cheaper than letting 'i2c-nau7802 weight' refuse and
     * be the first to mention it.
     */
    if (scale->tare_samples == 0) {
        STRRES_PRINTF(STR_I2C_NAU7802_NO_TARE);
    }

    /*
     * A factor is counts per unit at one gain, and every setter that perturbs
     * the analog path drops it -- including 'i2c-nau7802 init'. Worth saying at the moment
     * someone has just typed a number in by hand.
     */
    STRRES_PRINTF(STR_I2C_NAU7802_SCALE_SUPPLIED_NOTE, counts_per_unit);
    return 0;
}

int cmd_nau7802_weight(int argc, char **argv)
{
    if (!require_init()) {
        return -1;
    }

    const nau7802_scale_t *scale = nau7802_get_scale(nau);
    if (!scale->calibrated) {
        STRRES_ERROR(STR_I2C_NAU7802_NOT_CALIBRATED);
        return -1;
    }

    /*
     * Reachable only with a supplied factor: 'i2c-nau7802 calibrate' refuses without a tare
     * of at least two samples, so before 'i2c-nau7802 scale' existed a calibrated scale
     * implied a real zero. Without one the subtraction is against zero, and an
     * unloaded bridge sits tens of thousands of counts away from that -- which
     * would be reported as load, confidently. Refuse before spending the
     * conversions.
     */
    if (scale->tare_samples == 0) {
        STRRES_ERROR(STR_I2C_NAU7802_TARE_MISSING);
        return -1;
    }

    int samples = 0;
    if (take_sample_count(argc, argv, 1, &samples) < 0) {
        return -1;
    }

    nau7802_stats_t stats = {0};
    nau7802_weight_t weight = {0};
    esp_err_t err = nau7802_weigh(nau, samples, &stats, &weight);
    if (err != ESP_OK) {
        report_batch_error(err, &stats, samples);
        return -1;
    }
    warn_if_saturated(&stats);

    /*
     * Two different numbers, and reporting only one of them misleads.
     *
     * The +/- belongs to the figure actually printed, which is a mean of
     * `samples` conversions -- so it is the standard error of that mean,
     * combined with the tare's, not the peak-to-peak spread. Quoting the spread
     * overstates the error of the printed number by roughly sqrt(samples): on
     * the NAU7802's sensor board that is a factor of five at ten samples.
     *
     * The spread is still worth printing, because during bringup it answers a
     * different question -- what a single reading would look like, and whether
     * the front end is behaving -- so it goes alongside rather than instead.
     */
    if (samples < 2) {
        /* No spread from one conversion, so quote no error bar rather than a
         * zero one. */
        STRRES_PRINTF(STR_I2C_NAU7802_WEIGHT_ONE_SAMPLE, weight.units, weight.net_counts);
        return 0;
    }

    STRRES_PRINTF(STR_I2C_NAU7802_WEIGHT,
                  weight.units, weight.uncertainty_units, samples, weight.spread_units,
                  weight.net_counts);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Diagnostics                                                        */
/* ------------------------------------------------------------------ */

int cmd_nau7802_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!require_device()) {
        return -1;
    }

    nau7802_status_t status;
    if (nau7802_get_status(nau, &status) != ESP_OK) {
        STRRES_ERROR(STR_I2C_NAU7802_NO_RESPONSE_SHORT, NAU7802_I2C_ADDRESS);
        return -1;
    }

    STRRES_PRINTF(STR_I2C_NAU7802_STATUS_REVISION,
                  status.revision, NAU7802_I2C_ADDRESS);
    STRRES_PRINTF(STR_I2C_NAU7802_STATUS_PU_CTRL,
                  status.pu_ctrl, status.digital_up ? "up" : "down",
                  status.analog_up ? "up" : "down", status.power_ready ? "yes" : "no",
                  status.data_ready ? "ready" : "pending");
    STRRES_PRINTF(STR_I2C_NAU7802_STATUS_AVDD,
                  status.internal_ldo ? "internal LDO" : "AVDD pin");
    STRRES_PRINTF(STR_I2C_NAU7802_STATUS_CTRL1,
                  status.ctrl1, nau7802_gain_value(status.gain),
                  nau7802_ldo_millivolts(status.ldo) / 1000.0);
    STRRES_PRINTF(STR_I2C_NAU7802_STATUS_CTRL2,
                  status.ctrl2, status.rate_sps, status.cal_error ? "ERROR" : "ok");
    STRRES_PRINTF(STR_I2C_NAU7802_INPUT_CHANNEL,
                  status.channel == NAU7802_CHANNEL_B ? "B" : "A");

    /*
     * CAL_ERR is one bit, and it only says the device gave up. The offset it
     * settled on is the number that says whether the front end is anywhere near
     * balanced: an OCAL close to zero means the bridge sits near mid-supply,
     * one up against the rails means it does not and the gain has nowhere to
     * go. Note that OCAL is sign-magnitude; the driver decodes it.
     */
    if (status.cal_regs_valid) {
        STRRES_PRINTF(STR_I2C_NAU7802_STATUS_CHANNEL_CAL,
                      status.channel == NAU7802_CHANNEL_B ? 'B' : 'A',
                      (unsigned long)status.ocal_raw, (long)status.ocal_counts,
                      (unsigned long)status.gcal_raw, status.gcal_ratio);
    } else {
        STRRES_PRINTF(STR_I2C_NAU7802_STATUS_CAL_ERROR);
    }

    if (status.drdy_gpio >= 0) {
        STRRES_PRINTF(STR_I2C_NAU7802_STATUS_DRDY,
                      status.drdy_gpio, status.drdy_level ? "high" : "low");
    } else {
        STRRES_PRINTF(STR_I2C_NAU7802_STATUS_DRDY_POLLING);
    }

    if (status.pga_valid) {
        STRRES_PRINTF(STR_I2C_NAU7802_STATUS_PGA,
                      status.pga, status.ldomode ? 1 : 0, status.ldomode ? "5 ohms" : "1 ohm");
        STRRES_PRINTF(STR_I2C_NAU7802_STATUS_POWER,
                      status.power, status.pga_cap ? "enabled" : "disabled");
    }

    /*
     * REG0x15 is worth a decoded line rather than leaving it to `registers`,
     * because a wrong REG_CHPS costs about six bits and nothing else in this
     * output would show it. What it means is in docs/i2c.md; all this line has
     * to do is say when the value is not the one `init` writes.
     */
    if (status.adc_ctrl_valid) {
        if (status.chopper_off) {
            STRRES_PRINTF(STR_I2C_NAU7802_STATUS_ADC, status.adc_ctrl, status.chps);
        } else {
            STRRES_PRINTF(STR_I2C_NAU7802_STATUS_ADC_UNEXPECTED,
                          status.adc_ctrl, status.chps);
        }
    }

    if (status.brought_up) {
        STRRES_PRINTF(STR_I2C_NAU7802_STATUS_TARE,
                      (long)status.scale.tare_counts,
                      status.scale.calibrated ? "calibrated" : "not calibrated");
        if (status.scale.calibrated) {
            print_scale_provenance(status.scale.counts_per_unit,
                                   status.scale.supplied);
            print_scale_for_consumer(status.scale.counts_per_unit, status.gain,
                                     true, 0.0, false);
            /*
             * The +/- on a weight has always been this session's noise
             * propagated through a factor treated as exact. That is easy to
             * misread as accuracy, and a supplied factor is where it would
             * mislead most; docs/i2c.md carries the rest of the reasoning.
             */
            if (status.scale.supplied) {
                STRRES_PRINTF(STR_I2C_NAU7802_WEIGHT_PRECISION_NOTE);
            }
            if (status.scale.tare_samples == 0) {
                STRRES_PRINTF(STR_I2C_NAU7802_NO_TARE);
            }
        }
    } else {
        STRRES_PRINTF(STR_I2C_NAU7802_NOT_THIS_SESSION);
    }

    return 0;
}

int cmd_nau7802_registers(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!require_device()) {
        return -1;
    }

    STRRES_PRINTF(STR_I2C_NAU7802_DUMP_HEADER, NAU7802_I2C_ADDRESS);

    size_t count = 0;
    const nau7802_register_info_t *map = nau7802_register_map(&count);

    for (size_t i = 0; i < count; i++) {
        uint8_t value = 0;
        if (nau7802_read_register(nau, map[i].reg, &value) == ESP_OK) {
            STRRES_PRINTF(STR_I2C_NAU7802_DUMP_ROW, map[i].reg, map[i].name, value);
        } else {
            STRRES_PRINTF(STR_I2C_NAU7802_DUMP_ROW_ERROR, map[i].reg, map[i].name);
        }
    }

    return 0;
}
