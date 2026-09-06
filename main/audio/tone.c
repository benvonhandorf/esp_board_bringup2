/*
 * Test signal generation: a steady tone and a frequency sweep.
 *
 * Everything here is bus-agnostic -- it fills a buffer and hands it to
 * audio_bus_write(). Nothing knows what part is on the other end, which is why
 * a bare I2S amplifier with no control interface is exercised by exactly the
 * same commands as a codec with sixty registers.
 */
#include "app_bringup.h"
#include "audio.h"

#include "freertos/semphr.h"

#include "esp_timer.h"

#include <math.h>

/*
 * A 1024-entry table with 8-bit interpolation between entries, rather than
 * calling sinf() per sample.
 *
 * The ESP32-S3 has an FPU and would manage sinf() at 48 kHz; the ESP32-C3 has
 * none, and softfloat there would spend a large part of a core generating the
 * test signal -- exactly when the point of the exercise is to find out whether
 * the *hardware* keeps up. The table costs 2 KB of RAM, is built once, and is
 * the same speed on every chip. Interpolated this way its error floor measures
 * 90 dB below the signal and its worst single-sample error is 2 LSB of 32767,
 * far below anything an amplifier under test will contribute.
 */
#define SINE_BITS  10
#define SINE_SIZE  (1 << SINE_BITS)
#define SINE_MASK  (SINE_SIZE - 1)

static int16_t sine_table[SINE_SIZE];
static bool sine_ready;

static void sine_table_init(void)
{
    if (sine_ready) {
        return;
    }
    for (int i = 0; i < SINE_SIZE; i++) {
        sine_table[i] = (int16_t)lrint(32767.0 * sin(2.0 * M_PI * i / SINE_SIZE));
    }
    sine_ready = true;
}

/*
 * Phase is a full 32-bit accumulator, so a frequency is represented to about
 * 11 microhertz at 48 kHz and no frequency has to divide into the buffer
 * length. Phase carries across blocks, so block boundaries are inaudible.
 */
static inline int32_t sine_at(uint32_t phase)
{
    uint32_t index = phase >> (32 - SINE_BITS);
    uint32_t frac = (phase >> (32 - SINE_BITS - 8)) & 0xFF;
    int32_t a = sine_table[index];
    int32_t b = sine_table[(index + 1) & SINE_MASK];
    return a + (((b - a) * (int32_t)frac) >> 8);
}

static inline uint32_t phase_step(double hz, double rate)
{
    return (uint32_t)llrint(hz / rate * 4294967296.0);
}

/*
 * The generator writes one DMA descriptor at a time -- see
 * audio_bus_block_frames() for why that size and no other. At the descriptor
 * size the driver uses that is 240 frames, or 5 ms at 48 kHz, which also
 * bounds how coarsely a sweep steps in frequency and keeps the write loop
 * responsive to a stop request.
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
    volatile bool running;
    volatile bool stop_requested;
    SemaphoreHandle_t finished;
} player;

bool audio_playing(void)
{
    return player.running;
}

/*
 * A raised-cosine fade, sin^2(pi*t/2), built from the same table so the sample
 * loop stays integer-only. Returns Q15 gain for position `n` of `span`.
 */
static inline int32_t fade_gain(uint32_t n, uint32_t span)
{
    if (span == 0 || n >= span) {
        return 32768;
    }
    /* Quarter turn of the sine over the span. */
    uint32_t phase = (uint32_t)(((uint64_t)n << 30) / span);
    int32_t s = sine_at(phase);
    return (s * s) >> 15;
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

static double instantaneous_hz(const audio_signal_t *sig, double elapsed)
{
    if (sig->end_hz == sig->start_hz || sig->seconds <= 0.0) {
        return sig->start_hz;
    }

    double t = elapsed / sig->seconds;
    if (t > 1.0) {
        t = 1.0;
    }

    /* A log sweep spends equal time per octave, which is how audio hardware is
     * actually judged, so it is the default. */
    if (sig->logarithmic) {
        return sig->start_hz * pow(sig->end_hz / sig->start_hz, t);
    }
    return sig->start_hz + (sig->end_hz - sig->start_hz) * t;
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
 * Generate and write until the duration expires or a stop is requested.
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
    if (!buffer) {
        result->error = ESP_ERR_NO_MEM;
        return;
    }

    uint64_t total_frames = (sig->seconds > 0.0)
        ? (uint64_t)llrint(sig->seconds * rate) : 0;   /* 0 means until stopped */
    uint32_t fade_frames = (uint32_t)((uint64_t)FADE_MS * rate / 1000);
    /* The stop-time fade below emits one block, so the ramp has to fit in one. */
    if (fade_frames > block_frames) {
        fade_frames = (uint32_t)block_frames;
    }
    if (total_frames > 0 && fade_frames * 2 > total_frames) {
        fade_frames = (uint32_t)(total_frames / 2);
    }

    int32_t amplitude = (int32_t)sig->level_pct * 32767 / 100;
    uint64_t prefill = audio_bus_buffered_frames();

    uint32_t phase = 0;
    uint32_t step = phase_step(sig->start_hz, rate);
    uint64_t done = 0;

    while (!player.stop_requested) {
        size_t frames = block_frames;
        if (total_frames > 0 && done + frames > total_frames) {
            frames = (size_t)(total_frames - done);
        }
        if (frames == 0) {
            break;
        }

        /* One frequency per block. At 5 ms a block the steps are far below the
         * ear's pitch resolution, and phase continuity means no clicks. */
        step = phase_step(instantaneous_hz(sig, (double)done / rate), rate);

        for (size_t i = 0; i < frames; i++) {
            int32_t sample = (sine_at(phase) * amplitude) >> 15;

            uint64_t n = done + i;
            if (n < fade_frames) {
                sample = (sample * fade_gain((uint32_t)n, fade_frames)) >> 15;
            } else if (total_frames > 0 && n >= total_frames - fade_frames) {
                uint32_t remaining = (uint32_t)(total_frames - n);
                sample = (sample * fade_gain(remaining, fade_frames)) >> 15;
            }

            put_frame(buffer, i, sample, fmt->bits, sig->channel);
            phase += step;
        }

        size_t written = 0;
        esp_err_t err = audio_bus_write(buffer, frames * frame_bytes, &written,
                                        WRITE_TIMEOUT_MS);
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
        if (frames == block_frames) {
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

    /*
     * A continuous tone has no scheduled fade-out, so `audio stop` would cut
     * the waveform mid-cycle and thump the speaker. Ramp it down here instead.
     * This runs after the measurement is closed out so the extra frames do not
     * skew it.
     */
    if (player.stop_requested && result->error == ESP_OK && fade_frames > 0) {
        for (uint32_t i = 0; i < fade_frames; i++) {
            int32_t sample = (sine_at(phase) * amplitude) >> 15;
            sample = (sample * fade_gain(fade_frames - i, fade_frames)) >> 15;
            put_frame(buffer, i, sample, fmt->bits, sig->channel);
            phase += step;
        }
        size_t written = 0;
        audio_bus_write(buffer, fade_frames * frame_bytes, &written,
                        WRITE_TIMEOUT_MS);
        result->frames_written += fade_frames;
    }

    free(buffer);
}

static void report(const play_result_t *result)
{
    const audio_signal_t *sig = &player.signal;
    const audio_format_t *fmt = audio_bus_format();
    uint32_t configured = audio_bus_actual_rate();

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

    sine_table_init();

    esp_err_t err = audio_bus_tx_enable(true);
    if (err != ESP_OK) {
        STRRES_ERROR(STR_AUDIO_TRANSMITTER_ENABLE_FAILED, esp_err_to_name(err));
        return -1;
    }

    player.signal = *signal;
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
