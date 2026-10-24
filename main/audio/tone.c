/*
 * Playing a test signal: a steady tone or a frequency sweep.
 *
 * The waveform comes from the tone_gen component, which fills mono samples and
 * knows nothing about buses. This file is everything specific to this rig:
 * packing those samples into whatever frame the bus is using, writing them to
 * the DMA, measuring the rate the hardware really consumed them at, and saying
 * what happened. Nothing here knows what part is on the other end, which is why
 * a bare I2S amplifier with no control interface is exercised by exactly the
 * same commands as a codec with sixty registers.
 */
#include "app_bringup.h"
#include "audio.h"

#include "tone_gen.h"

#include "freertos/semphr.h"

#include "esp_timer.h"

#include <math.h>

/*
 * The generator writes one DMA descriptor at a time -- see
 * audio_bus_block_frames() for why that size and no other. At the descriptor
 * size the driver uses that is 240 frames, or 5 ms at 48 kHz, which also keeps
 * the write loop responsive to a stop request.
 */
#define MAX_BLOCK_FRAMES 512

/* Long enough that a step to full scale does not thump the speaker, short
 * enough not to distort the measurement of a one-second tone. */
#define FADE_MS 5

#define WRITE_TIMEOUT_MS 1000

/* ------------------------------------------------------------------ */
/* Player state                                                        */
/* ------------------------------------------------------------------ */

static struct {
    audio_signal_t signal;
    /* A dial string plays instead of `signal` when `dtmf` is set. It is
     * blocking-only, so these live exactly as long as the call that set them. */
    bool dtmf;
    tone_gen_dtmf_config_t dtmf_cfg;
    char digits[AUDIO_DTMF_MAX_DIGITS + 1];
    audio_dtmf_req_t dtmf_req;
    volatile bool running;
    volatile bool stop_requested;
    SemaphoreHandle_t finished;
} player;

bool audio_playing(void)
{
    return player.running;
}

/* Pack one sample pair into the buffer at frame `i`. */
static inline void put_frame(void *buffer, size_t i, int32_t sample,
                             uint8_t bits, audio_channel_t channel)
{
    int32_t left = (channel == AUDIO_CHANNEL_RIGHT) ? 0 : sample;
    int32_t right = (channel == AUDIO_CHANNEL_LEFT) ? 0 : sample;

    if (bits == 16) {
        int16_t *out = (int16_t *)buffer;
        out[2 * i] = (int16_t)left;
        out[2 * i + 1] = (int16_t)right;
    } else {
        /* 24-bit data sits right-aligned in a 32-bit slot, which is the
         * driver's default alignment; 32-bit data fills the slot. */
        int shift = (bits == 24) ? 8 : 16;
        int32_t *out = (int32_t *)buffer;
        out[2 * i] = left << shift;
        out[2 * i + 1] = right << shift;
    }
}

typedef struct {
    uint64_t frames_written;
    /* The measurement window: both ends taken at the same DMA queue state. */
    uint64_t frames_at_start;
    uint64_t frames_at_end;
    int64_t measure_start_us;
    int64_t measure_end_us;
    bool measured;
    int short_writes;
    esp_err_t error;
} play_result_t;

/*
 * Generate and write until the signal ends or a stop is requested.
 *
 * The rate measurement is taken between two moments when the DMA queue is
 * equally full, not across the whole run. Writes block once the queue fills,
 * so from then on the write rate *is* the rate the hardware consumes samples;
 * timing from the first block onwards instead would charge the prefill to the
 * measurement and under-report by the buffer depth.
 */
static void run_player(play_result_t *result)
{
    const audio_signal_t *sig = &player.signal;
    const audio_format_t *fmt = audio_bus_format();
    memset(result, 0, sizeof(*result));

    if (!fmt) {
        result->error = ESP_ERR_INVALID_STATE;
        return;
    }

    uint32_t rate = audio_bus_actual_rate();
    if (rate == 0) {
        rate = fmt->rate_hz;
    }

    size_t frame_bytes = audio_bus_frame_bytes();
    size_t block_frames = audio_bus_block_frames();
    if (block_frames == 0 || block_frames > MAX_BLOCK_FRAMES) {
        block_frames = MAX_BLOCK_FRAMES;
    }

    void *buffer = malloc(block_frames * frame_bytes);
    int16_t *mono = malloc(block_frames * sizeof(int16_t));
    if (!buffer || !mono) {
        free(buffer);
        free(mono);
        result->error = ESP_ERR_NO_MEM;
        return;
    }

    /*
     * One of two sources, behind the same fill-and-release shape. Either way
     * the samples are mono and the rest of this function neither knows nor
     * cares which it is.
     */
    tone_gen_t tone;
    tone_gen_dtmf_t dial;
    esp_err_t err;
    audio_channel_t channel;

    if (player.dtmf) {
        tone_gen_dtmf_config_t dcfg = player.dtmf_cfg;
        dcfg.sample_rate_hz = rate;
        channel = player.dtmf_req.channel;
        err = tone_gen_dtmf_init(&dial, &dcfg, player.digits);
    } else {
        /* A sweep with no duration is a steady tone at its start frequency,
         * which is what the generator would otherwise refuse. */
        tone_gen_config_t cfg = {
            .sample_rate_hz = rate,
            .start_hz = sig->start_hz,
            .end_hz = (sig->seconds > 0.0) ? sig->end_hz : sig->start_hz,
            .logarithmic = sig->logarithmic,
            .duration_samples = (sig->seconds > 0.0)
                ? (uint64_t)llrint(sig->seconds * rate) : 0,   /* 0 means until stopped */
            .peak = (int16_t)((int32_t)sig->level_pct * 32767 / 100),
            .ramp_ms = FADE_MS,
        };
        channel = sig->channel;
        err = tone_gen_init(&tone, &cfg);
    }
    if (err != ESP_OK) {
        free(buffer);
        free(mono);
        result->error = err;
        return;
    }

    uint64_t prefill = audio_bus_buffered_frames();
    uint64_t done = 0;
    bool released = false;

    for (;;) {
        /*
         * A continuous tone has no scheduled fade-out, so `audio stop` would cut
         * the waveform mid-cycle and thump the speaker. Releasing the generator
         * ramps it down instead, and the loop below keeps writing until it
         * runs dry. The measurement is closed out before this point, so the
         * extra frames do not skew it.
         */
        if (player.stop_requested && !released) {
            if (player.dtmf) {
                tone_gen_dtmf_release(&dial);
            } else {
                tone_gen_release(&tone);
            }
            released = true;
        }

        size_t frames = player.dtmf ? tone_gen_dtmf_fill(&dial, mono, block_frames)
                                    : tone_gen_fill(&tone, mono, block_frames);
        if (frames == 0) {
            break;
        }

        for (size_t i = 0; i < frames; i++) {
            put_frame(buffer, i, mono[i], fmt->bits, channel);
        }

        size_t written = 0;
        err = audio_bus_write(buffer, frames * frame_bytes, &written, WRITE_TIMEOUT_MS);
        if (err != ESP_OK) {
            result->error = err;
            break;
        }
        if (written < frames * frame_bytes) {
            result->short_writes++;
        }

        done += frames;
        result->frames_written = done;

        /*
         * Only a full-size write may open or close the window. A short final
         * block returns as soon as there is room for its smaller self, leaving
         * the queue fuller than a full block would, and that difference would
         * be charged to the measurement as if the hardware had consumed it.
         */
        if (!released && frames == block_frames) {
            if (!result->measured && done >= prefill) {
                /* Writes are rate-limited by the hardware from here on. */
                result->measured = true;
                result->measure_start_us = esp_timer_get_time();
                result->frames_at_start = done;
            } else if (result->measured) {
                result->measure_end_us = esp_timer_get_time();
                result->frames_at_end = done;
            }
        }
    }

    free(buffer);
    free(mono);
}

/* The sample-rate lines every playback ends with: asked for, configured, and
 * measured. */
static void report_rate(const play_result_t *result)
{
    const audio_format_t *fmt = audio_bus_format();
    uint32_t configured = audio_bus_actual_rate();

    STRRES_PRINTF(STR_AUDIO_RATE_REQUESTED, (unsigned long)(fmt ? fmt->rate_hz : 0));
    if (configured) {
        STRRES_PRINTF(STR_AUDIO_RATE_CONFIGURED, (unsigned long)configured);
    }

    /*
     * The measured rate is the one worth reading. The configured rate says what
     * the divider was set to; this says how fast samples actually left, and a
     * shortfall means the generator or the console starved the DMA.
     */
    if (result->measured && result->frames_at_end > result->frames_at_start &&
        result->measure_end_us > result->measure_start_us) {
        double seconds = (double)(result->measure_end_us - result->measure_start_us) / 1e6;
        uint64_t frames = result->frames_at_end - result->frames_at_start;
        STRRES_PRINTF(STR_AUDIO_RATE_MEASURED, frames / seconds);
    } else {
        diag_printf("\n");
        STRRES_PRINTF(STR_AUDIO_TOO_SHORT_TO_MEASURE);
    }

    if (result->short_writes) {
        STRRES_PRINTF(STR_AUDIO_BLOCKS_TIMED_OUT,
                      result->short_writes, result->short_writes == 1 ? "" : "s");
    }
}

static void report(const play_result_t *result)
{
    const audio_signal_t *sig = &player.signal;

    if (result->error != ESP_OK) {
        STRRES_ERROR(STR_AUDIO_PLAYBACK_FAILED, esp_err_to_name(result->error));
        return;
    }

    double dbfs = 20.0 * log10((double)sig->level_pct / 100.0);

    if (sig->end_hz == sig->start_hz) {
        STRRES_PRINTF(STR_AUDIO_PLAYED, sig->start_hz);
    } else {
        STRRES_PRINTF(STR_AUDIO_SWEPT,
                      sig->start_hz, sig->end_hz, sig->logarithmic ? "logarithmic" : "linear");
    }
    STRRES_PRINTF(STR_AUDIO_AT_LEVEL,
                  sig->level_pct, dbfs, (unsigned long long)result->frames_written);

    report_rate(result);
}

static void report_dtmf(const play_result_t *result)
{
    if (result->error != ESP_OK) {
        STRRES_ERROR(STR_AUDIO_PLAYBACK_FAILED, esp_err_to_name(result->error));
        return;
    }

    const audio_dtmf_req_t *req = &player.dtmf_req;
    double dbfs = 20.0 * log10((double)req->level_pct / 100.0);

    STRRES_PRINTF(STR_AUDIO_DTMF_DIALLED, player.digits);
    STRRES_PRINTF(STR_AUDIO_DTMF_TIMING, player.dtmf_cfg.tone_ms, player.dtmf_cfg.gap_ms,
                  player.dtmf_cfg.pause_ms);
    STRRES_PRINTF(STR_AUDIO_DTMF_AT_LEVEL, req->level_pct, dbfs, req->twist_db,
                  (unsigned long long)result->frames_written);

    report_rate(result);
}

static void player_task(void *arg)
{
    (void)arg;

    play_result_t result;
    run_player(&result);
    if (!player.signal.quiet) {
        report(&result);
    }

    player.running = false;
    xSemaphoreGive(player.finished);
    vTaskDelete(NULL);
}

int audio_play(const audio_signal_t *signal)
{
    if (!audio_bus_require()) {
        return -1;
    }
    if (player.running) {
        STRRES_ERROR(STR_AUDIO_ALREADY_PLAYING);
        return -1;
    }

    esp_err_t err = audio_bus_tx_enable(true);
    if (err != ESP_OK) {
        STRRES_ERROR(STR_AUDIO_TRANSMITTER_ENABLE_FAILED, esp_err_to_name(err));
        return -1;
    }

    player.signal = *signal;
    player.dtmf = false;
    player.stop_requested = false;
    player.running = true;

    /*
     * A finite tone runs on the caller's task and blocks. Commands execute one
     * at a time, so the shell -- serial and web alike -- is unavailable for the
     * duration either way; running it here keeps the ordinary case to one code
     * path and lets the result print before the prompt returns. `continuous`
     * is the interruptible form, and that one needs a task of its own.
     */
    if (signal->seconds > 0.0) {
        play_result_t result;
        run_player(&result);
        if (!signal->quiet) {
            report(&result);
        }
        player.running = false;
        return result.error == ESP_OK ? 0 : -1;
    }

    if (!player.finished) {
        player.finished = xSemaphoreCreateBinary();
        if (!player.finished) {
            player.running = false;
            STRRES_ERROR(STR_AUDIO_OUT_OF_MEMORY);
            return -1;
        }
    }

    /* Clear any completion left over from a run that ended on its own -- a
     * stale signal here would let the next `audio stop` return before the task
     * had actually finished with the channel. */
    xSemaphoreTake(player.finished, 0);

    /* Above the executor's priority so filling the DMA buffer wins over
     * command parsing; it spends nearly all its time blocked in the write. */
    if (xTaskCreate(player_task, "audio_play", 4096, NULL, 4, NULL) != pdPASS) {
        player.running = false;
        STRRES_ERROR(STR_AUDIO_TASK_START_FAILED);
        return -1;
    }

    if (!signal->quiet) {
        STRRES_PRINTF(STR_AUDIO_PLAYING_CONTINUOUSLY);
    }
    return 0;
}

/*
 * Overall level and twist to the two tone amplitudes. The two peaks sum to
 * level_pct of full scale, which is what guarantees their sum never clips;
 * twist makes the high tone louder by that many decibels.
 */
static void dtmf_peaks(int level_pct, double twist_db, int16_t *low, int16_t *high)
{
    int32_t total = (int32_t)level_pct * 32767 / 100;
    double ratio = pow(10.0, twist_db / 20.0);
    int32_t lo = (int32_t)lrint((double)total / (1.0 + ratio));
    *low = (int16_t)lo;
    *high = (int16_t)(total - lo);
}

int audio_play_dtmf(const audio_dtmf_req_t *req)
{
    if (!audio_bus_require()) {
        return -1;
    }
    if (player.running) {
        STRRES_ERROR(STR_AUDIO_ALREADY_PLAYING);
        return -1;
    }

    size_t bad = 0;
    esp_err_t err = tone_gen_dtmf_validate(req->digits, &bad);
    if (err == TONE_GEN_ERR_INVALID_DIGIT) {
        STRRES_ERROR(STR_AUDIO_DTMF_BAD_KEY, req->digits[bad], (int)bad + 1);
        return -1;
    }
    if (err != ESP_OK) {
        STRRES_ERROR(STR_AUDIO_DTMF_NOTHING_TO_DIAL);
        return -1;
    }
    if (strlen(req->digits) > AUDIO_DTMF_MAX_DIGITS) {
        STRRES_ERROR(STR_AUDIO_DTMF_TOO_LONG, AUDIO_DTMF_MAX_DIGITS);
        return -1;
    }

    err = audio_bus_tx_enable(true);
    if (err != ESP_OK) {
        STRRES_ERROR(STR_AUDIO_TRANSMITTER_ENABLE_FAILED, esp_err_to_name(err));
        return -1;
    }

    player.dtmf = true;
    player.dtmf_req = *req;
    strlcpy(player.digits, req->digits, sizeof(player.digits));
    player.dtmf_req.digits = player.digits;

    tone_gen_dtmf_config_t cfg = TONE_GEN_DTMF_CONFIG_DEFAULT(0);
    if (req->tone_ms) cfg.tone_ms = req->tone_ms;
    if (req->gap_ms) cfg.gap_ms = req->gap_ms;
    if (req->pause_ms) cfg.pause_ms = req->pause_ms;
    dtmf_peaks(req->level_pct, req->twist_db, &cfg.low_peak, &cfg.high_peak);
    player.dtmf_cfg = cfg;

    player.stop_requested = false;
    player.running = true;

    /* Always blocking: a dial string is seconds long and the line should be
     * quiet again when the command returns. */
    play_result_t result;
    run_player(&result);
    if (!req->quiet) {
        report_dtmf(&result);
    }
    player.running = false;
    player.dtmf = false;
    return result.error == ESP_OK ? 0 : -1;
}

int audio_play_stop(void)
{
    if (!player.running) {
        STRRES_PRINTF(STR_AUDIO_NOTHING_PLAYING);
        return 0;
    }

    player.stop_requested = true;

    /* Wait for the task to finish its block and report, so a following
     * `audio close` cannot delete the channel out from under it. */
    if (player.finished &&
        xSemaphoreTake(player.finished, pdMS_TO_TICKS(2000)) != pdTRUE) {
        STRRES_ERROR(STR_AUDIO_TASK_STOP_TIMEOUT);
        return -1;
    }

    return 0;
}
