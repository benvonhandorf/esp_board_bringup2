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
        diag_error("Allocating the NAU7802 driver: %s", esp_err_to_name(err));
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
        diag_error("Addressing device 0x%02X: %s", NAU7802_I2C_ADDRESS,
                 esp_err_to_name(err));
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
        diag_error("NAU7802 not initialized. Run 'nau7802 init' first.");
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
static void report_read_error(esp_err_t err, const char *context)
{
    const int pin = nau7802_drdy_gpio(nau);

    if (err == ESP_ERR_TIMEOUT && pin >= 0) {
        diag_error("DRDY (GPIO %d) never went high. Check that pin is really "
                 "wired to the device's DRDY output, or run 'drdy off' to "
                 "go back to polling the CR status bit over I2C.", pin);
        return;
    }
    diag_error("%s: %s", context, esp_err_to_name(err));
}

static void report_calibration_error(esp_err_t err)
{
    if (err == ESP_ERR_NAU7802_CAL_FAILED) {
        diag_error("Internal calibration reported an error (CTRL2.CAL_ERR). "
                 "Check the bridge wiring and the reference voltage.");
    } else if (err == ESP_ERR_TIMEOUT) {
        diag_error("Internal calibration did not finish: %s", esp_err_to_name(err));
    } else {
        diag_error("Starting internal calibration: %s", esp_err_to_name(err));
    }
}

/* Report a failed batch read, naming how far it got. */
static void report_batch_error(esp_err_t err, const nau7802_stats_t *stats, int wanted)
{
    char context[64];
    snprintf(context, sizeof(context), "Reading conversion %d of %d",
             stats->samples + 1, wanted);
    report_read_error(err, context);
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
        diag_error("ADC is saturated at %.2f%% of full scale. Check that the "
                 "bridge is connected and excited, and that the gain is not "
                 "too high.", stats->saturation_percent);
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
                         const char *what)
{
    if (change->scale_invalidated && change->scale_was_supplied) {
        /*
         * "Run calibrate again" is the wrong advice for a factor that was never
         * measured here: it came from a bench run at a different gain, and a
         * scale factor is counts per unit at one gain only.
         */
        diag_printf("The scale factor set by 'scale' was measured at the previous "
                  "gain, where it is wrong here by exactly the gain ratio, so "
                  "it has been dropped along with the tare. Set a factor "
                  "measured at this gain, or give both at once with "
                  "'init ... gain <n> scale <counts_per_unit>'.\n");
    } else if (change->scale_invalidated) {
        diag_printf("The tare and scale were captured under the previous "
                  "configuration and no longer apply; run 'tare' and "
                  "'calibrate' again.\n");
    }

    if (err != ESP_OK) {
        switch (change->failed_stage) {
        case NAU7802_STAGE_CALIBRATE:
            report_calibration_error(err);
            break;
        case NAU7802_STAGE_START:
            diag_error("Restarting conversions failed: %s", esp_err_to_name(err));
            break;
        case NAU7802_STAGE_SETTLE:
            report_read_error(err, "Discarding the settling conversions");
            break;
        default:
            /* The register write itself failed, so nothing was applied. */
            diag_error("%s: %s", what, esp_err_to_name(err));
            break;
        }
        return -1;
    }

    if (change->conversions_restarted) {
        diag_printf("Calibration left the conversion cycle stopped "
                  "(PU_CTRL.CS clear); restarting it\n");
    }

    diag_printf("Settling: discarding %d conversions (one holding the previous "
              "configuration's result, then %d output periods of filter "
              "history)\n", change->discards, change->settling_conversions);
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
        diag_error("Sample count must be 1-%d", MAX_SAMPLES);
        return -1;
    }
    return 0;
}

/* Map a gain like "128" onto an encoding. Prints the legal set itself. */
static int parse_gain_arg(const char *token, nau7802_gain_t *gain)
{
    int requested = 0;
    if (cli_parse_int_arg(token, &requested) < 0) {
        diag_error("Gain must be a number");
        return -1;
    }

    if (nau7802_gain_from_value(requested, gain) == ESP_OK) {
        return 0;
    }

    diag_printf("Gain must be one of:");
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
        diag_error("The scale factor must be a number, as reported by "
                 "'calibrate' or 'status'");
        return -1;
    }
    if (*counts_per_unit == 0.0) {
        diag_error("A scale factor of zero would divide every weight to "
                 "infinity. Use the number 'calibrate' reported.");
        return -1;
    }
    return 0;
}

/* Map a voltage like "3.0" onto an encoding. Prints the legal set itself. */
static int parse_ldo_arg(const char *token, nau7802_ldo_t *ldo)
{
    double volts = 0.0;
    if (cli_parse_double_arg(token, &volts) < 0) {
        diag_error("LDO voltage must be a number");
        return -1;
    }

    const int millivolts = (int)(volts * 1000.0 + 0.5);
    if (nau7802_ldo_from_millivolts(millivolts, ldo) == ESP_OK) {
        return 0;
    }

    diag_printf("LDO voltage must be one of:");
    for (size_t i = 0; i < sizeof(nau7802_ldos) / sizeof(nau7802_ldos[0]); i++) {
        diag_printf(" %.1f", nau7802_ldo_millivolts(nau7802_ldos[i]) / 1000.0);
    }
    diag_printf("\n");
    return -1;
}

static void print_init_usage(void)
{
    diag_printf("Usage: init [ldo <volts>] [drdy <pin>] [gain <1..128>] "
              "[scale <counts_per_unit>]\n");
    diag_printf("Without 'ldo', AVDD is taken from the pin (chip default).\n");
    diag_printf("Without 'drdy', conversions are detected by polling over I2C.\n");
    diag_printf("Without 'gain', the chip's own default of x1 applies -- which is "
              "far too low for a load cell. See the note under 'gain'.\n");
    diag_printf("'scale' installs a factor from an earlier bench calibration "
              "instead of measuring one. Give it here rather than running "
              "'scale' afterwards: bring-up drops the scale, so a factor set "
              "before 'init' does not survive. Still run 'tare' before "
              "weighing.\n");
}

/* ------------------------------------------------------------------ */
/* init                                                               */
/* ------------------------------------------------------------------ */

static void report_bringup_failure(const nau7802_bringup_report_t *report,
                                   esp_err_t err)
{
    switch (report->failed_stage) {
    case NAU7802_STAGE_PROBE:
        diag_error("No response from 0x%02X: %s", NAU7802_I2C_ADDRESS,
                 esp_err_to_name(err));
        break;
    case NAU7802_STAGE_RESET:
        diag_error("Resetting the device failed");
        break;
    case NAU7802_STAGE_POWER_DIGITAL:
        diag_error("Powering up the digital section failed");
        break;
    case NAU7802_STAGE_POWER_READY:
        diag_error("Device never reported power-up ready (PU_CTRL.PUR): %s",
                 esp_err_to_name(err));
        break;
    case NAU7802_STAGE_SET_LDO:
        diag_error("Setting the LDO voltage failed");
        break;
    case NAU7802_STAGE_SET_GAIN:
        diag_error("Setting the gain failed");
        break;
    case NAU7802_STAGE_POWER_ANALOG:
        diag_error("Powering up the analog section failed");
        break;
    case NAU7802_STAGE_ADC_CTRL:
        diag_error("Writing REG0x15 failed");
        break;
    case NAU7802_STAGE_CALIBRATE:
        report_calibration_error(err);
        break;
    case NAU7802_STAGE_START:
        diag_error("Starting conversions failed");
        break;
    case NAU7802_STAGE_SETTLE:
        report_read_error(err, "Discarding the settling conversions");
        break;
    default:
        diag_error("NAU7802 bring-up failed: %s", esp_err_to_name(err));
        break;
    }
}

static void report_bringup_success(const nau7802_bringup_report_t *report)
{
    diag_printf("NAU7802 ready at 0x%02X (device revision 0x%02X)\n",
              NAU7802_I2C_ADDRESS, report->revision);

    if (report->drdy_gpio >= 0) {
        diag_printf("Conversions signalled by DRDY on GPIO %d\n", report->drdy_gpio);
    } else {
        diag_printf("No DRDY pin: conversions are detected by polling the CR bit "
                  "over I2C, which cannot start the read at a known point in "
                  "the conversion. Wire DRDY and use 'init drdy <pin>' if the "
                  "readings show occasional large outliers.\n");
    }

    if (report->ldo_enabled) {
        diag_printf("AVDD from the internal LDO at %.1f V\n",
                  nau7802_ldo_millivolts(report->ldo) / 1000.0);
    } else {
        diag_printf("AVDD taken from the pin. If the load cell reads nothing, this "
                  "board may need the internal regulator: 'init ldo 3.0'\n");
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
    diag_printf("PGA gain x%d\n", gain);
    if (!report->gain_was_requested && gain < 128) {
        diag_printf("That is the chip's power-up default, not a choice this "
                  "command made: 'init' resets the registers, which returns the "
                  "gain to x1 whatever it was set to before. A load cell puts "
                  "out a few millivolts, so at x1 a full load is a fraction of "
                  "a percent of full scale and is indistinguishable from noise. "
                  "Run 'gain 128', or pass 'init ... gain 128' next time.\n");
    }

    /* Read back like the gain above. A chopper left at the power-up 00 costs
     * six bits and is otherwise invisible -- the converter still works and the
     * numbers still look plausible. */
    if (report->adc_ctrl_valid) {
        if (report->chopper_off) {
            diag_printf("Chopper clock off (REG0x15 = 0x%02X), per data sheet "
                      "section 9.1\n", report->adc_ctrl);
        } else {
            diag_error("REG0x15 reads 0x%02X, REG_CHPS %u -- the write did not "
                     "take. Every encoding but 3 is Reserved, and the wrong one "
                     "costs about six bits of resolution.",
                     report->adc_ctrl, report->chps);
        }
    }

    /*
     * A supplied factor changes what is left to do: the span is already known,
     * and only the zero is outstanding. Telling someone to calibrate against a
     * known mass when they have just handed over a bench-measured factor is
     * advice for the wrong task.
     */
    const nau7802_scale_t *scale = nau7802_get_scale(nau);
    if (scale->supplied) {
        diag_printf("Internal offset calibration passed. Scale %.1f counts per "
                  "unit (supplied, not measured); a factor fixes the span, not "
                  "the zero, so run 'tare' with no load, then 'weight'.\n",
                  scale->counts_per_unit);
        return;
    }

    diag_printf("Internal offset calibration passed. Run 'tare' with no load, then "
              "'calibrate <known mass>'.\n");
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
     * LDO (many load cell breakouts do) need 'init ldo <volts>'.
     */
    nau7802_bringup_opts_t opts = {0};
    int requested_drdy = -1;

    /* All three options describe how the board is wired, so any order is fine
     * and none is required. */
    for (int i = 1; i < argc; i++) {
        if (strcasecmp(argv[i], "ldo") == 0) {
            if (++i >= argc) {
                diag_error("Give the LDO voltage, e.g. 'init ldo 3.0'");
                return -1;
            }
            if (parse_ldo_arg(argv[i], &opts.ldo) < 0) {
                return -1;
            }
            opts.use_internal_ldo = true;
        } else if (strcasecmp(argv[i], "drdy") == 0) {
            if (++i >= argc) {
                diag_error("Give the GPIO the DRDY pin is wired to, e.g. 'init drdy 7'");
                return -1;
            }
            if (cli_parse_int_arg(argv[i], &requested_drdy) < 0) {
                diag_error("DRDY must be a GPIO number");
                return -1;
            }
        } else if (strcasecmp(argv[i], "gain") == 0) {
            if (++i >= argc) {
                diag_error("Give the gain, e.g. 'init gain 128'");
                return -1;
            }
            if (parse_gain_arg(argv[i], &opts.gain) < 0) {
                return -1;
            }
            opts.set_gain = true;
        } else if (strcasecmp(argv[i], "scale") == 0) {
            if (++i >= argc) {
                diag_error("Give the scale factor, e.g. 'init scale 214.7'");
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
        diag_error("Configuring GPIO %d as the DRDY input failed", requested_drdy);
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
            diag_error("Reading CTRL1 failed");
            return -1;
        }
        diag_printf("Gain is x%d\n", nau7802_gain_value(gain));
        return 0;
    }

    nau7802_gain_t gain;
    if (parse_gain_arg(argv[1], &gain) < 0) {
        return -1;
    }

    /* Changing the analog path invalidates the offset calibration. */
    diag_printf("Gain set to x%d; re-running internal offset calibration\n",
              nau7802_gain_value(gain));

    nau7802_change_report_t change = {0};
    esp_err_t err = nau7802_set_gain(nau, gain, &change);
    return report_change(err, &change, "Setting the gain");
}

int cmd_nau7802_rate(int argc, char **argv)
{
    if (!require_init()) {
        return -1;
    }

    if (argc < 2) {
        nau7802_rate_t rate;
        if (nau7802_get_rate(nau, &rate) != ESP_OK) {
            diag_error("Reading CTRL2 failed");
            return -1;
        }
        const int sps = nau7802_rate_sps(rate);
        if (sps < 0) {
            diag_printf("Sample rate register holds an undefined encoding\n");
        } else {
            diag_printf("Sample rate is %d SPS\n", sps);
        }
        return 0;
    }

    int requested = 0;
    if (cli_parse_int_arg(argv[1], &requested) < 0) {
        diag_error("Sample rate must be a number");
        return -1;
    }

    nau7802_rate_t rate;
    if (nau7802_rate_from_sps(requested, &rate) != ESP_OK) {
        diag_printf("Sample rate must be one of: 10 20 40 80 320 SPS\n");
        return -1;
    }

    /* The conversion rate changes the modulator and filter settings, so the
     * existing offset calibration no longer applies. Skipping this leaves the
     * ADC returning values that swing across most of the full-scale range. */
    diag_printf("Sample rate set to %d SPS; re-running internal offset "
              "calibration\n", requested);

    /* Measured on the board this was developed against: 10-80 SPS are rock
     * steady, while 320 SPS returns values swinging across the whole range.
     * Likely an oscillator limitation -- 320 SPS may need an external crystal
     * (PU_CTRL.OSCS) rather than the internal RC. */
    if (requested == 320) {
        diag_printf("Note: 320 SPS has been observed returning invalid, "
                  "full-range data with the internal RC oscillator. "
                  "Check the reading before trusting it.\n");
    }

    nau7802_change_report_t change = {0};
    esp_err_t err = nau7802_set_rate(nau, rate, &change);
    return report_change(err, &change, "Setting the sample rate");
}

int cmd_nau7802_input(int argc, char **argv)
{
    if (!require_init()) {
        return -1;
    }

    if (argc < 2) {
        nau7802_channel_t channel;
        if (nau7802_get_channel(nau, &channel) != ESP_OK) {
            diag_error("Reading CTRL2 failed");
            return -1;
        }
        diag_printf("Input channel: %s\n",
                  channel == NAU7802_CHANNEL_B ? "B" : "A");
        return 0;
    }

    nau7802_channel_t channel;
    if (strcasecmp(argv[1], "a") == 0) {
        channel = NAU7802_CHANNEL_A;
    } else if (strcasecmp(argv[1], "b") == 0) {
        channel = NAU7802_CHANNEL_B;
    } else {
        diag_error("Input must be 'a' or 'b'");
        return -1;
    }

    const char *name = (channel == NAU7802_CHANNEL_B) ? "B" : "A";
    diag_printf("Input channel set to %s; re-running internal offset calibration\n",
              name);

    nau7802_change_report_t change = {0};
    esp_err_t err = nau7802_set_channel(nau, channel, &change);
    return report_change(err, &change, "Switching the input channel");
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
            diag_printf("No DRDY pin; conversions are detected by polling the CR "
                      "status bit over I2C\n");
        } else {
            diag_printf("DRDY on GPIO %d, reading %s right now\n", pin,
                      nau7802_drdy_level(nau) ? "high (a result is waiting)"
                                              : "low (no result waiting)");
        }
        return 0;
    }

    if (strcasecmp(argv[1], "off") == 0) {
        nau7802_set_drdy(nau, -1);
        diag_printf("DRDY released; back to polling the CR status bit over I2C\n");
        return 0;
    }

    int requested = 0;
    if (cli_parse_int_arg(argv[1], &requested) < 0) {
        diag_printf("Usage: drdy [<pin>|off]\n");
        return -1;
    }

    esp_err_t err = nau7802_set_drdy(nau, requested);
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_ARG) {
            diag_error("GPIO %d does not exist on this chip", requested);
        }
        diag_error("Configuring GPIO %d as the DRDY input failed", requested);
        return -1;
    }

    diag_printf("DRDY on GPIO %d\n", requested);

    /*
     * A pin that is low here is the normal case -- the last result was read --
     * so silence is not evidence either way. Saying nothing at all about it
     * would leave a typo to surface later as a timeout mid-measurement.
     */
    diag_printf("Run 'read' to confirm it: a wrong pin times out rather than "
              "reporting bad numbers.\n");
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
            diag_error("Reading the PGA register failed");
            return -1;
        }
        diag_printf("LDOMODE %d: the AVDD capacitor %s\n", stable ? 1 : 0,
                  stable ? "may have ESR up to 5 ohms"
                         : "must have ESR below 1 ohm");
        diag_printf("%s\n", stable
                  ? "More stable regulator loop, lower DC gain."
                  : "Better DC accuracy, higher loop gain. This is the chip "
                    "default, and it is only correct if the board's AVDD "
                    "capacitor really is below 1 ohm ESR.");
        return 0;
    }

    int mode = 0;
    if (cli_parse_int_arg(argv[1], &mode) < 0 || (mode != 0 && mode != 1)) {
        diag_printf("Usage: ldomode [0|1]\n");
        diag_printf("  0  AVDD capacitor ESR below 1 ohm (chip default)\n");
        diag_printf("  1  AVDD capacitor ESR up to 5 ohms\n");
        return -1;
    }

    /* The two modes settle AVDD at slightly different levels, and AVDD is the
     * reference, so the existing offset calibration no longer applies. */
    diag_printf("LDOMODE set to %d; re-running internal offset calibration\n", mode);

    nau7802_change_report_t change = {0};
    esp_err_t err = nau7802_set_ldomode(nau, mode != 0, &change);
    return report_change(err, &change, "Setting LDOMODE");
}

int cmd_nau7802_pgacap(int argc, char **argv)
{
    if (!require_init()) {
        return -1;
    }

    if (argc < 2) {
        bool enabled = false;
        if (nau7802_get_pga_cap(nau, &enabled) != ESP_OK) {
            diag_error("Reading the power control register failed");
            return -1;
        }
        diag_printf("PGA output bypass capacitor: %s\n",
                  enabled ? "enabled" : "disabled");
        return 0;
    }

    bool enable;
    if (strcasecmp(argv[1], "on") == 0) {
        enable = true;
    } else if (strcasecmp(argv[1], "off") == 0) {
        enable = false;
    } else {
        diag_printf("Usage: pgacap [on|off]\n");
        return -1;
    }

    if (enable) {
        /*
         * Worth saying every time rather than once in the docs: with no
         * capacitor fitted this quietly does nothing, and it takes channel 2
         * away whether or not it helps.
         */
        diag_printf("PGA output bypass capacitor enabled. This needs a capacitor "
                  "fitted across VIN2P/VIN2N (330 pF at AVDD 3.3 V, 680 pF at "
                  "4.5 V); with none there it changes nothing. Channel B now "
                  "reads the filter node, not an input.\n");
    } else {
        diag_printf("PGA output bypass capacitor disabled; channel B is an input "
                  "again\n");
    }

    diag_printf("Re-running internal offset calibration\n");

    nau7802_change_report_t change = {0};
    esp_err_t err = nau7802_set_pga_cap(nau, enable, &change);
    return report_change(err, &change,
                         "Setting the PGA output bypass capacitor");
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
            diag_error("Invalid sample count: %s", argv[arg_idx]);
            return -1;
        }
        arg_idx++;
    }

    const char *channel = "a";
    if (arg_idx < argc) {
        if (strcasecmp(argv[arg_idx], "a") != 0 &&
            strcasecmp(argv[arg_idx], "b") != 0) {
            diag_error("Channel must be 'a' or 'b': %s", argv[arg_idx]);
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
        diag_error("Reading CTRL2 failed");
        return -1;
    }

    if (want != current) {
        diag_printf("Input channel set to %s; re-running internal offset "
                  "calibration\n", want == NAU7802_CHANNEL_B ? "B" : "A");

        nau7802_change_report_t change = {0};
        esp_err_t err = nau7802_set_channel(nau, want, &change);
        if (report_change(err, &change, "Switching the input channel") < 0) {
            return -1;
        }
    }

    diag_printf("Raw ADC readings (channel %s):\n", channel);

    for (int i = 0; i < samples; i++) {
        int32_t value = 0;
        esp_err_t err = nau7802_read_raw(nau, &value);
        if (err != ESP_OK) {
            char context[48];
            snprintf(context, sizeof(context), "Reading sample %d", i + 1);
            report_read_error(err, context);
            return -1;
        }

        const double percent = value * 100.0 / NAU7802_FULL_SCALE;
        diag_printf("  %8ld  (%.4f%% of full scale)\n", (long)value, percent);
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
    diag_printf("Raw %10.1f counts  (%d samples, spread %ld, %.4f%% of full scale)\n",
              stats.mean, samples, (long)(stats.max - stats.min),
              stats.mean * 100.0 / NAU7802_FULL_SCALE);

    const nau7802_scale_t *scale = nau7802_get_scale(nau);

    if (scale->tare_samples > 0) {
        diag_printf("     %10.1f counts net of tare\n",
                  stats.mean - scale->tare_counts);
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
            diag_printf("     %10.4f units\n", net / scale->counts_per_unit);
        } else {
            diag_printf("     %10.4f units  (+/-%.4f)\n",
                      net / scale->counts_per_unit,
                      hypot(stats.stderr_mean, scale->tare_stderr) /
                          fabs(scale->counts_per_unit));
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
 * only moment the firmware knows it -- nothing downstream of 'calibrate' can
 * recover how good the number was.
 */
static void print_scale_for_consumer(double counts_per_unit, nau7802_gain_t gain,
                                     bool gain_valid, double precision_percent,
                                     bool precision_valid)
{
    diag_printf("    scale factor for the consumer:  %.17g  /*", counts_per_unit);
    if (precision_valid) {
        diag_printf(" +/-%.2f%%,", precision_percent);
    }
    if (gain_valid) {
        diag_printf(" gain x%d,", nau7802_gain_value(gain));
    }
    diag_printf(" counts per unit */\n");
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

    diag_printf("Tare set to %ld counts (%d samples, spread %ld, mean known to "
              "+/-%.1f)\n", (long)stats.mean, samples,
              (long)(stats.max - stats.min), stats.stderr_mean);
    return 0;
}

int cmd_nau7802_calibrate(int argc, char **argv)
{
    if (!require_init()) {
        return -1;
    }

    if (argc < 2) {
        diag_printf("Usage: calibrate <known mass>\n");
        diag_printf("Run 'tare' with the scale empty first, then place a known "
                  "mass and run this. The unit is whatever you use here.\n");
        return -1;
    }

    double known = 0.0;
    if (cli_parse_double_arg(argv[1], &known) < 0 || known == 0.0) {
        diag_error("The known mass must be a non-zero number");
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
        diag_error("Both 'tare' and 'calibrate' need at least two samples: a "
                 "single conversion has no spread, so there is no way to tell a "
                 "real change from noise. Re-run as 'tare 10' and "
                 "'calibrate %g 10'.", known);
        return -1;
    }

    if (err == ESP_ERR_NAU7802_WITHIN_NOISE) {
        diag_error("The reading moved %.1f counts from the tare, and the two "
                 "averages are only known to +/-%.1f counts between them -- so "
                 "the move is within the noise. Is the mass on the cell, and "
                 "was 'tare' run while it was empty? If the part is simply "
                 "noisy, average harder: 'tare 100' then "
                 "'calibrate %g 100' cuts the uncertainty by sqrt(10).",
                 result.net_counts, result.uncertainty, known);

        /*
         * Averaging is the wrong advice when the signal is small because the
         * PGA is turned down -- no amount of it recovers a factor of 128. Say
         * so, because a gain of x1 is what `init` leaves behind and nothing
         * about the numbers points at it.
         */
        if (result.gain_valid) {
            const int gain = nau7802_gain_value(result.gain);
            if (gain > 0 && gain < 128) {
                diag_printf("The PGA is at gain x%d, and 'init' resets it to x1 "
                          "however it was set before. A load cell's output is a "
                          "few millivolts; 'gain 128' would make this signal "
                          "%dx larger, which no amount of averaging can.\n",
                          gain, 128 / gain);
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
    diag_printf("Calibrated: %.1f counts per unit (%.1f counts for %.4f units)\n",
              result.counts_per_unit, result.net_counts, known);
    diag_printf("Scale is good to +/-%.2f%%, from %.1f counts of uncertainty in "
              "the tare and this measurement together\n",
              result.precision_percent, result.uncertainty);
    if (result.precision_percent > 1.0) {
        diag_printf("For a tighter scale, average more: uncertainty falls as "
                  "sqrt(samples), so 'tare 100' and 'calibrate %g 100' gets "
                  "about %.2f%%\n", known,
                  result.precision_percent / sqrt(100.0 / samples));
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
            diag_printf("No scale factor. Either run 'tare' and "
                      "'calibrate <known mass>', or set a factor from an "
                      "earlier bench run with 'scale <counts_per_unit>'.\n");
            return 0;
        }
        diag_printf("Scale %.1f counts per unit (%s)\n", scale->counts_per_unit,
                  scale->supplied ? "supplied, not measured this session"
                                  : "measured this session");
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
        diag_error("Setting the scale factor failed: %s", esp_err_to_name(err));
        return -1;
    }

    diag_printf("Scale set to %.1f counts per unit (supplied, not measured)\n",
              counts_per_unit);

    /*
     * The factor says nothing about where zero is, and the two are separate
     * measurements. Saying so here is cheaper than letting 'weight' refuse and
     * be the first to mention it.
     */
    if (scale->tare_samples == 0) {
        diag_printf("No tare yet. Run 'tare' with the cell empty before "
                  "weighing; a factor fixes the span, not the zero.\n");
    }

    /*
     * A factor is counts per unit at one gain, and every setter that perturbs
     * the analog path drops it -- including 'init'. Worth saying at the moment
     * someone has just typed a number in by hand.
     */
    diag_printf("This factor belongs to the gain now in force and is dropped by "
              "'gain', 'input', 'ldomode', 'pgacap' and 'init'. To survive "
              "bring-up, give it as 'init ... scale %.17g' instead.\n",
              counts_per_unit);
    return 0;
}

int cmd_nau7802_weight(int argc, char **argv)
{
    if (!require_init()) {
        return -1;
    }

    const nau7802_scale_t *scale = nau7802_get_scale(nau);
    if (!scale->calibrated) {
        diag_error("Not calibrated. Run 'tare' with no load, then "
                 "'calibrate <known mass>'.");
        return -1;
    }

    /*
     * Reachable only with a supplied factor: 'calibrate' refuses without a tare
     * of at least two samples, so before 'scale' existed a calibrated scale
     * implied a real zero. Without one the subtraction is against zero, and an
     * unloaded bridge sits tens of thousands of counts away from that -- which
     * would be reported as load, confidently. Refuse before spending the
     * conversions.
     */
    if (scale->tare_samples == 0) {
        diag_error("A scale factor is set but no tare has been taken, so there is "
                 "no zero to measure from and every weight would be the "
                 "bridge's own offset reported as load. Run 'tare' with the "
                 "cell empty.");
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
        diag_printf("Weight %12.4f units  (1 sample, no noise estimate, "
                  "%.1f raw counts)\n", weight.units, weight.net_counts);
        return 0;
    }

    diag_printf("Weight %12.4f units  (+/-%.4f over %d samples, spread %.4f, "
              "%.1f raw counts)\n", weight.units, weight.uncertainty_units,
              samples, weight.spread_units, weight.net_counts);
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
        diag_error("No response from 0x%02X", NAU7802_I2C_ADDRESS);
        return -1;
    }

    diag_printf("Device revision 0x%02X at 0x%02X\n", status.revision,
              NAU7802_I2C_ADDRESS);
    diag_printf("PU_CTRL 0x%02X  digital %s, analog %s, ready %s, data %s\n",
              status.pu_ctrl,
              status.digital_up ? "up" : "down",
              status.analog_up ? "up" : "down",
              status.power_ready ? "yes" : "no",
              status.data_ready ? "ready" : "pending");
    diag_printf("AVDD source: %s\n",
              status.internal_ldo ? "internal LDO" : "AVDD pin");
    diag_printf("CTRL1   0x%02X  gain x%d, LDO %.1f V\n", status.ctrl1,
              nau7802_gain_value(status.gain),
              nau7802_ldo_millivolts(status.ldo) / 1000.0);
    diag_printf("CTRL2   0x%02X  %d SPS, calibration %s\n", status.ctrl2,
              status.rate_sps, status.cal_error ? "ERROR" : "ok");
    diag_printf("Input channel: %s\n",
              status.channel == NAU7802_CHANNEL_B ? "B" : "A");

    /*
     * CAL_ERR is one bit, and it only says the device gave up. The offset it
     * settled on is the number that says whether the front end is anywhere near
     * balanced: an OCAL close to zero means the bridge sits near mid-supply,
     * one up against the rails means it does not and the gain has nowhere to
     * go. Note that OCAL is sign-magnitude; the driver decodes it.
     */
    if (status.cal_regs_valid) {
        diag_printf("Channel %c calibration: OCAL 0x%06lX (%ld counts), "
                  "GCAL 0x%08lX (x%.6f)\n",
                  status.channel == NAU7802_CHANNEL_B ? 'B' : 'A',
                  (unsigned long)status.ocal_raw, (long)status.ocal_counts,
                  (unsigned long)status.gcal_raw, status.gcal_ratio);
    } else {
        diag_printf("Calibration registers: <error reading>\n");
    }

    if (status.drdy_gpio >= 0) {
        diag_printf("Data ready: DRDY on GPIO %d, now %s\n", status.drdy_gpio,
                  status.drdy_level ? "high" : "low");
    } else {
        diag_printf("Data ready: no DRDY pin, polling the CR bit over I2C\n");
    }

    if (status.pga_valid) {
        diag_printf("PGA     0x%02X  LDOMODE %d (AVDD capacitor ESR up to %s)\n",
                  status.pga, status.ldomode ? 1 : 0,
                  status.ldomode ? "5 ohms" : "1 ohm");
        diag_printf("POWER   0x%02X  PGA output bypass capacitor %s\n", status.power,
                  status.pga_cap ? "enabled" : "disabled");
    }

    /*
     * REG0x15 is worth a decoded line rather than leaving it to `registers`:
     * a chopper left at the power-up 00 is the difference between about twelve
     * and about eighteen effective bits, and nothing else in this output would
     * show it.
     */
    if (status.adc_ctrl_valid) {
        diag_printf("ADC     0x%02X  REG_CHPS %u, chopper clock %s\n",
                  status.adc_ctrl, status.chps,
                  status.chopper_off ? "off (as prescribed)" : "NOT SET");
        if (!status.chopper_off) {
            diag_printf("REG0x15 should read 0x30 after 'init'. Every other "
                      "REG_CHPS encoding is Reserved and costs about six bits "
                      "of resolution.\n");
        }
    }

    if (status.brought_up) {
        diag_printf("Tare %ld counts; %s\n", (long)status.scale.tare_counts,
                  status.scale.calibrated ? "calibrated" : "not calibrated");
        if (status.scale.calibrated) {
            diag_printf("Scale %.1f counts per unit (%s)\n",
                      status.scale.counts_per_unit,
                      status.scale.supplied ? "supplied, not measured this session"
                                            : "measured this session");
            print_scale_for_consumer(status.scale.counts_per_unit, status.gain,
                                     true, 0.0, false);
            /*
             * The +/- on a weight has always been this session's noise
             * propagated through a factor treated as exact -- 'calibrate' never
             * folded its own precision into it either. That is easy to misread
             * as accuracy, and a supplied factor is where it would mislead
             * most, since the firmware cannot know how good the number is.
             */
            if (status.scale.supplied) {
                diag_printf("The +/- on each weight is this session's noise "
                          "only; it does not include the accuracy of a "
                          "supplied factor, which this firmware cannot "
                          "know.\n");
            }
            if (status.scale.tare_samples == 0) {
                diag_printf("No tare yet -- a factor fixes the span, not the "
                          "zero. Run 'tare' with the cell empty before "
                          "weighing.\n");
            }
        }
    } else {
        diag_printf("Not initialized by this session; run 'nau7802 init'\n");
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

    diag_printf("Register dump for NAU7802 at 0x%02X:\n", NAU7802_I2C_ADDRESS);

    size_t count = 0;
    const nau7802_register_info_t *map = nau7802_register_map(&count);

    for (size_t i = 0; i < count; i++) {
        uint8_t value = 0;
        if (nau7802_read_register(nau, map[i].reg, &value) == ESP_OK) {
            diag_printf("  REG%02X %-10s 0x%02X\n", map[i].reg, map[i].name, value);
        } else {
            diag_printf("  REG%02X %-10s <error reading>\n", map[i].reg, map[i].name);
        }
    }

    return 0;
}
