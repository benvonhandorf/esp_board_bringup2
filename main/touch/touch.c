/*
 * Capacitive touch pad bring-up: `touch watch <pads> [seconds]`.
 *
 * Touch channels on hardware version 2 (ESP32-S2/S3) map 1:1 onto GPIO
 * numbers -- channel N is GPIO N (soc/touch_sensor_channel.h) -- so pads are
 * given and reported as GPIO numbers, consistent with every other command in
 * this project.
 *
 * There is no hardware touch peripheral on the ESP32-C3, this project's
 * default target, so the real implementation below only builds for chips
 * with touch sensor hardware version 2 or later; anything else gets a stub
 * that refuses with an explanation, the same shape as `cmd_sd_mmc` refuses on
 * chips with no SD host (main/sd/sd.c).
 */
#include "app_bringup.h"
#include "touch.h"

#include "soc/soc_caps.h"

#if SOC_TOUCH_SENSOR_SUPPORTED && SOC_TOUCH_SENSOR_VERSION >= 2

#include "driver/touch_sens.h"

#include <inttypes.h>
#include <math.h>

#define MAX_PADS (SOC_TOUCH_MAX_CHAN_ID - SOC_TOUCH_MIN_CHAN_ID + 1)

#define DEFAULT_WATCH_SECONDS 15.0
#define MAX_WATCH_SECONDS     120.0
#define TICK_SECONDS          0.2
#define CAL_SECONDS           1.0

/*
 * How many calibration-noise widths a delta must clear to be reported as a
 * touch -- the same shape of "reject anything within the observed noise"
 * guard that `nau7802 calibrate` uses
 * (nau7802_scale.c in the nau7802 component).
 */
#define SIGNIFICANCE_MARGIN 6.0

typedef struct {
    int pin; /* == touch channel id on this hardware */
    touch_channel_handle_t handle;
    double baseline;
    double noise;      /* peak |sample - baseline| observed during calibration */
    double peak_delta; /* peak |sample - baseline| observed during the watch */
} pad_t;

/* Drop anything out of range or repeated, saying which and why rather than
 * silently testing a smaller set than was asked for. Mirrors the pin
 * filtering in cmd_gpio_short (main/gpio/gpio.c). */
static int validate_pads(const int *requested, int requested_count, pad_t *pads)
{
    int count = 0;
    for (int i = 0; i < requested_count; i++) {
        bool duplicate = false;
        for (int j = 0; j < count; j++) {
            duplicate = duplicate || (pads[j].pin == requested[i]);
        }

        if (duplicate) {
            diag_printf("Skipping GPIO %d: listed more than once\n", requested[i]);
        } else if (requested[i] < SOC_TOUCH_MIN_CHAN_ID || requested[i] > SOC_TOUCH_MAX_CHAN_ID) {
            diag_printf("Skipping GPIO %d: not a touch-capable pin (GPIO%d-%d only)\n",
                      requested[i], SOC_TOUCH_MIN_CHAN_ID, SOC_TOUCH_MAX_CHAN_ID);
        } else {
            pads[count].pin = requested[i];
            pads[count].handle = NULL;
            pads[count].baseline = 0.0;
            pads[count].noise = 0.0;
            pads[count].peak_delta = 0.0;
            count++;
        }
    }
    return count;
}

static esp_err_t read_smooth(touch_channel_handle_t handle, uint32_t *out)
{
    return touch_channel_read_data(handle, TOUCH_CHAN_DATA_TYPE_SMOOTH, out);
}

/* Average `ticks` smooth samples of every pad, TICK_SECONDS apart. */
static void sample_all(pad_t *pads, int count, int ticks, bool track_noise)
{
    for (int t = 0; t < ticks; t++) {
        for (int i = 0; i < count; i++) {
            uint32_t value = 0;
            if (read_smooth(pads[i].handle, &value) != ESP_OK) {
                continue;
            }
            if (track_noise) {
                double deviation = fabs((double)value - pads[i].baseline);
                if (deviation > pads[i].noise) {
                    pads[i].noise = deviation;
                }
            } else {
                pads[i].baseline += (double)value;
            }
        }
        vTaskDelay(pdMS_TO_TICKS((int)(TICK_SECONDS * 1000)));
    }
}

int cmd_touch_watch(int argc, char **argv)
{
    if (argc < 2) {
        diag_printf("Usage: watch <pads> [seconds]\n");
        diag_printf("<pads> is a single GPIO, a range, or a comma separated list, "
                   "e.g. '1,3,5-7'. Touch-capable pins on this chip are "
                   "GPIO%d-%d.\n", SOC_TOUCH_MIN_CHAN_ID, SOC_TOUCH_MAX_CHAN_ID);
        return -1;
    }

    int *requested = NULL;
    int requested_count = 0;
    if (cli_parse_pin_list(argv[1], &requested, &requested_count) < 0) {
        diag_error("Invalid pin specification: %s", argv[1]);
        return -1;
    }

    pad_t pads[MAX_PADS];
    int count = validate_pads(requested, requested_count, pads);
    free(requested);

    if (count == 0) {
        diag_error("No valid touch pins given");
        return -1;
    }

    double seconds = DEFAULT_WATCH_SECONDS;
    if (argc > 2) {
        if (argc > 3) {
            diag_error("Unexpected argument '%s'", argv[3]);
            return -1;
        }
        if (cli_parse_double_arg(argv[2], &seconds) < 0 || seconds <= 0.0 ||
            seconds > MAX_WATCH_SECONDS) {
            diag_error("Duration must be greater than 0 and at most %.0f seconds",
                     MAX_WATCH_SECONDS);
            return -1;
        }
    }

    /* Defaults taken from ESP-IDF's touch_sens_basic example: 500 charge
     * cycles per sample, 0.5-2.2 V charge window, self-bias. This driver has
     * no way to know a board's pad geometry ahead of time, so it uses the
     * same starting point Espressif ships rather than inventing one. */
    touch_sensor_sample_config_t sample_cfg = TOUCH_SENSOR_V2_DEFAULT_SAMPLE_CONFIG(
        500, TOUCH_VOLT_LIM_L_0V5, TOUCH_VOLT_LIM_H_2V2);
    touch_sensor_config_t sens_cfg = TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(1, &sample_cfg);

    touch_sensor_handle_t sens = NULL;
    esp_err_t err = touch_sensor_new_controller(&sens_cfg, &sens);
    if (err != ESP_OK) {
        diag_error("Creating the touch controller: %s", esp_err_to_name(err));
        return -1;
    }

    /*
     * The hardware active_thresh is left effectively disabled, set to the
     * highest value the field accepts: significance is judged in software
     * below, against a baseline and noise figure measured from these
     * specific pads on this specific board, rather than a single threshold
     * guessed ahead of time. On hardware version 2 (S2/S3, the only chips
     * this branch builds for) that field is 22 bits wide -- UINT32_MAX
     * overflows it and touch_sensor_new_channel() rejects every channel with
     * ESP_ERR_INVALID_ARG, regardless of which pin was requested.
     */
    touch_channel_config_t chan_cfg = {
        .active_thresh = {0x3FFFFF},
        .charge_speed = TOUCH_CHARGE_SPEED_7,
        .init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT,
    };

    int ret = -1;
    bool scanning = false;

    for (int i = 0; i < count; i++) {
        err = touch_sensor_new_channel(sens, pads[i].pin, &chan_cfg, &pads[i].handle);
        if (err != ESP_OK) {
            diag_error("Allocating channel for GPIO %d: %s", pads[i].pin,
                     esp_err_to_name(err));
            goto cleanup;
        }
    }

    touch_sensor_filter_config_t filter_cfg = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
    if (touch_sensor_config_filter(sens, &filter_cfg) != ESP_OK) {
        diag_error("Configuring the touch filter failed");
        goto cleanup;
    }

    if (touch_sensor_enable(sens) != ESP_OK) {
        diag_error("Enabling the touch sensor failed");
        goto cleanup;
    }
    if (touch_sensor_start_continuous_scanning(sens) != ESP_OK) {
        diag_error("Starting continuous scanning failed");
        touch_sensor_disable(sens);
        goto cleanup;
    }
    scanning = true;

    diag_printf("Calibrating %d pad%s -- do not touch any of them...\n", count,
              count == 1 ? "" : "s");
    /* Let the hardware's own benchmark filter settle before trusting it. */
    vTaskDelay(pdMS_TO_TICKS(300));

    int cal_ticks = (int)(CAL_SECONDS / TICK_SECONDS);
    sample_all(pads, count, cal_ticks, false);
    for (int i = 0; i < count; i++) {
        pads[i].baseline /= cal_ticks;
    }
    sample_all(pads, count, cal_ticks, true);

    diag_printf("Baseline:");
    for (int i = 0; i < count; i++) {
        diag_printf("  GPIO%-2d %6.0f (+/-%.0f)", pads[i].pin, pads[i].baseline,
                  pads[i].noise);
    }
    diag_printf("\n\nWatching for %.0f s. Touch any pad -- all pads are reported "
              "every tick, so a neighbour reacting is as visible as the one "
              "you touched.\n", seconds);

    int ticks = (int)(seconds / TICK_SECONDS);
    for (int t = 0; t < ticks; t++) {
        diag_printf("%6.1fs", t * TICK_SECONDS);
        for (int i = 0; i < count; i++) {
            uint32_t value = 0;
            if (read_smooth(pads[i].handle, &value) != ESP_OK) {
                diag_printf("  GPIO%-2d    ERR      ", pads[i].pin);
                continue;
            }
            double delta = (double)value - pads[i].baseline;
            double magnitude = fabs(delta);
            double threshold = SIGNIFICANCE_MARGIN * (pads[i].noise + 1.0);
            if (magnitude > pads[i].peak_delta) {
                pads[i].peak_delta = magnitude;
            }
            diag_printf("  GPIO%-2d %6" PRIu32 " (%+6.0f)%s", pads[i].pin, value, delta,
                      magnitude > threshold ? " TOUCH" : "      ");
        }
        diag_printf("\n");
        vTaskDelay(pdMS_TO_TICKS((int)(TICK_SECONDS * 1000)));
    }

    diag_printf("\nPeak deflection from baseline over the run:\n");
    for (int i = 0; i < count; i++) {
        diag_printf("  GPIO%-2d  peak %5.0f  (%.1fx calibration noise)\n", pads[i].pin,
                  pads[i].peak_delta,
                  pads[i].noise > 0.0 ? pads[i].peak_delta / pads[i].noise : 0.0);
    }

    ret = 0;

cleanup:
    if (scanning) {
        touch_sensor_stop_continuous_scanning(sens);
        touch_sensor_disable(sens);
    }
    for (int i = 0; i < count; i++) {
        if (pads[i].handle) {
            touch_sensor_del_channel(pads[i].handle);
        }
    }
    touch_sensor_del_controller(sens);
    return ret;
}

#else /* !(SOC_TOUCH_SENSOR_SUPPORTED && SOC_TOUCH_SENSOR_VERSION >= 2) */

int cmd_touch_watch(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    diag_error("%s has no capacitive touch sensor peripheral supported by this "
             "driver. Rebuild for a chip with touch hardware version 2 or "
             "later, e.g. 'idf.py set-target esp32s3'.", CONFIG_IDF_TARGET);
    return -1;
}

#endif
