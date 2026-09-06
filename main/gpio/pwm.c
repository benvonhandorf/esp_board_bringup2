#include "app_bringup.h"
#include "pwm.h"

#include "driver/gpio.h"
#include "driver/ledc.h"

/* Low-speed mode is the only mode every ESP target implements. */
#define PWM_SPEED_MODE LEDC_LOW_SPEED_MODE

#define PWM_MAX_CHANNELS SOC_LEDC_CHANNEL_NUM
#define PWM_MAX_TIMERS   SOC_LEDC_TIMER_NUM

typedef struct {
    bool in_use;
    int pin;
    ledc_timer_t timer;
    uint32_t freq_hz;
    int duty_percent;
} pwm_channel_t;

static pwm_channel_t channels[PWM_MAX_CHANNELS];

/* Timers are shared between channels running at the same frequency; a chip
 * typically has four, so one-timer-per-channel would run out immediately. */
typedef struct {
    int users;
    uint32_t freq_hz;
    ledc_timer_bit_t resolution;
} pwm_timer_t;

static pwm_timer_t timers[PWM_MAX_TIMERS];

/*
 * Pick the finest duty resolution the LEDC clock can deliver at this
 * frequency. The source clock must satisfy freq * 2^resolution <= clk, so high
 * frequencies necessarily get coarser duty control.
 */
static ledc_timer_bit_t best_resolution(uint32_t freq_hz)
{
    /* APB is 80MHz on every target that has LEDC in low-speed mode. */
    const uint32_t source_clk_hz = 80000000;

    ledc_timer_bit_t resolution = (ledc_timer_bit_t)SOC_LEDC_TIMER_BIT_WIDTH;
    while (resolution > 1) {
        if ((uint64_t)freq_hz * (1ULL << resolution) <= source_clk_hz) {
            break;
        }
        resolution--;
    }
    return resolution;
}

static pwm_channel_t *find_channel_for_pin(int pin)
{
    for (int i = 0; i < PWM_MAX_CHANNELS; i++) {
        if (channels[i].in_use && channels[i].pin == pin) {
            return &channels[i];
        }
    }
    return NULL;
}

static void release_timer(ledc_timer_t timer)
{
    if (timers[timer].users > 0 && --timers[timer].users == 0) {
        ledc_timer_rst(PWM_SPEED_MODE, timer);
        timers[timer].freq_hz = 0;
    }
}

/* Reuse a timer already running at this frequency, else claim a free one. */
static int acquire_timer(uint32_t freq_hz, ledc_timer_bit_t resolution)
{
    for (int i = 0; i < PWM_MAX_TIMERS; i++) {
        if (timers[i].users > 0 && timers[i].freq_hz == freq_hz &&
            timers[i].resolution == resolution) {
            timers[i].users++;
            return i;
        }
    }

    for (int i = 0; i < PWM_MAX_TIMERS; i++) {
        if (timers[i].users != 0) {
            continue;
        }

        const ledc_timer_config_t config = {
            .speed_mode = PWM_SPEED_MODE,
            .timer_num = (ledc_timer_t)i,
            .duty_resolution = resolution,
            .freq_hz = freq_hz,
            .clk_cfg = LEDC_AUTO_CLK,
        };

        esp_err_t err = ledc_timer_config(&config);
        if (err != ESP_OK) {
            STRRES_ERROR(STR_GPIO_PWM_TIMER_CONFIG_FAILED,
                         i, (unsigned long)freq_hz, esp_err_to_name(err));
            return -1;
        }

        timers[i].users = 1;
        timers[i].freq_hz = freq_hz;
        timers[i].resolution = resolution;
        return i;
    }

    STRRES_ERROR(STR_GPIO_PWM_NO_FREE_TIMER, PWM_MAX_TIMERS);
    return -1;
}

int cmd_pwm_set(int argc, char **argv)
{
    if (argc < 4) {
        STRRES_PRINTF(STR_GPIO_PWM_USAGE_SET);
        return -1;
    }

    int pin, freq, duty;
    if (cli_parse_int_arg(argv[1], &pin) < 0) {
        STRRES_ERROR(STR_GPIO_PWM_PIN_INVALID, argv[1]);
        return -1;
    }
    if (cli_parse_int_arg(argv[2], &freq) < 0 || freq < 1 || freq > 10000000) {
        STRRES_ERROR(STR_GPIO_PWM_FREQUENCY_RANGE);
        return -1;
    }
    if (cli_parse_int_arg(argv[3], &duty) < 0 || duty < 0 || duty > 100) {
        STRRES_ERROR(STR_GPIO_PWM_DUTY_RANGE);
        return -1;
    }

    if (!GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
        STRRES_ERROR(STR_GPIO_PWM_PIN_NOT_OUTPUT_CAPABLE, pin);
        return -1;
    }

    /* Re-running set on the same pin retunes it in place. Re-running
     * ledc_channel_config() on a GPIO the driver has already routed makes it
     * complain that the pin conflicts with an existing channel. */
    pwm_channel_t *channel = find_channel_for_pin(pin);
    bool retune = channel != NULL;

    if (retune) {
        release_timer(channel->timer);
    } else {
        for (int i = 0; i < PWM_MAX_CHANNELS; i++) {
            if (!channels[i].in_use) {
                channel = &channels[i];
                break;
            }
        }
        if (!channel) {
            STRRES_ERROR(STR_GPIO_PWM_NO_FREE_CHANNEL, PWM_MAX_CHANNELS);
            return -1;
        }
    }

    ledc_channel_t index = (ledc_channel_t)(channel - channels);
    ledc_timer_bit_t resolution = best_resolution((uint32_t)freq);
    int timer = acquire_timer((uint32_t)freq, resolution);
    if (timer < 0) {
        if (!retune) {
            channel->in_use = false;
        }
        return -1;
    }

    uint32_t max_duty = (1u << resolution) - 1;
    uint32_t duty_value = (uint32_t)(((uint64_t)duty * max_duty) / 100);

    esp_err_t err;
    if (retune) {
        err = ledc_bind_channel_timer(PWM_SPEED_MODE, index, (ledc_timer_t)timer);
        if (err == ESP_OK) {
            err = ledc_set_duty(PWM_SPEED_MODE, index, duty_value);
        }
        if (err == ESP_OK) {
            err = ledc_update_duty(PWM_SPEED_MODE, index);
        }
    } else {
        const ledc_channel_config_t channel_config = {
            .gpio_num = pin,
            .speed_mode = PWM_SPEED_MODE,
            .channel = index,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = (ledc_timer_t)timer,
            .duty = duty_value,
            .hpoint = 0,
        };
        err = ledc_channel_config(&channel_config);
    }

    if (err != ESP_OK) {
        STRRES_ERROR(STR_GPIO_PWM_CHANNEL_CONFIG_FAILED, pin, esp_err_to_name(err));
        release_timer((ledc_timer_t)timer);
        if (!retune) {
            channel->in_use = false;
        }
        return -1;
    }

    channel->in_use = true;
    channel->pin = pin;
    channel->timer = (ledc_timer_t)timer;
    channel->freq_hz = (uint32_t)freq;
    channel->duty_percent = duty;

    /* The achievable frequency is quantized by the divider, so report what the
     * hardware actually produces rather than what was asked for. */
    uint32_t actual = ledc_get_freq(PWM_SPEED_MODE, (ledc_timer_t)timer);

    STRRES_PRINTF(STR_GPIO_PWM_RUNNING,
                  pin, (unsigned long)actual, duty, (int)resolution,
                  (unsigned long)max_duty + 1);

    if (actual != (uint32_t)freq) {
        STRRES_PRINTF(STR_GPIO_PWM_FREQUENCY_ROUNDED, freq, (unsigned long)actual);
    }

    return 0;
}

int cmd_pwm_stop(int argc, char **argv)
{
    if (argc < 2) {
        STRRES_PRINTF(STR_GPIO_PWM_USAGE_STOP);
        return -1;
    }

    int pin;
    if (cli_parse_int_arg(argv[1], &pin) < 0) {
        STRRES_ERROR(STR_GPIO_PWM_PIN_INVALID, argv[1]);
        return -1;
    }

    pwm_channel_t *channel = find_channel_for_pin(pin);
    if (!channel) {
        STRRES_ERROR(STR_GPIO_PWM_NOT_RUNNING, pin);
        return -1;
    }

    ledc_channel_t index = (ledc_channel_t)(channel - channels);

    /* Idle level 0, so a driven LED ends up off rather than at an
     * indeterminate level. */
    esp_err_t err = ledc_stop(PWM_SPEED_MODE, index, 0);
    if (err != ESP_OK) {
        STRRES_ERROR(STR_GPIO_PWM_STOP_FAILED, pin, esp_err_to_name(err));
        return -1;
    }

    release_timer(channel->timer);
    channel->in_use = false;

    STRRES_PRINTF(STR_GPIO_PWM_STOPPED, pin);
    return 0;
}
