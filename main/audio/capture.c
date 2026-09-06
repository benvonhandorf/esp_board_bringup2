/*
 * Capture and analysis: what came in, and what it looks like.
 *
 * Bus-agnostic in the same way tone.c is. It pulls frames from
 * audio_bus_read() and knows nothing about whether they arrived from a PDM
 * microphone, an I2S microphone or a codec's ADC -- so `audio record` reports
 * on all three, and `audio loopback` can pair any of them with the transmitter.
 *
 * Two kinds of measurement come out of one pass:
 *
 *   Time domain -- min, max, mean, standard deviation and peak -- is computed
 *   over every frame captured, streaming, in integers. Those are the numbers
 *   that answer "is the part alive and is it centred", and they are cheap
 *   enough to run for as long as anyone cares to listen.
 *
 *   Frequency domain is computed after the fact over a bounded window held in
 *   RAM. That split is deliberate: the ESP32-C3 has no FPU, and a filter bank
 *   running per sample would spend more of the core on softfloat than the DMA
 *   can spare, dropping the very samples it was meant to describe. Analysing a
 *   fixed window afterwards costs the same on every chip and cannot starve the
 *   capture.
 */
#include "app_bringup.h"
#include "audio.h"

#include "esp_timer.h"

#include <math.h>

/*
 * 4096 frames is 85 ms at 48 kHz: 12 Hz of resolution, which separates a test
 * tone from anything broadband, and 16 KB of buffer, which is affordable on
 * every chip this runs on.
 */
#define ANALYSIS_FRAMES 4096

/* Half-octave steps from here up. Twenty of them reach 22.6 kHz. */
#define BAND_BASE_HZ 31.25

#define READ_TIMEOUT_MS 1000
#define MAX_CAPTURE_SECONDS 60.0

/* The analysis window is kept at 16 bits whatever the stream's width, so the
 * spectrum's own noise floor sits here regardless of the source. */
#define ANALYSIS_FULL_SCALE 32768.0

double audio_dbfs(double amplitude, double full_scale)
{
    if (full_scale <= 0.0 || amplitude <= 0.0) {
        return AUDIO_DBFS_FLOOR;
    }
    double db = 20.0 * log10(amplitude / full_scale);
    return db < AUDIO_DBFS_FLOOR ? AUDIO_DBFS_FLOOR : db;
}

/* ------------------------------------------------------------------ */
/* Goertzel                                                            */
/* ------------------------------------------------------------------ */

/*
 * Used for the single-frequency detector only, where the frequency is known
 * exactly and no bin quantisation should stand between the question and the
 * answer. The spectrum uses an FFT instead, for the reason set out over
 * band_power().
 */
typedef struct {
    double coeff;   /* 2 cos w */
    double sinw;
    double cosw;
    double s1;
    double s2;
} goertzel_t;

static void goertzel_init(goertzel_t *g, double hz, double rate)
{
    double w = 2.0 * M_PI * hz / rate;
    g->cosw = cos(w);
    g->sinw = sin(w);
    g->coeff = 2.0 * g->cosw;
    g->s1 = 0.0;
    g->s2 = 0.0;
}

static inline void goertzel_push(goertzel_t *g, double x)
{
    double s0 = x + g->coeff * g->s1 - g->s2;
    g->s2 = g->s1;
    g->s1 = s0;
}

/*
 * Amplitude of a sinusoid at the probe frequency, in the same units as the
 * input. The Hann window applied by the caller has a coherent gain of 0.5, so
 * the usual 2/N scaling becomes 4/N.
 */
static double goertzel_amplitude(const goertzel_t *g, uint32_t n)
{
    if (n == 0) {
        return 0.0;
    }
    double real = g->s1 - g->s2 * g->cosw;
    double imag = g->s2 * g->sinw;
    return 4.0 * sqrt(real * real + imag * imag) / (double)n;
}

/* ------------------------------------------------------------------ */
/* FFT                                                                 */
/* ------------------------------------------------------------------ */

/*
 * Iterative radix-2, in place, single precision.
 *
 * The first version of this file probed each band with one Goertzel, which is
 * cheap and wrong: half-octave centres at 12 Hz of resolution leave most of
 * the spectrum between the probes, and a host-side check found a 1189 Hz tone
 * reading 89 dB low because it fell in one of the gaps. A display that can
 * miss a strong tone entirely is worse than no display, since the conclusion
 * it invites is "the microphone is dead".
 *
 * Every bin has to be computed and every bin has to land in some band, so it
 * is an FFT. That is affordable because the analysis runs after the capture
 * has finished rather than alongside it: this costs a few hundred thousand
 * flops once, where a filter bank wide enough to do the same job would have
 * cost that much per second and starved the DMA on a chip with no FPU.
 *
 * Twiddles come from a recurrence in double precision. Single precision drifts
 * measurably over the 2048 steps of the last stage; the data stays float,
 * where 24 bits of mantissa put the arithmetic noise floor far below anything
 * a microphone contributes.
 */
static void fft(float *re, float *im, uint32_t n)
{
    for (uint32_t i = 1, j = 0; i < n; i++) {
        uint32_t bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }

    for (uint32_t len = 2; len <= n; len <<= 1) {
        double angle = -2.0 * M_PI / (double)len;
        double wr = cos(angle);
        double wi = sin(angle);
        uint32_t half = len / 2;

        for (uint32_t i = 0; i < n; i += len) {
            double cr = 1.0;
            double ci = 0.0;
            for (uint32_t k = 0; k < half; k++) {
                float ur = re[i + k];
                float ui = im[i + k];
                float xr = re[i + k + half];
                float xi = im[i + k + half];
                float vr = (float)(xr * cr - xi * ci);
                float vi = (float)(xr * ci + xi * cr);

                re[i + k] = ur + vr;
                im[i + k] = ui + vi;
                re[i + k + half] = ur - vr;
                im[i + k + half] = ui - vi;

                double next = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = next;
            }
        }
    }
}

/*
 * Convert summed bin power into the amplitude of the sinusoid that would carry
 * it.
 *
 * A Hann-windowed tone at bin centre puts N/4 of its amplitude in that bin and
 * N/8 in each neighbour, so its power sums to 3/32 of (A*N)^2 -- and the same
 * constant relates power to amplitude for noise spread across the band, which
 * is what makes one scale usable for both. Every bin belongs to exactly one
 * band, so nothing is counted twice and nothing falls through.
 */
static double band_power(double power, uint32_t n)
{
    if (n == 0 || power <= 0.0) {
        return 0.0;
    }
    return sqrt(power) / ((double)n * sqrt(3.0 / 32.0));
}

/* ------------------------------------------------------------------ */
/* Capture                                                             */
/* ------------------------------------------------------------------ */

/*
 * Streaming accumulators. Sums are integers: a 32-bit sample squared needs 62
 * bits, so the sum of squares is kept as a double once it can no longer be
 * exact, but the counts themselves never lose their least significant bit the
 * way a running float mean would.
 */
typedef struct {
    int32_t min;
    int32_t max;
    int64_t sum;
    double sum_sq;
    uint64_t clipped;
} slot_accum_t;

static inline void slot_push(slot_accum_t *a, int32_t sample, int32_t limit)
{
    if (sample < a->min) {
        a->min = sample;
    }
    if (sample > a->max) {
        a->max = sample;
    }
    a->sum += sample;
    a->sum_sq += (double)sample * (double)sample;
    if (sample >= limit || sample <= -limit) {
        a->clipped++;
    }
}

static inline void frame_at(const void *buffer, size_t i, uint8_t bits,
                            int32_t *left, int32_t *right)
{
    if (bits == 16) {
        const int16_t *p = (const int16_t *)buffer;
        *left = p[2 * i];
        *right = p[2 * i + 1];
    } else {
        /* 24-bit data arrives left-shifted inside a 32-bit slot, which is the
         * same alignment tone.c writes, so full scale is the slot's. */
        const int32_t *p = (const int32_t *)buffer;
        *left = p[2 * i];
        *right = p[2 * i + 1];
    }
}

void audio_capture_flush(double seconds)
{
    if (!audio_bus_rx_ready() || !audio_bus_rx_enabled()) {
        return;
    }

    size_t frame_bytes = audio_bus_rx_frame_bytes();
    size_t block_frames = audio_bus_block_frames();
    uint32_t rate = audio_bus_rx_rate();
    if (rate == 0) {
        rate = 48000;
    }

    void *buffer = malloc(block_frames * frame_bytes);
    if (!buffer) {
        return;
    }

    uint64_t target = (uint64_t)llrint(seconds * rate);
    uint64_t done = 0;
    while (done < target) {
        size_t got = 0;
        /* A short timeout, because the point is to drain what is already
         * queued rather than to wait for more to arrive. */
        if (audio_bus_read(buffer, block_frames * frame_bytes, &got, 100) != ESP_OK ||
            got == 0) {
            break;
        }
        done += got / frame_bytes;
    }

    free(buffer);
}

/*
 * Analyse the window collected during the capture.
 *
 * Hann-windowed, which costs one multiply per sample and buys two things worth
 * having: scalloping loss drops from 3.9 dB to 1.4 dB, so a tone that does not
 * land on a probe frequency is still reported at roughly the right level, and
 * the skirts of a loud tone stop appearing in unrelated probes.
 */
static void analyse(audio_capture_t *cap, const int16_t *window, uint32_t n,
                    float *re, float *im)
{
    if (n < 64) {
        return;   /* too short for any of this to mean anything */
    }

    double rate = (double)cap->rate;
    uint32_t bands = 0;

    /* Half-octave centres, stopping short of Nyquist where the anti-alias
     * response of whatever is upstream is unknown anyway.
     *
     * Left at zero without the transform buffers, so band_count is never
     * non-zero while band_dbfs is still full of zeroes -- which would draw a
     * chart pinned at full scale on every band. */
    if (re) {
        for (uint32_t i = 0; i < AUDIO_BANDS; i++) {
            double hz = BAND_BASE_HZ * pow(2.0, i / 2.0);
            if (hz >= rate * 0.45) {
                break;
            }
            cap->band_hz[bands++] = hz;
        }
    }
    cap->band_count = bands;

    /* The FFT wants a power of two, and the capture may have stopped short of
     * one; analyse the largest that fits. */
    uint32_t fft_n = 1;
    while (fft_n * 2 <= n) {
        fft_n *= 2;
    }

    for (uint32_t ch = 0; ch < 2; ch++) {
        goertzel_t detector;
        if (cap->detect_hz > 0.0) {
            goertzel_init(&detector, cap->detect_hz, rate);
        }

        /* Hann by recurrence: cos((k+1)a) = 2 cos(a) cos(ka) - cos((k-1)a).
         * Two adds and a multiply per sample instead of a cosine. It costs
         * 1.4 dB of scalloping instead of 3.9, and keeps the skirts of a loud
         * tone out of unrelated bands. */
        double a = 2.0 * M_PI / (double)fft_n;
        double c = 2.0 * cos(a);
        double u_prev = 1.0;          /* cos(0) */
        double u = cos(a);

        for (uint32_t k = 0; k < fft_n; k++) {
            double w = 0.5 - 0.5 * u_prev;
            double x = window[2 * k + ch] * w;

            if (cap->detect_hz > 0.0) {
                goertzel_push(&detector, x);
            }
            if (re) {
                re[k] = (float)x;
                im[k] = 0.0f;
            }

            double next = c * u - u_prev;
            u_prev = u;
            u = next;
        }

        cap->tone_dbfs[ch] = (cap->detect_hz > 0.0)
            ? audio_dbfs(goertzel_amplitude(&detector, fft_n), ANALYSIS_FULL_SCALE)
            : AUDIO_DBFS_FLOOR;

        if (!re || bands == 0) {
            continue;
        }

        fft(re, im, fft_n);

        /*
         * Every positive-frequency bin is assigned to the band whose centre it
         * is nearest in log frequency, which is what the half-octave spacing
         * means: the bands tile the spectrum rather than sampling it. Bin
         * zero is skipped -- that is DC, already reported as the mean, and it
         * would otherwise dominate the bottom band on any part with an offset.
         */
        double power[AUDIO_BANDS] = {0};
        for (uint32_t k = 1; k < fft_n / 2; k++) {
            double hz = (double)k * rate / (double)fft_n;
            int band = (int)lrint(2.0 * log2(hz / BAND_BASE_HZ));
            if (band < 0 || band >= (int)bands) {
                continue;
            }
            power[band] += (double)re[k] * re[k] + (double)im[k] * im[k];
        }

        for (uint32_t i = 0; i < bands; i++) {
            cap->band_dbfs[ch][i] =
                audio_dbfs(band_power(power[i], fft_n), ANALYSIS_FULL_SCALE);
        }
    }

    cap->analysed = fft_n;
}

esp_err_t audio_capture_run(const audio_capture_req_t *req, audio_capture_t *cap)
{
    memset(cap, 0, sizeof(*cap));
    cap->tone_dbfs[0] = cap->tone_dbfs[1] = AUDIO_DBFS_FLOOR;

    if (!audio_bus_rx_ready()) {
        cap->error = ESP_ERR_INVALID_STATE;
        return cap->error;
    }

    uint8_t bits = audio_bus_rx_bits();
    size_t frame_bytes = audio_bus_rx_frame_bytes();
    size_t block_frames = audio_bus_block_frames();
    uint32_t rate = audio_bus_rx_rate();
    if (rate == 0) {
        rate = 48000;
    }

    cap->rate = rate;
    cap->bits = bits;
    cap->detect_hz = req->detect_hz;
    /* 16-bit data fills a 16-bit slot; 24- and 32-bit both fill a 32-bit one,
     * so full scale is the slot's either way. */
    cap->full_scale = (bits == 16) ? 32768.0 : 2147483648.0;

    double seconds = req->seconds;
    if (seconds <= 0.0 || seconds > MAX_CAPTURE_SECONDS) {
        seconds = 1.0;
    }
    uint64_t target = (uint64_t)llrint(seconds * rate);

    void *buffer = malloc(block_frames * frame_bytes);
    int16_t *window = NULL;
    float *re = NULL;
    float *im = NULL;
    bool want_window = req->spectrum || req->detect_hz > 0.0;
    if (want_window) {
        window = malloc((size_t)ANALYSIS_FRAMES * 2 * sizeof(int16_t));
    }
    if (req->spectrum) {
        /* Only the spectrum needs the transform; the detector runs off the
         * same window without it, which keeps `audio level` -- called ten
         * times a second -- from allocating 32 KB each time. */
        re = malloc((size_t)ANALYSIS_FRAMES * sizeof(float));
        im = malloc((size_t)ANALYSIS_FRAMES * sizeof(float));
    }
    if (!buffer || (want_window && !window) || (req->spectrum && (!re || !im))) {
        free(buffer);
        free(window);
        free(re);
        free(im);
        cap->error = ESP_ERR_NO_MEM;
        return cap->error;
    }

    esp_err_t err = audio_bus_rx_enable(true);
    if (err != ESP_OK) {
        free(buffer);
        free(window);
        free(re);
        free(im);
        cap->error = err;
        return err;
    }

    slot_accum_t acc[2] = {
        {.min = INT32_MAX, .max = INT32_MIN},
        {.min = INT32_MAX, .max = INT32_MIN},
    };
    int32_t limit = (bits == 16) ? 32767 : 2147483647;
    uint32_t collected = 0;

    while (cap->frames < target) {
        size_t want = block_frames;
        if (cap->frames + want > target) {
            want = (size_t)(target - cap->frames);
        }

        size_t got = 0;
        err = audio_bus_read(buffer, want * frame_bytes, &got, READ_TIMEOUT_MS);
        if (err != ESP_OK) {
            cap->error = err;
            break;
        }
        size_t frames = got / frame_bytes;
        if (frames == 0) {
            cap->overruns++;
            break;
        }
        if (frames < want) {
            cap->overruns++;
        }

        for (size_t i = 0; i < frames; i++) {
            int32_t left = 0;
            int32_t right = 0;
            frame_at(buffer, i, bits, &left, &right);
            slot_push(&acc[0], left, limit);
            slot_push(&acc[1], right, limit);

            if (window && collected < ANALYSIS_FRAMES) {
                /* Held at 16 bits: the window costs half as much RAM, and a
                 * spectrum floor 90 dB down is already below anything a
                 * microphone under test will show. */
                if (bits == 16) {
                    window[2 * collected] = (int16_t)left;
                    window[2 * collected + 1] = (int16_t)right;
                } else {
                    window[2 * collected] = (int16_t)(left >> 16);
                    window[2 * collected + 1] = (int16_t)(right >> 16);
                }
                collected++;
            }
        }

        cap->frames += frames;
    }

    free(buffer);

    if (cap->frames > 0) {
        for (int ch = 0; ch < 2; ch++) {
            double n = (double)cap->frames;
            double mean = (double)acc[ch].sum / n;
            double variance = acc[ch].sum_sq / n - mean * mean;
            cap->min[ch] = acc[ch].min;
            cap->max[ch] = acc[ch].max;
            cap->mean[ch] = mean;
            cap->stdev[ch] = variance > 0.0 ? sqrt(variance) : 0.0;
            cap->peak[ch] = fmax(fabs((double)acc[ch].max),
                                 fabs((double)acc[ch].min));
            cap->clipped[ch] = acc[ch].clipped;
        }
    }

    if (window) {
        analyse(cap, window, collected, re, im);
        free(window);
    }
    free(re);
    free(im);

    return cap->error;
}

/* ------------------------------------------------------------------ */
/* Reporting                                                           */
/* ------------------------------------------------------------------ */

static const char *rx_description(void)
{
    switch (audio_bus_rx_mode()) {
    case AUDIO_RX_PDM: return "PDM microphone";
    case AUDIO_RX_STD: return audio_bus_rx_internal()
                              ? "I2S, looped back inside the chip"
                              : "I2S receive";
    default: return "nothing";
    }
}

void audio_capture_report(const audio_capture_t *cap)
{
    if (cap->error != ESP_OK && cap->frames == 0) {
        STRRES_ERROR(STR_AUDIO_CAPTURE_FAILED, esp_err_to_name(cap->error));
        return;
    }

    STRRES_PRINTF(STR_AUDIO_CAPTURED,
                  (unsigned long long)cap->frames, (unsigned long)cap->rate, rx_description(),
                  cap->bits, cap->full_scale);

    STRRES_PRINTF(STR_AUDIO_HEADER, "slot", "min", "max", "mean", "stdev", "rms", "peak");
    for (int ch = 0; ch < 2; ch++) {
        STRRES_PRINTF(STR_AUDIO_ROW,
                      ch == 0 ? "left" : "right", (long)cap->min[ch], (long)cap->max[ch],
                      cap->mean[ch], cap->stdev[ch],
                      audio_dbfs(cap->stdev[ch], cap->full_scale),
                      audio_dbfs(cap->peak[ch], cap->full_scale));
    }

    /*
     * RMS is quoted about the mean rather than about zero, which is why the
     * column is labelled stdev as well. A microphone with a DC offset is
     * normal and says nothing about the signal; counting that offset as
     * "level" would make a silent part look like a loud one.
     */
    /*
     * The most common way for an input to be wrong is not to be noisy or quiet
     * but to be *stuck*, and that reads confusingly in the table above: RMS
     * floors at -120 dB while peak sits near 0 dB, because peak is measured
     * from zero for headroom and a stuck line is nearly all offset. Naming the
     * case is worth more than leaving it to be deduced.
     *
     * Observed on the Cardputer by clocking the microphone from the wrong pin:
     * every sample came back as -30935 exactly. An unclocked part, a data line
     * that is not connected, and a receiver on the wrong pin all look like
     * this.
     */
    for (int ch = 0; ch < 2; ch++) {
        if (cap->stdev[ch] == 0.0 && cap->frames > 0) {
            STRRES_PRINTF(STR_AUDIO_SLOT_STUCK,
                          ch == 0 ? "left" : "right", (unsigned long long)cap->frames,
                          (long)cap->min[ch]);
        }
    }

    if (cap->clipped[0] || cap->clipped[1]) {
        STRRES_PRINTF(STR_AUDIO_CLIPPED,
                      (unsigned long long)cap->clipped[0], (unsigned long long)cap->clipped[1]);
    }

    if (cap->overruns) {
        STRRES_PRINTF(STR_AUDIO_SHORT_READS, cap->overruns, cap->overruns == 1 ? "" : "s");
    }

    if (cap->error != ESP_OK) {
        STRRES_ERROR(STR_AUDIO_CAPTURE_ENDED_EARLY, esp_err_to_name(cap->error));
    }
}

void audio_capture_spectrum(const audio_capture_t *cap)
{
    if (cap->band_count == 0 || cap->analysed == 0) {
        return;
    }

    /*
     * Every bin lands in exactly one band, so a band is the total power in
     * that half octave rather than the level at its centre frequency. A tone
     * anywhere inside a band shows up at its real level, and the bands sum to
     * the broadband RMS above -- which is the property that lets the two
     * halves of this report be checked against each other.
     */
    STRRES_PRINTF(STR_AUDIO_SPECTRUM_HEADER,
                  (unsigned long long)cap->analysed, (double)cap->rate / (double)cap->analysed);

    const int width = 40;
    const double floor_db = -90.0;

    for (uint32_t i = 0; i < cap->band_count; i++) {
        char bar[2][41];
        for (int ch = 0; ch < 2; ch++) {
            double db = cap->band_dbfs[ch][i];
            int cells = (int)lrint((db - floor_db) / (0.0 - floor_db) * width);
            if (cells < 0) {
                cells = 0;
            }
            if (cells > width) {
                cells = width;
            }
            memset(bar[ch], '#', (size_t)cells);
            bar[ch][cells] = '\0';
        }
        STRRES_PRINTF(STR_AUDIO_SPECTRUM_ROW_L,
                      cap->band_hz[i], bar[0], cap->band_dbfs[0][i]);
        STRRES_PRINTF(STR_AUDIO_SPECTRUM_ROW_R, "", bar[1], cap->band_dbfs[1][i]);
    }
    STRRES_PRINTF(STR_AUDIO_SPECTRUM_NOTE, floor_db);
}
