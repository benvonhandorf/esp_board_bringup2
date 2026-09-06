#include "app_bringup.h"
#include "gpio.h"

#include "driver/gpio.h"
#include "esp_cpu.h"
#include "esp_private/esp_clk.h"
#include "esp_private/esp_gpio_reserve.h"
#include "esp_rom_sys.h"
#include "soc/gpio_reg.h"
#include "soc/io_mux_reg.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"

#include <math.h>

#define DEFAULT_BLINK_PERIOD_MS 500
#define MAX_BLINK_COUNT 1000

/* One handle per ADC unit, created on first use. Channels are configured
 * lazily as pins are requested; configuring channel 0 once and then reading
 * whichever channel a pin maps to (as the previous version did) returns
 * garbage for every pin except the one on channel 0. */
static adc_oneshot_unit_handle_t adc_units[SOC_ADC_PERIPH_NUM];
static adc_cali_handle_t adc_cali[SOC_ADC_PERIPH_NUM];
static uint32_t adc_configured_channels[SOC_ADC_PERIPH_NUM];

static bool check_pin(int pin, bool need_output)
{
    if (!GPIO_IS_VALID_GPIO(pin)) {
        STRRES_ERROR(STR_GPIO_PIN_ABSENT, pin);
        return false;
    }
    if (need_output && !GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
        STRRES_ERROR(STR_GPIO_PIN_INPUT_ONLY, pin);
        return false;
    }
    return true;
}

/*
 * Which internal pull, if any, to hold an input at while reading it.
 *
 * Without one a read cannot tell a pin that is driven low from a pin that is
 * not connected to anything: both tend to read 0. Enabling the pull-up makes
 * that distinction, which is the difference between "this net is shorted to
 * ground" and "this net is floating" during bringup.
 */
typedef enum {
    PULL_NONE,
    PULL_UP,
    PULL_DOWN,
} pin_pull_t;

static esp_err_t configure_pin_pull(int pin, gpio_mode_t mode, pin_pull_t pull)
{
    const gpio_config_t config = {
        .pin_bit_mask = BIT64(pin),
        .mode = mode,
        .pull_up_en = (pull == PULL_UP) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = (pull == PULL_DOWN) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&config);
}

static esp_err_t configure_pin(int pin, gpio_mode_t mode)
{
    return configure_pin_pull(pin, mode, PULL_NONE);
}

int cmd_gpio_set(int argc, char **argv)
{
    if (argc < 3) {
        STRRES_PRINTF(STR_GPIO_USAGE_SET);
        return -1;
    }

    int state;
    if (strcasecmp(argv[2], "true") == 0 || strcmp(argv[2], "1") == 0 ||
        strcasecmp(argv[2], "high") == 0) {
        state = 1;
    } else if (strcasecmp(argv[2], "false") == 0 || strcmp(argv[2], "0") == 0 ||
               strcasecmp(argv[2], "low") == 0) {
        state = 0;
    } else {
        STRRES_ERROR(STR_GPIO_STATE_INVALID, argv[2]);
        return -1;
    }

    int *pins = NULL;
    int count = 0;
    if (cli_parse_pin_list(argv[1], &pins, &count) < 0) {
        STRRES_ERROR(STR_GPIO_PIN_SPEC_INVALID, argv[1]);
        return -1;
    }

    int failures = 0;
    for (int i = 0; i < count; i++) {
        int pin = pins[i];

        if (!check_pin(pin, true)) {
            failures++;
            continue;
        }

        esp_err_t err = configure_pin(pin, GPIO_MODE_OUTPUT);
        if (err != ESP_OK) {
            STRRES_ERROR(STR_GPIO_CONFIGURE_FAILED, pin, esp_err_to_name(err));
            failures++;
            continue;
        }

        gpio_set_level(pin, state);
        diag_printf("%d: %s\n", pin, state ? "high" : "low");
    }

    free(pins);
    return failures ? -1 : 0;
}

int cmd_gpio_read(int argc, char **argv)
{
    if (argc < 2) {
        STRRES_PRINTF(STR_GPIO_USAGE_READ);
        return -1;
    }

    /*
     * The optional pull defaults to none, which is what a bare `read` has always
     * done. `up` is the useful one during bringup: a pin that still reads 0 with
     * the internal pull-up fighting it is genuinely being held low, whereas a
     * floating pin goes high.
     */
    pin_pull_t pull = PULL_NONE;
    if (argc > 2) {
        if (strcasecmp(argv[2], "up") == 0) {
            pull = PULL_UP;
        } else if (strcasecmp(argv[2], "down") == 0) {
            pull = PULL_DOWN;
        } else if (strcasecmp(argv[2], "none") == 0) {
            pull = PULL_NONE;
        } else {
            STRRES_ERROR(STR_GPIO_PULL_INVALID, argv[2]);
            return -1;
        }
    }

    int *pins = NULL;
    int count = 0;
    if (cli_parse_pin_list(argv[1], &pins, &count) < 0) {
        STRRES_ERROR(STR_GPIO_PIN_SPEC_INVALID, argv[1]);
        return -1;
    }

    int failures = 0;
    for (int i = 0; i < count; i++) {
        int pin = pins[i];

        if (!check_pin(pin, false)) {
            failures++;
            continue;
        }

        esp_err_t err = configure_pin_pull(pin, GPIO_MODE_INPUT, pull);
        if (err != ESP_OK) {
            STRRES_ERROR(STR_GPIO_CONFIGURE_FAILED, pin, esp_err_to_name(err));
            failures++;
            continue;
        }

        /* docs/gpio.md sample output: "<pin>: <level>", one pin per row. */
        diag_printf("%d: %d\n", pin, gpio_get_level(pin));
    }

    free(pins);
    return failures ? -1 : 0;
}

/* Bring up the unit handle, the channel, and (if available) calibration. */
static esp_err_t prepare_adc(adc_unit_t unit, adc_channel_t channel)
{
    if (unit >= SOC_ADC_PERIPH_NUM) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!adc_units[unit]) {
        const adc_oneshot_unit_init_cfg_t init_config = {.unit_id = unit};
        esp_err_t err = adc_oneshot_new_unit(&init_config, &adc_units[unit]);
        if (err != ESP_OK) {
            adc_units[unit] = NULL;
            return err;
        }
    }

    if (!(adc_configured_channels[unit] & BIT(channel))) {
        const adc_oneshot_chan_cfg_t config = {
            .atten = ADC_ATTEN_DB_12, /* widest input range: roughly 0-3.1V */
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        esp_err_t err = adc_oneshot_config_channel(adc_units[unit], channel, &config);
        if (err != ESP_OK) {
            return err;
        }
        adc_configured_channels[unit] |= BIT(channel);
    }

    /* Calibration turns raw counts into millivolts. Not every chip ships with
     * the required eFuse data, so a failure here is not fatal. */
    if (!adc_cali[unit]) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        const adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = unit,
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        adc_cali_create_scheme_curve_fitting(&cali_config, &adc_cali[unit]);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
        const adc_cali_line_fitting_config_t cali_config = {
            .unit_id = unit,
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        adc_cali_create_scheme_line_fitting(&cali_config, &adc_cali[unit]);
#endif
    }

    return ESP_OK;
}

int cmd_gpio_aread(int argc, char **argv)
{
    if (argc < 2) {
        STRRES_PRINTF(STR_GPIO_USAGE_AREAD);
        return -1;
    }

    int *pins = NULL;
    int count = 0;
    if (cli_parse_pin_list(argv[1], &pins, &count) < 0) {
        STRRES_ERROR(STR_GPIO_PIN_SPEC_INVALID, argv[1]);
        return -1;
    }

    int failures = 0;
    for (int i = 0; i < count; i++) {
        int pin = pins[i];

        adc_unit_t unit;
        adc_channel_t channel;
        if (adc_oneshot_io_to_channel(pin, &unit, &channel) != ESP_OK) {
            STRRES_ERROR(STR_GPIO_PIN_NOT_ADC, pin);
            failures++;
            continue;
        }

        esp_err_t err = prepare_adc(unit, channel);
        if (err != ESP_OK) {
            STRRES_ERROR(STR_GPIO_ADC_PREPARE_FAILED,
                         unit + 1, channel, pin, esp_err_to_name(err));
            failures++;
            continue;
        }

        int raw = 0;
        err = adc_oneshot_read(adc_units[unit], channel, &raw);
        if (err != ESP_OK) {
            STRRES_ERROR(STR_GPIO_ADC_READ_FAILED, pin, esp_err_to_name(err));
            failures++;
            continue;
        }

        int millivolts = 0;
        if (adc_cali[unit] &&
            adc_cali_raw_to_voltage(adc_cali[unit], raw, &millivolts) == ESP_OK) {
            STRRES_PRINTF(STR_GPIO_AREAD_CALIBRATED, pin, raw, millivolts, unit + 1, channel);
        } else {
            STRRES_PRINTF(STR_GPIO_AREAD_UNCALIBRATED, pin, raw, unit + 1, channel);
        }
    }

    free(pins);
    return failures ? -1 : 0;
}

int cmd_gpio_blink(int argc, char **argv)
{
    if (argc < 3) {
        STRRES_PRINTF(STR_GPIO_USAGE_BLINK);
        return -1;
    }

    int count;
    if (cli_parse_int_arg(argv[2], &count) < 0 || count < 1 || count > MAX_BLINK_COUNT) {
        STRRES_ERROR(STR_GPIO_COUNT_RANGE, MAX_BLINK_COUNT);
        return -1;
    }

    int period = DEFAULT_BLINK_PERIOD_MS;
    if (argc > 3 && (cli_parse_int_arg(argv[3], &period) < 0 || period < 10 || period > 10000)) {
        STRRES_ERROR(STR_GPIO_PERIOD_RANGE);
        return -1;
    }

    int *pins = NULL;
    int pin_count = 0;
    if (cli_parse_pin_list(argv[1], &pins, &pin_count) < 0) {
        STRRES_ERROR(STR_GPIO_PIN_SPEC_INVALID, argv[1]);
        return -1;
    }

    /* Configure everything up front so a bad pin fails before any blinking. */
    for (int i = 0; i < pin_count; i++) {
        if (!check_pin(pins[i], true)) {
            free(pins);
            return -1;
        }
        esp_err_t err = configure_pin(pins[i], GPIO_MODE_OUTPUT);
        if (err != ESP_OK) {
            STRRES_ERROR(STR_GPIO_CONFIGURE_FAILED, pins[i], esp_err_to_name(err));
            free(pins);
            return -1;
        }
    }

    STRRES_PRINTF(STR_GPIO_BLINKING, pin_count, count, period);

    for (int i = 0; i < count; i++) {
        for (int level = 1; level >= 0; level--) {
            for (int p = 0; p < pin_count; p++) {
                gpio_set_level(pins[p], level);
            }
            /* vTaskDelay yields, so the rest of the system keeps running. */
            vTaskDelay(pdMS_TO_TICKS(period / 2));
        }
    }

    STRRES_PRINTF(STR_GPIO_BLINK_COMPLETE);
    free(pins);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Short detection                                                     */
/* ------------------------------------------------------------------ */

/*
 * Settling time after changing a pin before reading its neighbours.
 *
 * The observers are held by the internal pull-up, which is weak -- tens of
 * kiloohms against whatever capacitance the board has. 45k into 100pF is a few
 * microseconds, so 200us is generous without making a 64-pin scan slow.
 */
#define SHORT_SETTLE_US 200

/*
 * Pins the scan must never drive.
 *
 * Driving a flash or PSRAM pin hangs the chip instantly, and driving the console
 * pins cuts off the very connection the results are printed to. ESP-IDF already
 * tracks the first group: spi_flash and esp_psram reserve their pins at startup,
 * so esp_gpio_is_reserved() catches them along with any peripheral currently
 * holding a pin. The console pins it does not track, so they are named here.
 */
bool app_pin_is_drivable(int pin, strres_id_t *why)
{
    if (!GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
        *why = STR_GPIO_WHY_INPUT_ONLY;
        return false;
    }
    if (esp_gpio_is_reserved(BIT64(pin))) {
        *why = STR_GPIO_WHY_RESERVED;
        return false;
    }
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG && defined(USB_INT_PHY0_DM_GPIO_NUM)
    if (pin == USB_INT_PHY0_DM_GPIO_NUM || pin == USB_INT_PHY0_DP_GPIO_NUM) {
        *why = STR_GPIO_WHY_USB_CONSOLE;
        return false;
    }
#endif
#ifdef CONFIG_ESP_CONSOLE_UART_TX_GPIO
    if (pin == CONFIG_ESP_CONSOLE_UART_TX_GPIO || pin == CONFIG_ESP_CONSOLE_UART_RX_GPIO) {
        *why = STR_GPIO_WHY_CONSOLE_UART;
        return false;
    }
#endif
    return true;
}

/* Park every pin in the set as a pulled-up input: the state observers need. */
static void park_as_observers(const int *pins, int count)
{
    for (int i = 0; i < count; i++) {
        configure_pin_pull(pins[i], GPIO_MODE_INPUT, PULL_UP);
    }
}

/*
 * Drive pins[driven] low and record which of the others follow it down.
 * `mask` gets a bit set per observer index that read low.
 */
static void probe_from(const int *pins, int count, int driven, uint64_t *mask)
{
    configure_pin_pull(pins[driven], GPIO_MODE_OUTPUT, PULL_NONE);
    gpio_set_level(pins[driven], 0);
    esp_rom_delay_us(SHORT_SETTLE_US);

    for (int i = 0; i < count; i++) {
        if (i != driven && gpio_get_level(pins[i]) == 0) {
            *mask |= BIT64(i);
        }
    }

    /* Back to an observer before moving on, so only one pin is ever driven. */
    configure_pin_pull(pins[driven], GPIO_MODE_INPUT, PULL_UP);
    esp_rom_delay_us(SHORT_SETTLE_US);
}

int cmd_gpio_short(int argc, char **argv)
{
    if (argc < 2) {
        STRRES_PRINTF(STR_GPIO_USAGE_SHORT);
        return -1;
    }

    int *requested = NULL;
    int requested_count = 0;
    if (cli_parse_pin_list(argv[1], &requested, &requested_count) < 0) {
        STRRES_ERROR(STR_GPIO_PIN_SPEC_INVALID, argv[1]);
        return -1;
    }

    /* Drop anything unsafe to drive, saying which and why rather than
     * silently testing a smaller set than was asked for. */
    int *pins = calloc((size_t)requested_count, sizeof(int));
    if (!pins) {
        STRRES_ERROR(STR_GPIO_OUT_OF_MEMORY);
        free(requested);
        return -1;
    }
    int count = 0;
    for (int i = 0; i < requested_count; i++) {
        strres_id_t why = STRRES_ID_NONE;
        bool duplicate = false;
        for (int j = 0; j < count; j++) {
            duplicate = duplicate || (pins[j] == requested[i]);
        }

        if (duplicate) {
            /* Without this a repeated pin is compared against itself, follows
             * itself down, and gets reported as shorted to itself. */
            STRRES_PRINTF(STR_GPIO_SKIP_DUPLICATE, requested[i]);
        } else if (!GPIO_IS_VALID_GPIO(requested[i])) {
            STRRES_PRINTF(STR_GPIO_SKIP_ABSENT, requested[i]);
        } else if (!app_pin_is_drivable(requested[i], &why)) {
            char reason[APP_STR_LEN];
            STRRES_PRINTF(STR_GPIO_SKIP_REASON, requested[i],
                          app_str(why, reason, sizeof(reason)));
        } else {
            pins[count++] = requested[i];
        }
    }
    free(requested);

    if (count < 2) {
        STRRES_ERROR(STR_GPIO_NEED_TWO_PINS, count);
        free(pins);
        return -1;
    }

    uint64_t *observed = calloc((size_t)count, sizeof(uint64_t));
    if (!observed) {
        STRRES_ERROR(STR_GPIO_OUT_OF_MEMORY);
        free(pins);
        return -1;
    }

    STRRES_PRINTF(STR_GPIO_SHORT_SCAN_INTRO, count);

    park_as_observers(pins, count);

    /*
     * A pin already sitting low fights nothing and follows everything, so it
     * would otherwise be reported as shorted to every other pin. Find those
     * first and leave them out of the pairing.
     */
    uint64_t stuck_low = 0;
    esp_rom_delay_us(SHORT_SETTLE_US);
    for (int i = 0; i < count; i++) {
        if (gpio_get_level(pins[i]) == 0) {
            stuck_low |= BIT64(i);
        }
    }

    for (int i = 0; i < count; i++) {
        if (!(stuck_low & BIT64(i))) {
            probe_from(pins, count, i, &observed[i]);
        }
    }

    /* A real short is symmetric, and neither end may be a stuck-low pin. */
    #define SHORTED(a, b) (!(stuck_low & BIT64(a)) && !(stuck_low & BIT64(b)) && \
                           (observed[a] & BIT64(b)) && (observed[b] & BIT64(a)))

    /*
     * Adjacent pins first. A solder bridge at the package is by far the most
     * common way two nets get tied together, and neighbouring GPIO numbers are
     * usually neighbouring pads -- so this is the answer worth seeing first.
     */
    int adjacent_pairs = 0, adjacent_shorts = 0;
    STRRES_PRINTF(STR_GPIO_SHORT_ADJACENT_HEADER);
    for (int a = 0; a < count; a++) {
        for (int b = a + 1; b < count; b++) {
            if (pins[b] - pins[a] != 1) {
                continue;
            }
            adjacent_pairs++;
            if (SHORTED(a, b)) {
                STRRES_PRINTF(STR_GPIO_SHORT_PAIR, pins[a], pins[b]);
                adjacent_shorts++;
            }
        }
    }
    if (adjacent_pairs == 0) {
        STRRES_PRINTF(STR_GPIO_SHORT_NO_ADJACENT);
    } else if (adjacent_shorts == 0) {
        STRRES_PRINTF(STR_GPIO_SHORT_PAIRS_CLEAR,
                      adjacent_pairs, adjacent_pairs == 1 ? "" : "s");
    }

    int other_pairs = 0, other_shorts = 0;
    STRRES_PRINTF(STR_GPIO_SHORT_REMAINING_HEADER);
    for (int a = 0; a < count; a++) {
        for (int b = a + 1; b < count; b++) {
            if (pins[b] - pins[a] == 1) {
                continue;
            }
            other_pairs++;
            if (SHORTED(a, b)) {
                STRRES_PRINTF(STR_GPIO_SHORT_PAIR, pins[a], pins[b]);
                other_shorts++;
            }
        }
    }
    if (other_pairs == 0) {
        STRRES_PRINTF(STR_GPIO_SHORT_NONE_TO_TEST);
    } else if (other_shorts == 0) {
        STRRES_PRINTF(STR_GPIO_SHORT_PAIRS_CLEAR, other_pairs, other_pairs == 1 ? "" : "s");
    }

    if (stuck_low) {
        STRRES_PRINTF(STR_GPIO_SHORT_HELD_LOW_HEADER);
        for (int i = 0; i < count; i++) {
            if (stuck_low & BIT64(i)) {
                diag_printf(" GPIO %d", pins[i]);
            }
        }
        STRRES_PRINTF(STR_GPIO_SHORT_HELD_LOW_NOTE);
    }

    #undef SHORTED

    /* Leave the pins passive rather than holding pull-ups on someone's bus. */
    for (int i = 0; i < count; i++) {
        configure_pin_pull(pins[i], GPIO_MODE_INPUT, PULL_NONE);
    }

    const int total_shorts = adjacent_shorts + other_shorts;
    STRRES_PRINTF(STR_GPIO_SHORT_FOUND, total_shorts, total_shorts == 1 ? "" : "s");

    free(observed);
    free(pins);
    return total_shorts ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* Pull-up and capacitance measurement                                 */
/* ------------------------------------------------------------------ */

/*
 * `gpio short` answers "are these two nets the same net". It cannot answer
 * "is this net pulled up, how hard, and how much is hanging off it" -- and a
 * logic read cannot either, because a 10k pull-up to a healthy rail and a 10k
 * pull-up whose far end is floating both read as 1.
 *
 * Releasing a pin that was driven low and timing the rise separates them. The
 * net charges through whatever pulls it up, so the rise time is R*C. Doing it
 * twice, once with the internal pull-up added in parallel, gives two equations
 * for the two unknowns, using the internal pull-up as an on-chip reference:
 *
 *   t_ext = k * R_ext * C            t_par = k * (R_ext || R_int) * C
 *   t_ext / t_par = (R_ext + R_int) / R_int
 *
 * so R_ext = R_int * (t_ext / t_par - 1), and C follows. No meter needed.
 *
 * The internal pull-up is only nominally 45k and varies a lot with process and
 * temperature, so both results carry that error and are worth about a factor of
 * two. That is plenty: the cases this is built to tell apart differ by orders
 * of magnitude, not percent.
 */

/* Datasheet nominal for the internal pull-up. Wide tolerance -- see above. */
#define RC_R_INTERNAL 45000.0

/*
 * An RC net reaches the ~0.75*VDD input threshold after ln(4) time constants,
 * so the measured rise is 1.386*tau.
 */
#define RC_THRESHOLD_TAUS 1.386

/* Long enough to fully discharge the net through the output driver: 1ms is
 * about 25 time constants even for a microfarad behind 40 ohms. */
#define RC_DISCHARGE_US 1000

/* Give up after this long. It also bounds how long interrupts stay masked.
 * Reaching it means either the net is held low or it carries tens of nF. */
#define RC_TIMEOUT_US 5000

static portMUX_TYPE rc_lock = portMUX_INITIALIZER_UNLOCKED;

/*
 * Bare-register pad access.
 *
 * Timing the rise by polling gpio_get_level() in a loop only resolves one loop
 * iteration, which measured out at ~290ns here -- the same order as the rise
 * being measured, so every net came back quantised to the same two values.
 * These reduce the timed path to single register accesses.
 */
static inline void pad_write(int pin, int level)
{
#ifdef GPIO_OUT1_W1TS_REG
    if (pin >= 32) {
        REG_WRITE(level ? GPIO_OUT1_W1TS_REG : GPIO_OUT1_W1TC_REG, BIT(pin - 32));
        return;
    }
#endif
    REG_WRITE(level ? GPIO_OUT_W1TS_REG : GPIO_OUT_W1TC_REG, BIT(pin));
}

static inline int pad_level(int pin)
{
#ifdef GPIO_IN1_REG
    if (pin >= 32) {
        return (REG_READ(GPIO_IN1_REG) >> (pin - 32)) & 1;
    }
#endif
    return (REG_READ(GPIO_IN_REG) >> pin) & 1;
}

/*
 * Discharge the net, release the pin, and sample it once exactly `delay` CPU
 * cycles later. One sample per call: sampling at a chosen instant costs no
 * time inside the measurement, where a polling loop costs an iteration.
 *
 * In open-drain mode writing 1 releases the pad to the pull-ups. In push-pull
 * it drives high instead, which rises in well under a nanosecond and so
 * measures the fixed cost of the path from the release to the sample.
 */
static int sample_at(int pin, uint32_t delay)
{
    pad_write(pin, 0);
    esp_rom_delay_us(RC_DISCHARGE_US);

    portENTER_CRITICAL(&rc_lock);
    const uint32_t t0 = esp_cpu_get_cycle_count();
    pad_write(pin, 1);
    while (esp_cpu_get_cycle_count() - t0 < delay) {
        /* spin */
    }
    const int level = pad_level(pin);
    portEXIT_CRITICAL(&rc_lock);
    return level;
}

/*
 * Smallest delay, in CPU cycles, at which the pin reads high after release,
 * or UINT32_MAX if it never does. Binary search: the rise is monotonic, so
 * ~20 samples pin it down regardless of the range.
 */
static uint32_t find_crossing(int pin, uint32_t limit)
{
    if (!sample_at(pin, limit)) {
        return UINT32_MAX;
    }
    if (sample_at(pin, 0)) {
        return 0;
    }

    uint32_t lo = 0, hi = limit;
    while (hi - lo > 1) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (sample_at(pin, mid)) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    return hi;
}

/*
 * Rise time of `pin` in CPU cycles with the given internal pull, or
 * UINT32_MAX if it never gets high. `overhead` is subtracted out.
 */
static uint32_t measure_rise(int pin, pin_pull_t pull, uint32_t limit, uint32_t overhead)
{
    configure_pin_pull(pin, GPIO_MODE_INPUT_OUTPUT_OD, pull);
    const uint32_t crossing = find_crossing(pin, limit);
    if (crossing == UINT32_MAX) {
        return UINT32_MAX;
    }
    return crossing > overhead ? crossing - overhead : 0;
}

/* Fixed cost of the release-to-sample path, measured by driving the pin high
 * push-pull so that the net itself contributes nothing. */
static uint32_t measure_overhead(int pin, uint32_t limit)
{
    configure_pin_pull(pin, GPIO_MODE_INPUT_OUTPUT, PULL_NONE);
    const uint32_t crossing = find_crossing(pin, limit);
    return crossing == UINT32_MAX ? 0 : crossing;
}

static void print_seconds(double s)
{
    if (s < 1e-6) {
        diag_printf("%7.0f ns", s * 1e9);
    } else if (s < 1e-3) {
        diag_printf("%7.2f us", s * 1e6);
    } else {
        diag_printf("%7.2f ms", s * 1e3);
    }
}

static void print_ohms(double r)
{
    if (r >= 1e6) {
        diag_printf("%6.1f M", r / 1e6);
    } else {
        diag_printf("%6.1f k", r / 1e3);
    }
}

static void print_farads(double c)
{
    if (c < 1e-9) {
        diag_printf("%7.1f pF", c * 1e12);
    } else if (c < 1e-6) {
        diag_printf("%7.2f nF", c * 1e9);
    } else {
        diag_printf("%7.2f uF", c * 1e6);
    }
}

/* Both rise times for one pin, in CPU cycles. UINT32_MAX means it never rose. */
typedef struct {
    uint32_t ext; /* external pull-up only */
    uint32_t par; /* external in parallel with the internal pull-up */
} rc_result_t;

static rc_result_t measure_pin(int pin, uint32_t limit)
{
    const uint32_t overhead = measure_overhead(pin, limit);
    rc_result_t r;
    r.ext = measure_rise(pin, PULL_NONE, limit, overhead);
    r.par = measure_rise(pin, PULL_UP, limit, overhead);
    configure_pin_pull(pin, GPIO_MODE_INPUT, PULL_NONE);
    return r;
}

/*
 * Everything here scales with the internal pull-up, which is the one value that
 * cannot be measured from the inside: the rise-time ratio gives R_ext/R_int, so
 * an error in R_int passes straight through to every result. Naming a pin whose
 * pull-up is known turns that around and solves for R_int instead, which then
 * applies to every other pin on the chip.
 */
static bool calibrate_internal(int argc, char **argv, uint32_t limit, uint32_t cpu_hz,
                               double *r_int)
{
    (void)cpu_hz;
    if (argc < 5 || strcasecmp(argv[2], "ref") != 0) {
        if (argc > 2) {
            STRRES_ERROR(STR_GPIO_RC_REF_EXPECTED);
            return false;
        }
        return true; /* no calibration requested */
    }

    const int ref_pin = atoi(argv[3]);
    const double ref_ohms = strtod(argv[4], NULL) * 1000.0;
    strres_id_t why = STRRES_ID_NONE;
    if (!GPIO_IS_VALID_GPIO(ref_pin)) {
        STRRES_ERROR(STR_GPIO_RC_REF_ABSENT, ref_pin);
        return false;
    }
    if (!app_pin_is_drivable(ref_pin, &why)) {
        char reason[APP_STR_LEN];
        STRRES_ERROR(STR_GPIO_RC_REF_UNDRIVABLE, ref_pin,
                     app_str(why, reason, sizeof(reason)));
        return false;
    }
    if (ref_ohms <= 0) {
        STRRES_ERROR(STR_GPIO_RC_REF_POSITIVE);
        return false;
    }

    const rc_result_t r = measure_pin(ref_pin, limit);
    if (r.ext == UINT32_MAX || r.par == UINT32_MAX || r.par == 0) {
        STRRES_ERROR(STR_GPIO_RC_REF_NOT_PULLED_UP, ref_pin);
        return false;
    }
    const double ratio = (double)r.ext / (double)r.par;
    if (ratio <= 1.05) {
        STRRES_ERROR(STR_GPIO_RC_REF_TOO_FAST, ref_pin);
        return false;
    }

    *r_int = ref_ohms / (ratio - 1.0);
    return true;
}

/* ------------------------------------------------------------------ */
/* Survey                                                              */
/* ------------------------------------------------------------------ */

/*
 * What a pin looks like from the inside, before anything is known about the
 * board. `rc` measures one pin and prints numbers; this asks the same question
 * of every pin at once and says what the numbers mean.
 *
 * The one inference that holds is the pull-up. An I2C bus is the only thing
 * that routinely puts a few kiloohms on a pair of pins, so finding exactly two
 * of them is close to conclusive, and it is worth having because it is the
 * first thing you want to know about an unfamiliar board.
 *
 * The inference that does *not* hold is capacitance. It is tempting to read a
 * few extra picofarads as "something is connected here", and on a real board
 * it does not survive contact: a survey of one carrier put its I2S lines among
 * the pins that looked unremarkable, and a search built on the pins that
 * looked promising found nothing. Capacitance is reported because it separates
 * a bare pad from a routed net, and for no stronger purpose than that.
 */
typedef enum {
    NET_SKIPPED,     /* not ours to drive */
    NET_DRIVEN,      /* something is holding it down */
    NET_BUS,         /* a pull-up strong enough to be deliberate */
    NET_WEAK,        /* a pull-up, but not a bus-strength one */
    NET_FLOATING,    /* nothing pulling it up at all */
} net_kind_t;

/* Below this a pull-up was fitted on purpose. Above it, leakage or a very
 * weak part; the usual I2C values -- 2.2k, 4.7k, 10k -- sit well under. */
#define BUS_PULLUP_MAX_OHMS 15000.0

typedef struct {
    int pin;
    net_kind_t kind;
    double ohms;        /* 0 when unresolvably strong */
    double farads;
    strres_id_t why;    /* NET_SKIPPED only */
} net_t;

static const char *kind_word(const net_t *n)
{
    switch (n->kind) {
    case NET_DRIVEN:   return "driven low";
    case NET_BUS:      return "pull-up";
    case NET_WEAK:     return "weak pull-up";
    case NET_FLOATING: return "floating";
    default:           return "skipped";
    }
}

static void classify(net_t *n, const rc_result_t *r, uint32_t cpu_hz, double r_int)
{
    n->farads = -1.0;   /* negative means "not resolved", not "zero" */

    if (r->par == UINT32_MAX) {
        /* Never rose even with help: either something is holding it, or the
         * net is enormous. Both mean "not an idle input". */
        n->kind = NET_DRIVEN;
        return;
    }

    /*
     * No external pull-up at all. This has to be tested before any conclusion
     * is drawn from a fast rise, because a bare pad with almost no capacitance
     * also rises immediately -- and reading that as a strong pull-up would put
     * every unconnected pin in the bus column.
     */
    if (r->ext == UINT32_MAX) {
        n->kind = NET_FLOATING;
        if (r->par > 0) {
            n->farads = (double)r->par / cpu_hz / RC_THRESHOLD_TAUS / r_int;
        }
        return;
    }

    /*
     * A rise the timer cannot resolve, with an external pull-up present.
     *
     * The internal pull-up alone takes about 150 ns even on a bare pad, so a
     * time of zero means something far stronger is doing the work. Both ends
     * are guarded: a zero denominator here used to produce a NaN ratio, and
     * because every comparison against NaN is false it fell through to the
     * arithmetic below and reported a real I2C bus as a weak pull-up of "nan
     * k" -- which then broke the conclusion drawn from the whole survey.
     */
    if (r->ext == 0 || r->par == 0) {
        n->kind = NET_BUS;
        n->ohms = 0.0;
        return;
    }

    const double t_ext = (double)r->ext / cpu_hz;
    const double t_par = (double)r->par / cpu_hz;
    const double ratio = t_ext / t_par;
    if (!isfinite(ratio) || ratio <= 1.05) {
        n->kind = NET_BUS;
        n->ohms = 0.0;
        return;
    }

    n->ohms = r_int * (ratio - 1.0);
    if (!isfinite(n->ohms) || n->ohms <= 0.0) {
        n->kind = NET_BUS;
        n->ohms = 0.0;
        return;
    }
    n->farads = t_ext / RC_THRESHOLD_TAUS / n->ohms;
    n->kind = (n->ohms <= BUS_PULLUP_MAX_OHMS) ? NET_BUS : NET_WEAK;
}

int cmd_gpio_survey(int argc, char **argv)
{
    int *pins = NULL;
    int count = 0;

    if (argc > 1) {
        if (cli_parse_pin_list(argv[1], &pins, &count) < 0) {
            STRRES_ERROR(STR_GPIO_PIN_SPEC_INVALID, argv[1]);
            return -1;
        }
    } else {
        /* Every pin the chip has. The unsafe ones are filtered below rather
         * than here, so the report can say which were left out and why. */
        pins = calloc(SOC_GPIO_PIN_COUNT, sizeof(int));
        if (!pins) {
            STRRES_ERROR(STR_GPIO_OUT_OF_MEMORY);
            return -1;
        }
        for (int pin = 0; pin < SOC_GPIO_PIN_COUNT; pin++) {
            if (GPIO_IS_VALID_GPIO(pin)) {
                pins[count++] = pin;
            }
        }
    }

    net_t *nets = calloc((size_t)count, sizeof(net_t));
    if (!nets) {
        free(pins);
        STRRES_ERROR(STR_GPIO_OUT_OF_MEMORY);
        return -1;
    }

    const uint32_t cpu_hz = (uint32_t)esp_clk_cpu_freq();
    const uint32_t limit = (uint32_t)((uint64_t)RC_TIMEOUT_US * cpu_hz / 1000000);
    const double r_int = RC_R_INTERNAL;

    STRRES_PRINTF(STR_GPIO_SURVEY_INTRO, count);

    for (int i = 0; i < count; i++) {
        nets[i].pin = pins[i];

        if (!GPIO_IS_VALID_GPIO(pins[i])) {
            nets[i].kind = NET_SKIPPED;
            nets[i].why = STR_GPIO_WHY_ABSENT;
            continue;
        }
        if (!app_pin_is_drivable(pins[i], &nets[i].why)) {
            nets[i].kind = NET_SKIPPED;
            continue;
        }

        const rc_result_t r = measure_pin(pins[i], limit);
        classify(&nets[i], &r, cpu_hz, r_int);
    }
    free(pins);

    /* ---------------- the table ---------------- */

    STRRES_PRINTF(STR_GPIO_SURVEY_HEADER, "state", "pull-up", "net C");
    for (int i = 0; i < count; i++) {
        const net_t *n = &nets[i];
        diag_printf("%4d  %-14s ", n->pin, kind_word(n));

        switch (n->kind) {
        case NET_SKIPPED:
            strres_printf(n->why);
            diag_printf("\n");
            continue;
        case NET_DRIVEN:
            STRRES_PRINTF(STR_GPIO_SURVEY_HELD, "--", "--");
            continue;
        case NET_FLOATING:
            diag_printf("%10s ", "none");
            if (n->farads < 0.0) {
                STRRES_PRINTF(STR_GPIO_SURVEY_TOO_FAST, "--");
            } else {
                print_farads(n->farads);
                diag_printf("\n");
            }
            continue;
        default:
            break;
        }

        if (n->ohms == 0.0) {
            print_ohms(r_int / 20.0);
            STRRES_PRINTF(STR_GPIO_SURVEY_TOO_STRONG, "--");
        } else {
            print_ohms(n->ohms);
            diag_printf("  ");
            if (n->farads < 0.0) {
                diag_printf("%10s", "--");
            } else {
                print_farads(n->farads);
            }
            diag_printf("\n");
        }
    }

    /* ---------------- what it means ---------------- */

    int bus_pins[8];
    int bus_count = 0;
    int driven_count = 0;
    int skipped_count = 0;

    for (int i = 0; i < count; i++) {
        if (nets[i].kind == NET_BUS && bus_count < 8) {
            bus_pins[bus_count++] = nets[i].pin;
        } else if (nets[i].kind == NET_DRIVEN) {
            driven_count++;
        } else if (nets[i].kind == NET_SKIPPED) {
            skipped_count++;
        }
    }

    diag_printf("\n");

    if (bus_count == 2) {
        /* The strong case. Which pin is which is not observable from here, so
         * both orders are offered; only one will find anything. */
        STRRES_PRINTF(STR_GPIO_SURVEY_I2C_PAIR, bus_pins[0], bus_pins[1]);
        STRRES_PRINTF(STR_GPIO_SURVEY_I2C_TRY, bus_pins[0], bus_pins[1]);
        STRRES_PRINTF(STR_GPIO_SURVEY_I2C_TRY_SWAPPED, bus_pins[1], bus_pins[0]);
    } else if (bus_count > 2) {
        STRRES_PRINTF(STR_GPIO_SURVEY_PULLUPS_HEADER);
        for (int i = 0; i < bus_count; i++) {
            diag_printf(" %d", bus_pins[i]);
        }
        STRRES_PRINTF(STR_GPIO_SURVEY_MANY_PULLUPS);
    } else if (bus_count == 1) {
        STRRES_PRINTF(STR_GPIO_SURVEY_ONE_PULLUP, bus_pins[0]);
    } else {
        STRRES_PRINTF(STR_GPIO_SURVEY_NO_PULLUPS);
    }

    if (driven_count) {
        STRRES_PRINTF(STR_GPIO_SURVEY_HELD_SUMMARY,
                      driven_count, driven_count == 1 ? " is" : "s are");
    }
    if (skipped_count) {
        STRRES_PRINTF(STR_GPIO_SURVEY_SKIPPED_SUMMARY,
                      skipped_count, skipped_count == 1 ? " was" : "s were");
    }

    /*
     * Said plainly because the temptation is real and the cost of yielding to
     * it was a wasted search on a board where the answer was in front of me.
     */
    STRRES_PRINTF(STR_GPIO_SURVEY_CAPACITANCE_NOTE);

    free(nets);
    return 0;
}

int cmd_gpio_rc(int argc, char **argv)
{
    if (argc < 2) {
        STRRES_PRINTF(STR_GPIO_USAGE_RC);
        return -1;
    }

    int *pins = NULL;
    int count = 0;
    if (cli_parse_pin_list(argv[1], &pins, &count) < 0) {
        STRRES_ERROR(STR_GPIO_PIN_SPEC_INVALID, argv[1]);
        return -1;
    }

    const uint32_t cpu_hz = (uint32_t)esp_clk_cpu_freq();
    const uint32_t limit_cycles = (uint32_t)((uint64_t)RC_TIMEOUT_US * cpu_hz / 1000000);

    double r_int = RC_R_INTERNAL;
    if (!calibrate_internal(argc, argv, limit_cycles, cpu_hz, &r_int)) {
        free(pins);
        return -1;
    }
    const bool calibrated = (r_int != RC_R_INTERNAL);

    STRRES_PRINTF(STR_GPIO_RC_INTRO);

    if (calibrated) {
        STRRES_PRINTF(STR_GPIO_RC_REFERENCE_MEASURED, r_int / 1000.0, argv[4], argv[3]);
    } else {
        STRRES_PRINTF(STR_GPIO_RC_REFERENCE_ASSUMED, RC_R_INTERNAL / 1000.0);
    }

    STRRES_PRINTF(STR_GPIO_RC_HEADER);

    for (int i = 0; i < count; i++) {
        const int pin = pins[i];
        strres_id_t why = STRRES_ID_NONE;
        if (!GPIO_IS_VALID_GPIO(pin)) {
            STRRES_PRINTF(STR_GPIO_RC_ABSENT, pin);
            continue;
        }
        if (!app_pin_is_drivable(pin, &why)) {
            char reason[APP_STR_LEN];
            STRRES_PRINTF(STR_GPIO_RC_SKIPPED, pin,
                          app_str(why, reason, sizeof(reason)));
            continue;
        }

        const rc_result_t r = measure_pin(pin, limit_cycles);

        diag_printf("%4d  ", pin);

        if (r.par == UINT32_MAX) {
            /* Not even the internal pull-up gets it there in the time allowed. */
            STRRES_PRINTF(STR_GPIO_RC_HELD_LOW);
            continue;
        }

        const double t_par = (double)r.par / cpu_hz;
        if (r.ext == UINT32_MAX) {
            diag_printf("      none  ");
            print_seconds(t_par);
            diag_printf("       none  ");
            print_farads(t_par / RC_THRESHOLD_TAUS / r_int);
            STRRES_PRINTF(STR_GPIO_RC_NO_EXTERNAL);
            continue;
        }

        const double t_ext = (double)r.ext / cpu_hz;
        print_seconds(t_ext);
        diag_printf("  ");
        print_seconds(t_par);
        diag_printf("  ");

        /*
         * Adding the internal pull-up to a net that already has one must speed
         * it up. If it did not, the rise is too fast to time and the external
         * pull-up is much stronger than the reference -- not weaker.
         */
        const double ratio = t_ext / t_par;
        if (ratio <= 1.05) {
            diag_printf("  ");
            print_ohms(r_int / 20.0);
            STRRES_PRINTF(STR_GPIO_RC_TOO_FAST);
            continue;
        }

        const double r_ext = r_int * (ratio - 1.0);
        print_ohms(r_ext);
        diag_printf("  ");
        print_farads(t_ext / RC_THRESHOLD_TAUS / r_ext);
        diag_printf("\n");
    }

    free(pins);
    return 0;
}
