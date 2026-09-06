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
        STRRES_ERROR(STR_LOADCELL_NOT_INITIALIZED);
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
        STRRES_ERROR(STR_LOADCELL_POWERED_DOWN);
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
    STRRES_PRINTF(STR_LOADCELL_CHANNEL_GAIN,
                  channel_char(mode), hx711_mode_gain(mode), hx711_mode_pulses(mode));
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
static void report_read_error(esp_err_t err, strres_id_t context)
{
    hx711_status_t status = {0};
    hx711_get_status(hx, &status);

    switch (err) {
    case ESP_ERR_TIMEOUT:
        STRRES_ERROR(STR_LOADCELL_DOUT_NEVER_LOW, status.dout_gpio, status.sck_gpio);
        break;
    case ESP_ERR_HX711_CLOCK_STRETCHED:
        /* worst_high_us is monotonic and was just updated by the burst that
         * failed, so it is that burst's figure. */
        STRRES_ERROR(STR_LOADCELL_CLOCK_STRETCHED,
                     (unsigned long)hx711_worst_high_us(hx), HX711_T3_MAX_US);
        break;
    case ESP_ERR_HX711_NO_CLOCK:
        STRRES_ERROR(STR_LOADCELL_DOUT_STUCK_LOW, status.dout_gpio, status.sck_gpio);
        break;
    default:
        /* The id is a variable here, so this is the one call in the file the
         * compiler cannot check; every id given to it takes exactly one %s. */
        if (context != STRRES_ID_NONE) {
            strres_error(context, esp_err_to_name(err));
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
        STRRES_ERROR(STR_LOADCELL_ADC_SATURATED);
    }
}

static void report_batch_error(esp_err_t err, const hx711_stats_t *stats,
                               int wanted)
{
    hx711_status_t status = {0};
    hx711_get_status(hx, &status);

    if (err == ESP_ERR_HX711_STUCK_READING) {
        STRRES_ERROR(STR_LOADCELL_STUCK_READING,
                     stats->samples, (unsigned long)(stats->min & 0xFFFFFF), status.dout_gpio);
        return;
    }

    report_read_error(err, STRRES_ID_NONE);
    STRRES_ERROR(STR_LOADCELL_CONVERSION_FAILED, stats->samples + 1, wanted);
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
        STRRES_PRINTF(STR_LOADCELL_LONG_WAIT,
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
        STRRES_PRINTF(STR_LOADCELL_RATE_STRAP_LOW, sps, 1000.0 / sps);
        break;
    case HX711_RATE_80SPS:
        STRRES_PRINTF(STR_LOADCELL_RATE_STRAP_HIGH, sps, 1000.0 / sps);
        break;
    case HX711_RATE_UNKNOWN:
        STRRES_PRINTF(STR_LOADCELL_RATE_NO_STRAP, sps);
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
    STRRES_PRINTF(STR_LOADCELL_SETTLING, HX711_CHANGE_DISCARDS);
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
        STRRES_PRINTF(STR_LOADCELL_TARE_SCALE_DROPPED_SUPPLIED);
    } else {
        STRRES_PRINTF(STR_LOADCELL_TARE_SCALE_DROPPED);
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
        STRRES_ERROR(STR_LOADCELL_SAMPLE_COUNT_RANGE, MAX_SAMPLES);
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
        STRRES_ERROR(STR_LOADCELL_SCALE_NOT_A_NUMBER);
        return -1;
    }
    if (*counts_per_unit == 0.0) {
        STRRES_ERROR(STR_LOADCELL_SCALE_ZERO);
        return -1;
    }
    return 0;
}

static int parse_gain_arg(const char *token, hx711_mode_t *out)
{
    int gain = 0;
    if (cli_parse_int_arg(token, &gain) < 0) {
        STRRES_ERROR(STR_LOADCELL_GAIN_NOT_A_NUMBER);
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
    STRRES_PRINTF(STR_LOADCELL_GAIN_CHOICES);
    STRRES_PRINTF(STR_LOADCELL_GAIN_CHOICE_32);
    STRRES_PRINTF(STR_LOADCELL_GAIN_CHOICE_64);
    STRRES_PRINTF(STR_LOADCELL_GAIN_CHOICE_128);
    return -1;
}

static void print_init_usage(void)
{
    STRRES_PRINTF(STR_LOADCELL_USAGE_INIT);
    STRRES_PRINTF(STR_LOADCELL_USAGE_INIT_PINS);
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
        STRRES_PRINTF(STR_LOADCELL_SCALE_IS_SUPPLIED, counts_per_unit);
    } else {
        STRRES_PRINTF(STR_LOADCELL_SCALE_IS_MEASURED, counts_per_unit);
    }
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
    STRRES_PRINTF(STR_LOADCELL_SCALE_FACTOR_COMMENT, counts_per_unit);
    if (precision_valid) {
        diag_printf(" +/-%.2f%%,", precision);
    }
    STRRES_PRINTF(STR_LOADCELL_SCALE_FACTOR_COMMENT_END,
                  channel_char(mode), hx711_mode_gain(mode));
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
    STRRES_PRINTF(STR_LOADCELL_CHANNEL_B_NOTE);
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static void report_bringup_failure(const hx711_bringup_report_t *report,
                                   esp_err_t err, double scale_wanted)
{
    switch (report->failed_stage) {
    case HX711_STAGE_CONFIGURE_SCK:
        STRRES_ERROR(STR_LOADCELL_SCK_CONFIG_FAILED, report->sck_gpio);
        break;
    case HX711_STAGE_CONFIGURE_DOUT:
        STRRES_ERROR(STR_LOADCELL_DOUT_CONFIG_FAILED, report->dout_gpio);
        break;
    case HX711_STAGE_FIRST_READY:
    case HX711_STAGE_SET_MODE:
    case HX711_STAGE_MEASURE_RATE:
    case HX711_STAGE_FIRST_READING:
        report_read_error(err, STR_LOADCELL_BRINGING_UP_FAILED);
        break;
    case HX711_STAGE_SET_SCALE:
        STRRES_ERROR(STR_LOADCELL_SCALE_SET_FAILED, scale_wanted);
        break;
    case HX711_STAGE_PINS:
    case HX711_STAGE_SETTLE:
    case HX711_STAGE_NONE:
        STRRES_ERROR(STR_LOADCELL_BRINGUP_FAILED, esp_err_to_name(err));
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
        STRRES_ERROR(STR_LOADCELL_PINS_NUMERIC);
        return -1;
    }

    hx711_bringup_opts_t opts = {.mode = HX711_MODE_A128};
    for (int i = 3; i < argc; i++) {
        if (strcasecmp(argv[i], "gain") == 0) {
            if (++i >= argc) {
                STRRES_ERROR(STR_LOADCELL_INIT_GAIN_MISSING);
                return -1;
            }
            if (parse_gain_arg(argv[i], &opts.mode) < 0) {
                return -1;
            }
        } else if (strcasecmp(argv[i], "scale") == 0) {
            if (++i >= argc) {
                STRRES_ERROR(STR_LOADCELL_INIT_SCALE_MISSING);
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
        STRRES_ERROR(STR_LOADCELL_GPIO_ABSENT, dout);
        return -1;
    }

    strres_id_t why = STRRES_ID_NONE;
    if (!app_pin_is_drivable(sck, &why)) {
        char reason[APP_STR_LEN];
        STRRES_ERROR(STR_LOADCELL_SCK_UNUSABLE, sck,
                     app_str(why, reason, sizeof(reason)));
        return -1;
    }

    if (dout == sck) {
        STRRES_ERROR(STR_LOADCELL_PINS_DISTINCT, dout);
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
    STRRES_PRINTF(STR_LOADCELL_WAITING_FIRST);
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

    STRRES_PRINTF(STR_LOADCELL_RESPONDING, report.dout_gpio, report.sck_gpio);
    print_mode_of(report.mode);
    if (report.rate_measured) {
        report_rate(report.sps);
    }
    STRRES_PRINTF(STR_LOADCELL_IDLE_READING,
                  report.first_reading.mean,
                  (long)(report.first_reading.max - report.first_reading.min));

    const hx711_scale_t *scale = hx711_get_scale(hx);
    if (scale->supplied) {
        STRRES_PRINTF(STR_LOADCELL_SCALE_SUPPLIED, scale->counts_per_unit);
    }
    if (scale->supplied) {
        STRRES_PRINTF(STR_LOADCELL_NEXT_STEP_WEIGH);
    } else {
        STRRES_PRINTF(STR_LOADCELL_NEXT_STEP_CALIBRATE);
    }
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
        STRRES_PRINTF(STR_LOADCELL_GAIN_32_SWITCHES_CHANNEL);
    }
    warn_if_channel_b(requested);

    announce_settling();

    hx711_change_report_t change = {0};
    esp_err_t err = hx711_set_mode(hx, requested, &change);
    if (err != ESP_OK) {
        report_read_error(err, STR_LOADCELL_SETTLE_GAIN_FAILED);
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
        STRRES_ERROR(STR_LOADCELL_INPUT_INVALID);
        return -1;
    }

    if (channel == HX711_CHANNEL_B) {
        warn_if_channel_b(HX711_MODE_B32);
    }

    announce_settling();

    hx711_change_report_t change = {0};
    esp_err_t err = hx711_set_channel(hx, channel, &change);
    if (err != ESP_OK) {
        report_read_error(err, STR_LOADCELL_SETTLE_CHANNEL_FAILED);
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
        STRRES_PRINTF(STR_LOADCELL_POWER_IS, hx711_is_powered_down(hx) ? "down" : "up");
        return 0;
    }

    hx711_status_t status = {0};
    hx711_get_status(hx, &status);

    if (strcasecmp(argv[1], "off") == 0) {
        if (hx711_is_powered_down(hx)) {
            STRRES_PRINTF(STR_LOADCELL_ALREADY_DOWN);
            return 0;
        }
        hx711_power_down(hx);
        STRRES_PRINTF(STR_LOADCELL_POWER_DOWN_NOTE, status.sck_gpio, HX711_POWERDOWN_US);
        return 0;
    }

    if (strcasecmp(argv[1], "on") == 0) {
        if (!hx711_is_powered_down(hx)) {
            STRRES_PRINTF(STR_LOADCELL_ALREADY_UP);
            return 0;
        }
        announce_settling();

        hx711_change_report_t change = {0};
        esp_err_t err = hx711_power_up(hx, &change);
        if (err != ESP_OK) {
            report_read_error(err, STR_LOADCELL_POWER_UP_FAILED);
            return -1;
        }
        report_scale_dropped(&change);
        STRRES_PRINTF(STR_LOADCELL_POWER_UP_NOTE);
        print_mode_of(change.mode);
        return 0;
    }

    STRRES_PRINTF(STR_LOADCELL_USAGE_POWER);
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
            STRRES_ERROR(STR_LOADCELL_SAMPLE_COUNT_RANGE, MAX_SAMPLES);
            return -1;
        }
    }

    hx711_mode_t mode = HX711_MODE_A128;
    hx711_get_mode(hx, &mode);
    STRRES_PRINTF(STR_LOADCELL_RAW_HEADER,
                  channel_char(mode), hx711_mode_gain(mode));

    for (int i = 0; i < samples; i++) {
        int32_t value = 0;
        esp_err_t err = hx711_read_raw(hx, &value);
        if (err != ESP_OK) {
            report_read_error(err, STR_LOADCELL_CONVERSION_READ_FAILED);
            STRRES_ERROR(STR_LOADCELL_SAMPLE_FAILED, i + 1);
            return -1;
        }
        STRRES_PRINTF(STR_LOADCELL_RAW_ROW,
                      (long)value, value * 100.0 / HX711_FULL_SCALE);
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
    STRRES_PRINTF(STR_LOADCELL_READ_RAW,
                  stats.mean, samples, (long)(stats.max - stats.min),
                  stats.mean * 100.0 / HX711_FULL_SCALE);

    const hx711_scale_t *scale = hx711_get_scale(hx);

    if (scale->tare_samples > 0) {
        STRRES_PRINTF(STR_LOADCELL_READ_NET, stats.mean - scale->tare_counts);
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
            STRRES_PRINTF(STR_LOADCELL_READ_UNITS, weight.units);
        } else {
            STRRES_PRINTF(STR_LOADCELL_READ_UNITS_PM,
                          weight.units, weight.uncertainty_units);
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
    STRRES_PRINTF(STR_LOADCELL_TARE_SET,
                  (long)scale->tare_counts, samples, (long)(stats.max - stats.min),
                  stats.stderr_mean);
    return 0;
}

int cmd_hx711_calibrate(int argc, char **argv)
{
    if (!require_ready()) {
        return -1;
    }

    if (argc < 2) {
        STRRES_PRINTF(STR_LOADCELL_USAGE_CALIBRATE);
        STRRES_PRINTF(STR_LOADCELL_CALIBRATE_HOW);
        return -1;
    }

    double known = 0.0;
    if (cli_parse_double_arg(argv[1], &known) < 0 || known == 0.0) {
        STRRES_ERROR(STR_LOADCELL_MASS_INVALID);
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
        STRRES_ERROR(STR_LOADCELL_NEED_TWO_SAMPLES, known);
        return -1;
    }
    if (err == ESP_ERR_HX711_WITHIN_NOISE) {
        STRRES_ERROR(STR_LOADCELL_TARE_DRIFT, result.net_counts, result.uncertainty, known);
        return -1;
    }
    if (err != ESP_OK) {
        report_batch_error(err, &stats, samples);
        return -1;
    }
    warn_if_saturated(&stats);

    STRRES_PRINTF(STR_LOADCELL_CALIBRATED, result.counts_per_unit, result.net_counts, known);
    STRRES_PRINTF(STR_LOADCELL_CALIBRATE_PRECISION,
                  result.precision_percent, result.uncertainty);
    if (result.precision_percent > 1.0) {
        STRRES_PRINTF(STR_LOADCELL_CALIBRATE_MORE_SAMPLES,
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
            STRRES_PRINTF(STR_LOADCELL_NO_SCALE);
            return 0;
        }
        print_scale_provenance(scale->counts_per_unit, scale->supplied);
        print_scale_for_consumer(scale->counts_per_unit, 0.0, false, mode);
        return 0;
    }

    double counts_per_unit = 0.0;
    if (parse_scale_arg(argv[1], &counts_per_unit) < 0) {
        return -1;
    }

    if (hx711_set_scale(hx, counts_per_unit) != ESP_OK) {
        STRRES_ERROR(STR_LOADCELL_SCALE_SET_FAILED, counts_per_unit);
        return -1;
    }

    STRRES_PRINTF(STR_LOADCELL_SCALE_SET, counts_per_unit);

    /*
     * The factor says nothing about where zero is, and the two are separate
     * measurements. Saying so here is cheaper than letting 'loadcell weight' refuse and
     * be the first to mention it.
     */
    if (scale->tare_samples == 0) {
        STRRES_PRINTF(STR_LOADCELL_NO_TARE);
    }

    /*
     * A factor is counts per unit at one gain, and gain and channel are one
     * setting on this part. Worth saying at the moment someone has just typed a
     * number in by hand.
     */
    STRRES_PRINTF(STR_LOADCELL_SCALE_SUPPLIED_NOTE, counts_per_unit);
    return 0;
}

int cmd_hx711_weight(int argc, char **argv)
{
    if (!require_ready()) {
        return -1;
    }

    const hx711_scale_t *scale = hx711_get_scale(hx);

    if (!scale->calibrated) {
        STRRES_ERROR(STR_LOADCELL_NOT_CALIBRATED);
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
        STRRES_ERROR(STR_LOADCELL_TARE_MISSING);
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
        STRRES_PRINTF(STR_LOADCELL_WEIGHT_ONE_SAMPLE, weight.units, weight.net_counts);
        return 0;
    }

    STRRES_PRINTF(STR_LOADCELL_WEIGHT,
                  weight.units, weight.uncertainty_units, samples, weight.spread_units,
                  weight.net_counts);
    return 0;
}

int cmd_hx711_close(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!hx || !hx711_is_ready(hx)) {
        STRRES_PRINTF(STR_LOADCELL_NOTHING_TO_RELEASE);
        return 0;
    }

    /* hx711_delete() leaves the clock low on the way out, and releases PD_SCK
     * pulled down rather than in its reset state: a pin left high powers the
     * part down sixty microseconds later, and the next session would find a
     * device that had silently reset. */
    hx711_delete(hx);
    hx = NULL;

    STRRES_PRINTF(STR_LOADCELL_RELEASED);
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

    STRRES_PRINTF(STR_LOADCELL_INFO_PART);
    STRRES_PRINTF(STR_LOADCELL_INFO_SETTINGS_HEADER);
    for (size_t i = 0; i < sizeof(hx711_modes) / sizeof(hx711_modes[0]); i++) {
        STRRES_PRINTF(STR_LOADCELL_INFO_SETTINGS_ROW,
                      hx711_mode_pulses(hx711_modes[i]), channel_char(hx711_modes[i]),
                      hx711_mode_gain(hx711_modes[i]));
    }
    STRRES_PRINTF(STR_LOADCELL_INFO_CODING);

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
    STRRES_PRINTF(STR_LOADCELL_INFO_INPUT_RANGE,
                  gain, 0.5 * 5000.0 / gain, 0.5 * 3300.0 / gain);
    STRRES_PRINTF(STR_LOADCELL_INFO_COMMON_MODE);

    if (!status.ready) {
        STRRES_PRINTF(STR_LOADCELL_STATUS_NOT_INITIALIZED);
        return 0;
    }

    /* The datasheet says nothing about what DOUT does while the part is powered
     * down, so do not report the level as if it meant something. */
    if (status.powered_down) {
        STRRES_PRINTF(STR_LOADCELL_STATUS_DOUT_POWERED_DOWN, status.dout_gpio);
    } else if (status.dout_level) {
        STRRES_PRINTF(STR_LOADCELL_STATUS_DOUT_HIGH, status.dout_gpio);
    } else {
        STRRES_PRINTF(STR_LOADCELL_STATUS_DOUT_LOW, status.dout_gpio);
    }
    STRRES_PRINTF(STR_LOADCELL_STATUS_SCK,
                  status.sck_gpio, status.sck_level ? "high" : "low");
    STRRES_PRINTF(STR_LOADCELL_STATUS_POWER, status.powered_down ? "down" : "up");
    STRRES_PRINTF(STR_LOADCELL_STATUS_SESSION_PREFIX);
    print_mode_of(status.mode);

    bool reached = true;
    if (status.powered_down) {
        STRRES_PRINTF(STR_LOADCELL_STATUS_RATE_POWERED_DOWN);
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
            report_read_error(err, STR_LOADCELL_RATE_MEASURE_FAILED);
            reached = false;
        }
    }

    /* Re-read: the rate measurement above may have moved the timing figures. */
    hx711_get_status(hx, &status);

    STRRES_PRINTF(STR_LOADCELL_STATUS_WORST_HIGH,
                  (unsigned long)status.worst_high_us, HX711_T3_MAX_US);
    if (status.stretched_bursts > 0) {
        STRRES_PRINTF(STR_LOADCELL_STATUS_BURSTS_DISCARDED,
                      (unsigned long)status.stretched_bursts);
    }
    diag_printf("\n");

    STRRES_PRINTF(STR_LOADCELL_STATUS_TARE,
                  (long)status.scale.tare_counts,
                  status.scale.calibrated ? "calibrated" : "not calibrated");
    if (status.scale.calibrated) {
        print_scale_provenance(status.scale.counts_per_unit,
                               status.scale.supplied);
        print_scale_for_consumer(status.scale.counts_per_unit, 0.0, false,
                                 status.mode);
        /*
         * The +/- on a weight has always been this session's noise propagated
         * through a factor treated as exact. That is easy to misread as
         * accuracy, and a supplied factor is where it would mislead most;
         * docs/loadcell.md carries the rest of the reasoning.
         */
        if (status.scale.supplied) {
            STRRES_PRINTF(STR_LOADCELL_WEIGHT_PRECISION_NOTE);
        }
        if (status.scale.tare_samples == 0) {
            STRRES_PRINTF(STR_LOADCELL_NO_TARE);
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
    STRRES_PRINTF(STR_LOADCELL_RESET_NOTE);
    return reached ? 0 : -1;
}
