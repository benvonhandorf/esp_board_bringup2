/*
 * Console commands for the HX711. The driver is the hx711 shared component.
 *
 * What stays here is everything the component deliberately does not do: which
 * pins this firmware may drive, how many samples a command defaults to, and
 * every line of text. The data sheet lives on the other side of the API.
 */
#include "app_bringup.h"
#include "hx711_cmd.h"

#include "gpio.h"

#include "driver/gpio.h"

#include "hx711.h"

#include <math.h>

/*
 * MAX_SAMPLES is inherited from the NAU7802, where 200 samples is under a
 * second. On an HX711 strapped to 10 SPS it is twenty, which is why
 * take_batch() warns before a long one.
 */
#define MAX_SAMPLES     200
#define DEFAULT_SAMPLES  10

/*
 * Created only by `init`, and non-NULL exactly when the part is up. There is no
 * lazy constructor, because unlike `i2c nau7802 drdy` no command here is a
 * statement about the board that works without pins -- the one thing `status`
 * prints uninitialized comes from the driver's free encoding functions.
 */
static hx711_handle_t hx;

/* ------------------------------------------------------------------ */
/* Guards                                                              */
/* ------------------------------------------------------------------ */

/*
 * Initialization is checked before power, and the order is deliberate: when
 * neither has happened both are true, but power-down is only reachable *after*
 * init, so "not initialized" is never the wrong answer while "powered down"
 * would name a state that cannot exist.
 */
static bool require_init(void)
{
    if (!hx || !hx711_is_ready(hx)) {
        diag_error("HX711 not initialized. Run 'loadcell init <dout> <sck>' first.");
        return false;
    }
    return true;
}

static bool require_ready(void)
{
    if (!require_init()) {
        return false;
    }
    if (hx711_is_powered_down(hx)) {
        diag_error("HX711 is powered down. Run 'loadcell power on' first.");
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Formatting the driver's facts                                       */
/* ------------------------------------------------------------------ */

static char channel_char(hx711_mode_t mode)
{
    return hx711_mode_channel(mode) == HX711_CHANNEL_B ? 'B' : 'A';
}

static void print_mode_of(hx711_mode_t mode)
{
    diag_printf("Channel %c, gain %d (%d clock pulses)\n", channel_char(mode),
              hx711_mode_gain(mode), hx711_mode_pulses(mode));
}

static void print_mode(void)
{
    hx711_mode_t mode = HX711_MODE_A128;
    hx711_get_mode(hx, &mode);
    print_mode_of(mode);
}

/*
 * Report a failure to take a conversion.
 *
 * The driver returns the reason as a code; which pin to blame and what to check
 * is the console's to say, because it is the only side that knows the pins were
 * typed by a person who might have typed them wrong.
 */
static void report_read_error(esp_err_t err, const char *context)
{
    hx711_status_t status = {0};
    hx711_get_status(hx, &status);

    switch (err) {
    case ESP_ERR_TIMEOUT:
        diag_error("DOUT (GPIO %d) never went low, so no conversion became "
                 "ready. Check the wiring, the part's power, and that PD_SCK "
                 "(GPIO %d) is not stuck high.",
                 status.dout_gpio, status.sck_gpio);
        break;
    case ESP_ERR_HX711_CLOCK_STRETCHED:
        /* worst_high_us is monotonic and was just updated by the burst that
         * failed, so it is that burst's figure. */
        diag_error("A clock pulse was held high for %lu us against the T3 "
                 "limit of %d us; the reading was discarded. If it repeats, "
                 "something is stalling the CPU -- usually a flash write.",
                 (unsigned long)hx711_worst_high_us(hx), HX711_T3_MAX_US);
        break;
    case ESP_ERR_HX711_NO_CLOCK:
        diag_error("DOUT (GPIO %d) did not return high after the clock burst: "
                 "either the clock is not reaching the part -- check PD_SCK is "
                 "on GPIO %d -- or DOUT is shorted to ground.",
                 status.dout_gpio, status.sck_gpio);
        break;
    default:
        if (context) {
            diag_error("%s: %s", context, esp_err_to_name(err));
        }
        break;
    }
}

static void warn_if_saturated(const hx711_stats_t *stats)
{
    /*
     * A converter pinned at either rail is not measuring anything. During
     * bringup that usually means the bridge is disconnected or miswired, the
     * excitation is missing, or the gain is too high for the signal.
     */
    if (stats->saturated) {
        diag_error("ADC saturated at full scale. Check the bridge excitation, "
                 "the gain, and that the bridge sits inside the common mode "
                 "window (AGND+1.2 V to AVDD-1.3 V).");
    }
}

static void report_batch_error(esp_err_t err, const hx711_stats_t *stats,
                               int wanted)
{
    hx711_status_t status = {0};
    hx711_get_status(hx, &status);

    if (err == ESP_ERR_HX711_STUCK_READING) {
        diag_error("All %d readings were identical (0x%06lX), so DOUT (GPIO "
                 "%d) is not carrying real data. Check it is on the HX711's "
                 "DOUT pin and not shorted.",
                 stats->samples, (unsigned long)(stats->min & 0xFFFFFF),
                 status.dout_gpio);
        return;
    }

    report_read_error(err, NULL);
    diag_error("Reading conversion %d of %d failed", stats->samples + 1, wanted);
}

/*
 * Take a batch, warning first if it is going to be a long wait.
 *
 * A console that prints nothing for twenty seconds looks like a hang worth
 * power-cycling, so the warning has to come out before the driver blocks.
 */
static void warn_if_slow(int samples)
{
    hx711_status_t status = {0};
    hx711_get_status(hx, &status);

    if (status.sps > 0.0 && (double)samples / status.sps > 2.0) {
        diag_printf("Averaging %d samples at %.1f SPS will take about %.0f s.\n",
                  samples, status.sps, (double)samples / status.sps);
    }
}

static int take_batch(int samples, hx711_stats_t *stats)
{
    warn_if_slow(samples);

    esp_err_t err = hx711_read_average(hx, samples, stats);
    if (err != ESP_OK) {
        report_batch_error(err, stats, samples);
        return -1;
    }

    warn_if_saturated(stats);
    return 0;
}

static void report_rate(double sps)
{
    switch (hx711_rate_classify(sps)) {
    case HX711_RATE_10SPS:
        diag_printf("Rate %.1f SPS -- RATE strap low (10 SPS nominal), period "
                  "%.0f ms\n", sps, 1000.0 / sps);
        break;
    case HX711_RATE_80SPS:
        diag_printf("Rate %.1f SPS -- RATE strap high (80 SPS nominal), period "
                  "%.1f ms\n", sps, 1000.0 / sps);
        break;
    case HX711_RATE_UNKNOWN:
        diag_printf("Rate %.1f SPS -- neither strap. Check XI (pin 14) is "
                  "grounded; a stalled console task also does this\n", sps);
        break;
    }
}

/*
 * Announce the settling before it happens.
 *
 * The count is fixed and published by the driver, so this agrees with what the
 * next call actually does. It has to come first: a console that prints nothing
 * for half a second looks like a hang, and the whole point of the line is to
 * say why the pause is about to happen.
 */
static void announce_settling(void)
{
    diag_printf("Settling: discarding %d conversions\n", HX711_CHANGE_DISCARDS);
}

/* What the change dropped, which is only known once it has run. */
static void report_scale_dropped(const hx711_change_report_t *change)
{
    if (!change->scale_invalidated) {
        return;
    }

    if (change->scale_was_supplied) {
        /*
         * "Calibrate again" is the wrong advice for a factor that was never
         * measured here: it came from a bench run at a different gain, where it
         * is wrong by exactly the gain ratio.
         */
        diag_printf("Tare and scale dropped: a supplied factor belongs to the "
                  "gain it was measured at. Give both with 'loadcell init "
                  "<dout> <sck> gain <n> scale <counts_per_unit>'.\n");
    } else {
        diag_printf("Tare and scale dropped; run 'loadcell tare' and "
                  "'loadcell calibrate' again.\n");
    }
}

/* ------------------------------------------------------------------ */
/* Argument parsing                                                    */
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

/*
 * Read a scale factor a user typed or pasted back in.
 *
 * Zero is refused because it divides every subsequent weight to infinity, and a
 * negative factor is *not*: a cell wired the other way round genuinely
 * calibrates to one, and the arithmetic handles the sign throughout.
 * cli_parse_double_arg() has already rejected trailing garbage and non-finite
 * values, so this only has to speak to the one case it allows through.
 */
static int parse_scale_arg(const char *token, double *counts_per_unit)
{
    if (cli_parse_double_arg(token, counts_per_unit) < 0) {
        diag_error("Scale factor must be a number");
        return -1;
    }
    if (*counts_per_unit == 0.0) {
        diag_error("Scale factor must not be zero");
        return -1;
    }
    return 0;
}

static int parse_gain_arg(const char *token, hx711_mode_t *out)
{
    int gain = 0;
    if (cli_parse_int_arg(token, &gain) < 0) {
        diag_error("Gain must be a number");
        return -1;
    }

    if (hx711_mode_from_gain(gain, out) == ESP_OK) {
        return 0;
    }

    /*
     * Say which channel each gain implies, because on this part they are not
     * separable and a user who has met the NAU7802 will expect them to be.
     * Written out in gain order rather than generated from hx711_modes[], which
     * is in pulse order, and carrying the aside about channel B.
     */
    diag_printf("Gain must be 32, 64 or 128, and each one fixes the channel "
              "too:\n");
    diag_printf("   32  channel B (26 pulses) -- B has no other gain\n");
    diag_printf("   64  channel A (27 pulses)\n");
    diag_printf("  128  channel A (25 pulses)\n");
    return -1;
}

static void print_init_usage(void)
{
    diag_printf("Usage: init <dout> <sck> [gain <32|64|128>] "
              "[scale <counts_per_unit>]\n");
    diag_printf("<dout> is the GPIO on DOUT (an input here), <sck> the one on "
              "PD_SCK (an output). Gain defaults to 128 on channel A.\n");
}

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
 * only moment the firmware knows it -- nothing downstream of 'loadcell calibrate' can
 * recover how good the number was.
 */
static void print_scale_for_consumer(double counts_per_unit, double precision,
                                     bool precision_valid, hx711_mode_t mode)
{
    diag_printf("    scale factor for the consumer:  %.17g  /*", counts_per_unit);
    if (precision_valid) {
        diag_printf(" +/-%.2f%%,", precision);
    }
    diag_printf(" channel %c gain %d, counts per unit */\n", channel_char(mode),
              hx711_mode_gain(mode));
}

/*
 * Warn whenever channel B is selected, by whichever route.
 *
 * It has to fire for `gain 32` as well as `input b`, because gain and channel
 * are one setting on this part -- and the warning is about where the pins go,
 * not about which command was typed. The data sheet's own application example
 * uses channel B for battery monitoring, and on a load cell breakout INB+/INB-
 * usually go nowhere, so this reads a floating input: it drifts convincingly
 * rather than failing.
 */
static void warn_if_channel_b(hx711_mode_t requested)
{
    if (requested != HX711_MODE_B32) {
        return;
    }
    diag_printf("Channel B has a fixed gain of 32, and on most load cell "
              "breakouts INB+/INB- go nowhere -- the reading will be drift, "
              "not an error.\n");
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static void report_bringup_failure(const hx711_bringup_report_t *report,
                                   esp_err_t err, double scale_wanted)
{
    switch (report->failed_stage) {
    case HX711_STAGE_CONFIGURE_SCK:
        diag_error("Configuring GPIO %d as the PD_SCK output failed",
                 report->sck_gpio);
        break;
    case HX711_STAGE_CONFIGURE_DOUT:
        diag_error("Configuring GPIO %d as the DOUT input failed",
                 report->dout_gpio);
        break;
    case HX711_STAGE_FIRST_READY:
    case HX711_STAGE_SET_MODE:
    case HX711_STAGE_MEASURE_RATE:
    case HX711_STAGE_FIRST_READING:
        report_read_error(err, "Bringing up the HX711");
        break;
    case HX711_STAGE_SET_SCALE:
        diag_error("Setting the scale factor to %g failed", scale_wanted);
        break;
    case HX711_STAGE_PINS:
    case HX711_STAGE_SETTLE:
    case HX711_STAGE_NONE:
        diag_error("Bringing up the HX711 failed: %s", esp_err_to_name(err));
        break;
    }
}

int cmd_hx711_init(int argc, char **argv)
{
    if (argc < 3) {
        print_init_usage();
        return -1;
    }

    int dout = -1;
    int sck = -1;
    if (cli_parse_int_arg(argv[1], &dout) < 0 || cli_parse_int_arg(argv[2], &sck) < 0) {
        diag_error("Both pins must be GPIO numbers");
        return -1;
    }

    hx711_bringup_opts_t opts = {.mode = HX711_MODE_A128};
    for (int i = 3; i < argc; i++) {
        if (strcasecmp(argv[i], "gain") == 0) {
            if (++i >= argc) {
                diag_error("Give the gain, e.g. 'loadcell init 5 6 gain 128'");
                return -1;
            }
            if (parse_gain_arg(argv[i], &opts.mode) < 0) {
                return -1;
            }
        } else if (strcasecmp(argv[i], "scale") == 0) {
            if (++i >= argc) {
                diag_error("Give the scale factor, e.g. 'loadcell init 10 3 scale 1074.3'");
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

    /*
     * The driver checks these too, but the console checks them first and in
     * this order so each complaint names the specific thing that is wrong. The
     * middle one is not the driver's business at all: which pins this firmware
     * may drive -- above everything else, not the console's own -- is a fact
     * about this session, not about the HX711.
     */
    if (!GPIO_IS_VALID_GPIO(dout)) {
        diag_error("GPIO %d does not exist on this chip", dout);
        return -1;
    }

    const char *why = NULL;
    if (!app_pin_is_drivable(sck, &why)) {
        diag_error("GPIO %d cannot be used for PD_SCK: %s", sck, why);
        return -1;
    }

    if (dout == sck) {
        diag_error("DOUT and PD_SCK must be different pins; both were given as "
                 "GPIO %d", dout);
        return -1;
    }

    /* A new pin pair is a new instance, so any previous claim is released
     * rather than abandoned. */
    if (hx) {
        hx711_delete(hx);
        hx = NULL;
    }

    const hx711_config_t config = {.dout_gpio = dout, .sck_gpio = sck};
    esp_err_t err = hx711_create(&config, &hx);
    if (err != ESP_OK) {
        const hx711_bringup_report_t failed = {.failed_stage = HX711_STAGE_NONE};
        report_bringup_failure(&failed, err, 0.0);
        return -1;
    }

    /*
     * DOUT falling is the first half of the proof that a part is there. A
     * floating pin cannot do it -- it is pulled up -- and neither can one
     * shorted low, which never read high to begin with. The second half is the
     * driver's: DOUT must come back high after the burst, which only the clock
     * reaching the device can cause.
     */
    diag_printf("Waiting for the first conversion...\n");
    /*
     * Both waits are announced before the driver blocks for either. In the
     * fused version this line came out between the first-ready wait and the
     * discards, because apply_mode() printed it; a component cannot, and
     * announcing early is the behaviour the line exists for anyway.
     */
    announce_settling();

    hx711_bringup_report_t report = {0};
    err = hx711_bring_up(hx, &opts, &report);
    if (err != ESP_OK) {
        report_bringup_failure(&report, err, opts.counts_per_unit);
        hx711_delete(hx);
        hx = NULL;
        return -1;
    }

    warn_if_saturated(&report.first_reading);

    diag_printf("HX711 responding on DOUT GPIO %d, PD_SCK GPIO %d\n",
              report.dout_gpio, report.sck_gpio);
    print_mode_of(report.mode);
    if (report.rate_measured) {
        report_rate(report.sps);
    }
    diag_printf("Idle reading %.1f counts (5 samples, spread %ld)\n",
              report.first_reading.mean,
              (long)(report.first_reading.max - report.first_reading.min));

    const hx711_scale_t *scale = hx711_get_scale(hx);
    if (scale->supplied) {
        diag_printf("Scale %.1f counts per unit (supplied, not measured)\n",
                  scale->counts_per_unit);
    }
    diag_printf("Run 'loadcell tare' with no load, then %s\n",
              scale->supplied ? "'loadcell weight'"
                              : "'loadcell calibrate <known mass>'");
    return 0;
}

int cmd_hx711_gain(int argc, char **argv)
{
    if (!require_ready()) {
        return -1;
    }

    if (argc < 2) {
        print_mode();
        return 0;
    }

    hx711_mode_t requested = HX711_MODE_A128;
    hx711_get_mode(hx, &requested);
    if (parse_gain_arg(argv[1], &requested) < 0) {
        return -1;
    }

    if (hx711_mode_channel(requested) == HX711_CHANNEL_B) {
        diag_printf("Gain 32 exists only on channel B, so this also switches "
                  "channel.\n");
    }
    warn_if_channel_b(requested);

    announce_settling();

    hx711_change_report_t change = {0};
    esp_err_t err = hx711_set_mode(hx, requested, &change);
    if (err != ESP_OK) {
        report_read_error(err, "Settling after the gain change");
        return -1;
    }
    report_scale_dropped(&change);

    print_mode_of(change.mode);
    return 0;
}

int cmd_hx711_input(int argc, char **argv)
{
    if (!require_ready()) {
        return -1;
    }

    if (argc < 2) {
        print_mode();
        return 0;
    }

    hx711_channel_t channel;
    if (strcasecmp(argv[1], "a") == 0) {
        channel = HX711_CHANNEL_A;
    } else if (strcasecmp(argv[1], "b") == 0) {
        channel = HX711_CHANNEL_B;
    } else {
        diag_error("Input must be 'a' or 'b'");
        return -1;
    }

    if (channel == HX711_CHANNEL_B) {
        warn_if_channel_b(HX711_MODE_B32);
    }

    announce_settling();

    hx711_change_report_t change = {0};
    esp_err_t err = hx711_set_channel(hx, channel, &change);
    if (err != ESP_OK) {
        report_read_error(err, "Settling after the channel change");
        return -1;
    }
    report_scale_dropped(&change);

    print_mode_of(change.mode);
    return 0;
}

int cmd_hx711_power(int argc, char **argv)
{
    /*
     * require_init() rather than require_ready(): 'loadcell power on' is the command
     * that clears the powered-down state, so routing it through the readiness
     * guard would make that state unrecoverable.
     */
    if (!require_init()) {
        return -1;
    }

    if (argc < 2) {
        diag_printf("Power is %s\n", hx711_is_powered_down(hx) ? "down" : "up");
        return 0;
    }

    hx711_status_t status = {0};
    hx711_get_status(hx, &status);

    if (strcasecmp(argv[1], "off") == 0) {
        if (hx711_is_powered_down(hx)) {
            diag_printf("Already powered down\n");
            return 0;
        }
        hx711_power_down(hx);
        diag_printf("PD_SCK (GPIO %d) held high; the part powers down after "
                  "%d us, and the bridge with it if the internal regulator "
                  "feeds it\n", status.sck_gpio, HX711_POWERDOWN_US);
        return 0;
    }

    if (strcasecmp(argv[1], "on") == 0) {
        if (!hx711_is_powered_down(hx)) {
            diag_printf("Already powered up\n");
            return 0;
        }
        announce_settling();

        hx711_change_report_t change = {0};
        esp_err_t err = hx711_power_up(hx, &change);
        if (err != ESP_OK) {
            report_read_error(err, "Powering the HX711 back up");
            return -1;
        }
        report_scale_dropped(&change);
        diag_printf("PD_SCK released; the part reset and the configured "
                  "setting was re-applied\n");
        print_mode_of(change.mode);
        return 0;
    }

    diag_printf("Usage: power [on|off]\n");
    return -1;
}

/*
 * No [a|b] variant, unlike `i2c nau7802 raw`. Switching channel costs five
 * conversions of settling -- half a second at 10 SPS -- so a per-sample
 * alternation would spend nearly all of its time settling and report a handful
 * of readings for the wait.
 */
int cmd_hx711_raw(int argc, char **argv)
{
    if (!require_ready()) {
        return -1;
    }

    int samples = 1;
    if (argc > 1) {
        if (cli_parse_int_arg(argv[1], &samples) < 0 || samples < 1 ||
            samples > MAX_SAMPLES) {
            diag_error("Sample count must be 1-%d", MAX_SAMPLES);
            return -1;
        }
    }

    hx711_mode_t mode = HX711_MODE_A128;
    hx711_get_mode(hx, &mode);
    diag_printf("Raw ADC readings (channel %c, gain %d):\n", channel_char(mode),
              hx711_mode_gain(mode));

    for (int i = 0; i < samples; i++) {
        int32_t value = 0;
        esp_err_t err = hx711_read_raw(hx, &value);
        if (err != ESP_OK) {
            report_read_error(err, "Reading a conversion");
            diag_error("Reading sample %d failed", i + 1);
            return -1;
        }
        diag_printf("  %8ld  (%.4f%% of full scale)\n", (long)value,
                  value * 100.0 / HX711_FULL_SCALE);
    }

    return 0;
}

int cmd_hx711_read(int argc, char **argv)
{
    if (!require_ready()) {
        return -1;
    }

    int samples = 0;
    if (take_sample_count(argc, argv, 1, &samples) < 0) {
        return -1;
    }

    hx711_stats_t stats = {0};
    if (take_batch(samples, &stats) < 0) {
        return -1;
    }

    /* Percent of full scale is wiring-independent; an absolute voltage would
     * depend on AVDD, which this driver has no way to know. */
    diag_printf("Raw %10.1f counts  (%d samples, spread %ld, %.4f%% of full scale)\n",
              stats.mean, samples, (long)(stats.max - stats.min),
              stats.mean * 100.0 / HX711_FULL_SCALE);

    const hx711_scale_t *scale = hx711_get_scale(hx);

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
     * in quadrature with the tare's, not the peak-to-peak spread. Guarded on
     * `calibrated` alone rather than on the tare as well -- a zero tare is a
     * legitimate offset, and dropping the line there would be silent.
     */
    if (scale->calibrated) {
        hx711_weight_t weight = {0};
        hx711_stats_to_weight(hx, &stats, &weight);
        if (samples < 2) {
            /* One conversion has no spread, so quote no error bar rather than
             * a zero one. */
            diag_printf("     %10.4f units\n", weight.units);
        } else {
            diag_printf("     %10.4f units  (+/-%.4f)\n", weight.units,
                      weight.uncertainty_units);
        }
    }

    return 0;
}

int cmd_hx711_tare(int argc, char **argv)
{
    if (!require_ready()) {
        return -1;
    }

    int samples = 0;
    if (take_sample_count(argc, argv, 1, &samples) < 0) {
        return -1;
    }

    warn_if_slow(samples);

    hx711_stats_t stats = {0};
    esp_err_t err = hx711_tare(hx, samples, &stats);
    if (err != ESP_OK) {
        report_batch_error(err, &stats, samples);
        return -1;
    }
    warn_if_saturated(&stats);

    const hx711_scale_t *scale = hx711_get_scale(hx);
    diag_printf("Tare set to %ld counts (%d samples, spread %ld, mean known to "
              "+/-%.1f)\n", (long)scale->tare_counts, samples,
              (long)(stats.max - stats.min), stats.stderr_mean);
    return 0;
}

int cmd_hx711_calibrate(int argc, char **argv)
{
    if (!require_ready()) {
        return -1;
    }

    if (argc < 2) {
        diag_printf("Usage: calibrate <known mass> [samples]\n");
        diag_printf("Run 'loadcell tare' with the scale empty first, then place a known "
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

    warn_if_slow(samples);

    hx711_stats_t stats = {0};
    hx711_calibration_t result = {0};
    esp_err_t err = hx711_calibrate(hx, known, samples, &stats, &result);

    if (err == ESP_ERR_HX711_TOO_FEW_SAMPLES) {
        diag_error("'loadcell tare' and 'loadcell calibrate' need at least two "
                 "samples. Re-run as 'loadcell tare 10' and "
                 "'loadcell calibrate %g 10'.", known);
        return -1;
    }
    if (err == ESP_ERR_HX711_WITHIN_NOISE) {
        diag_error("Moved %.1f counts from the tare, within the +/-%.1f the "
                 "two averages are known to. Is the mass on the cell, and was "
                 "'loadcell tare' run empty? Or average harder: "
                 "'loadcell tare 100' then 'loadcell calibrate %g 100'.",
                 result.net_counts, result.uncertainty, known);
        return -1;
    }
    if (err != ESP_OK) {
        report_batch_error(err, &stats, samples);
        return -1;
    }
    warn_if_saturated(&stats);

    diag_printf("Calibrated: %.1f counts per unit (%.1f counts for %.4f units)\n",
              result.counts_per_unit, result.net_counts, known);
    diag_printf("Scale good to +/-%.2f%% (%.1f counts of uncertainty)\n",
              result.precision_percent, result.uncertainty);
    if (result.precision_percent > 1.0) {
        diag_printf("For about %.2f%%, run 'loadcell tare 100' and "
                  "'loadcell calibrate %g 100'\n",
                  result.precision_percent / sqrt(100.0 / samples), known);
    }
    print_scale_for_consumer(result.counts_per_unit, result.precision_percent,
                             true, result.mode);
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
int cmd_hx711_scale(int argc, char **argv)
{
    if (!require_ready()) {
        return -1;
    }

    const hx711_scale_t *scale = hx711_get_scale(hx);
    hx711_mode_t mode = HX711_MODE_A128;
    hx711_get_mode(hx, &mode);

    if (argc < 2) {
        if (!scale->calibrated) {
            diag_printf("No scale factor. Run 'loadcell tare' then "
                      "'loadcell calibrate <known mass>', or set one with "
                      "'loadcell scale <counts_per_unit>'.\n");
            return 0;
        }
        diag_printf("Scale %.1f counts per unit (%s)\n", scale->counts_per_unit,
                  scale->supplied ? "supplied, not measured this session"
                                  : "measured this session");
        print_scale_for_consumer(scale->counts_per_unit, 0.0, false, mode);
        return 0;
    }

    double counts_per_unit = 0.0;
    if (parse_scale_arg(argv[1], &counts_per_unit) < 0) {
        return -1;
    }

    if (hx711_set_scale(hx, counts_per_unit) != ESP_OK) {
        diag_error("Setting the scale factor to %g failed", counts_per_unit);
        return -1;
    }

    diag_printf("Scale set to %.1f counts per unit (supplied, not measured)\n",
              counts_per_unit);

    /*
     * The factor says nothing about where zero is, and the two are separate
     * measurements. Saying so here is cheaper than letting 'loadcell weight' refuse and
     * be the first to mention it.
     */
    if (scale->tare_samples == 0) {
        diag_printf("No tare yet; run 'loadcell tare' with the cell empty "
                  "before weighing\n");
    }

    /*
     * A factor is counts per unit at one gain, and gain and channel are one
     * setting on this part. Worth saying at the moment someone has just typed a
     * number in by hand.
     */
    diag_printf("This factor belongs to the gain now in force, and a gain or "
              "channel change drops it. To survive 'loadcell init', give it as "
              "'loadcell init <dout> <sck> scale %.17g'.\n", counts_per_unit);
    return 0;
}

int cmd_hx711_weight(int argc, char **argv)
{
    if (!require_ready()) {
        return -1;
    }

    const hx711_scale_t *scale = hx711_get_scale(hx);

    if (!scale->calibrated) {
        diag_error("Not calibrated. Run 'loadcell tare' with no load, then "
                 "'loadcell calibrate <known mass>'.");
        return -1;
    }

    /*
     * Reachable only with a supplied factor: 'loadcell calibrate' refuses without a tare
     * of at least two samples, so before 'loadcell scale' existed a calibrated scale
     * implied a real zero. Without one the subtraction is against zero, and an
     * unloaded bridge sits tens of thousands of counts away from that -- which
     * would be reported as load, confidently. Refuse before spending the
     * conversions.
     */
    if (scale->tare_samples == 0) {
        diag_error("A scale factor is set but no tare has been taken, so every "
                 "weight would be the bridge's own offset reported as load. "
                 "Run 'loadcell tare' with the cell empty.");
        return -1;
    }

    int samples = 0;
    if (take_sample_count(argc, argv, 1, &samples) < 0) {
        return -1;
    }

    hx711_stats_t stats = {0};
    if (take_batch(samples, &stats) < 0) {
        return -1;
    }

    hx711_weight_t weight = {0};
    hx711_stats_to_weight(hx, &stats, &weight);

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

int cmd_hx711_close(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!hx || !hx711_is_ready(hx)) {
        diag_printf("Nothing to release\n");
        return 0;
    }

    /* hx711_delete() leaves the clock low on the way out, and releases PD_SCK
     * pulled down rather than in its reset state: a pin left high powers the
     * part down sixty microseconds later, and the next session would find a
     * device that had silently reset. */
    hx711_delete(hx);
    hx = NULL;

    diag_printf("HX711 released; both pins are back in their reset state\n");
    return 0;
}

/*
 * `i2c nau7802 status` reads its answers back out of silicon, so it reports how
 * the part is really configured rather than what this firmware believes. That
 * is not available here: there is no bus, no register to read, and without
 * `init` not even a pin to read it on. Every line below is therefore either a
 * fact about the part that is true regardless, or something this driver set
 * itself -- and it says which, rather than presenting the second as the first.
 */
int cmd_hx711_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    diag_printf("HX711 24-bit ADC, no control bus and no registers\n");
    diag_printf("Settings (Table 3), selected by the number of clock pulses:\n");
    for (size_t i = 0; i < sizeof(hx711_modes) / sizeof(hx711_modes[0]); i++) {
        diag_printf("  %2d pulses  channel %c, gain %3d\n",
                  hx711_mode_pulses(hx711_modes[i]),
                  channel_char(hx711_modes[i]),
                  hx711_mode_gain(hx711_modes[i]));
    }
    diag_printf("Coding: two's complement, saturating at 0x800000 / 0x7FFFFF\n");

    hx711_status_t status = {0};
    if (hx) {
        hx711_get_status(hx, &status);
    }

    /*
     * The input range scales with AVDD, and a great many breakouts are run at
     * 3.3 V off the ESP rather than the 5 V the front page assumes -- so the
     * "+/-20 mV at gain 128" everyone quotes is a third smaller than they
     * think, and the cell saturates earlier than expected.
     */
    const int gain = hx711_mode_gain(status.mode);
    diag_printf("Input range: +/-0.5 x AVDD/gain -- at gain %d that is "
              "+/-%.1f mV at AVDD 5.0 V, +/-%.1f mV at 3.3 V\n",
              gain, 0.5 * 5000.0 / gain, 0.5 * 3300.0 / gain);
    diag_printf("Common mode window: AGND+1.2 V to AVDD-1.3 V -- outside it a "
              "bridge reads confidently and wrongly; check it with a meter\n");

    if (!status.ready) {
        diag_printf("Not initialized; run 'loadcell init <dout> <sck>'\n");
        return 0;
    }

    /* The datasheet says nothing about what DOUT does while the part is powered
     * down, so do not report the level as if it meant something. */
    if (status.powered_down) {
        diag_printf("DOUT GPIO %d, level not meaningful while powered down\n",
                  status.dout_gpio);
    } else {
        diag_printf("DOUT GPIO %d, now %s\n", status.dout_gpio,
                  status.dout_level ? "high (converting)"
                                    : "low (a result is waiting)");
    }
    diag_printf("PD_SCK GPIO %d, now %s\n", status.sck_gpio,
              status.sck_level ? "high" : "low");
    diag_printf("Power: %s\n", status.powered_down ? "down" : "up");
    diag_printf("Set by this session: ");
    print_mode_of(status.mode);

    bool reached = true;
    if (status.powered_down) {
        diag_printf("Rate not measured while powered down\n");
    } else {
        double sps = 0.0;
        esp_err_t err = hx711_measure_rate(hx, &sps);
        if (err == ESP_OK) {
            report_rate(sps);
        } else {
            /* Say which pin did not answer. The remaining lines are still true,
             * so print them -- but this command failed to reach the part, and
             * `i2c nau7802 status` returns -1 in the same situation rather than
             * reporting success. */
            report_read_error(err, "Measuring the output rate");
            reached = false;
        }
    }

    /* Re-read: the rate measurement above may have moved the timing figures. */
    hx711_get_status(hx, &status);

    diag_printf("Worst PD_SCK high phase %lu us (T3 limit %d us)",
              (unsigned long)status.worst_high_us, HX711_T3_MAX_US);
    if (status.stretched_bursts > 0) {
        diag_printf("; %lu burst(s) exceeded it and were discarded",
                  (unsigned long)status.stretched_bursts);
    }
    diag_printf("\n");

    diag_printf("Tare %ld counts; %s\n", (long)status.scale.tare_counts,
              status.scale.calibrated ? "calibrated" : "not calibrated");
    if (status.scale.calibrated) {
        diag_printf("Scale %.1f counts per unit (%s)\n",
                  status.scale.counts_per_unit,
                  status.scale.supplied ? "supplied, not measured this session"
                                        : "measured this session");
        print_scale_for_consumer(status.scale.counts_per_unit, 0.0, false,
                                 status.mode);
        /*
         * The +/- on a weight has always been this session's noise propagated
         * through a factor treated as exact. That is easy to misread as
         * accuracy, and a supplied factor is where it would mislead most;
         * docs/loadcell.md carries the rest of the reasoning.
         */
        if (status.scale.supplied) {
            diag_printf("The +/- on a weight is repeatability, not the accuracy "
                      "of a supplied factor\n");
        }
        if (status.scale.tare_samples == 0) {
            diag_printf("No tare yet; run 'loadcell tare' with the cell empty "
                      "before weighing\n");
        }
    }

    /*
     * The same split the NAU7802 has, and for the same reason: a soft reset of
     * the ESP loses these while leaving the part converting.
     *
     * Only half of it has to be lost, though, and that asymmetry is worth
     * stating: a scale factor is a property of the cell and the gain, so it can
     * be measured once and compiled into a consumer or passed to 'loadcell init'. The
     * tare cannot -- it is the bridge's own zero, and it moves.
     */
    diag_printf("A reset loses the tare and scale. Give the factor back with "
              "'loadcell init <dout> <sck> scale <counts_per_unit>'; the tare "
              "has to be measured again.\n");
    return reached ? 0 : -1;
}
