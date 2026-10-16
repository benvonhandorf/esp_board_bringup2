/*
 * `audio pdmbench`: time the software PDM decimators and the capture
 * statistics against each other and cross-check them, with no microphone.
 *
 * The input is a synthetic 4.8 MHz PDM stream: a second-order delta-sigma
 * modulator fed two tones. Its shaped noise is not the microphone's, so the
 * timings are exact and the tone amplitudes are the comparison; use real
 * microphone data (`pdmbits dump`, offline) for noise-floor comparisons.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "app_bringup.h"
#include "audio.h"
#include "pdm_decimator.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"

#define TONE1_HZ 10000.0
#define TONE2_HZ 50000.0
#define PDM_FS   4800000.0
#define SKIP     64        /* output frames dropped for filter settling */

static void modulate(uint16_t *raw, size_t frames, int channels)
{
    for (int c = 0; c < channels; c++) {
        float i1 = 0.0f, i2 = 0.0f, y = 1.0f;
        const float ph = 0.7f * (float)c;
        for (size_t f = 0; f < frames; f++) {
            uint16_t w = 0;
            for (int b = 0; b < 16; b++) {
                double n = (double)(f * 16 + (size_t)b);
                float x = 0.25f * sinf((float)(2.0 * M_PI * TONE1_HZ * n / PDM_FS) + ph) +
                          0.15f * sinf((float)(2.0 * M_PI * TONE2_HZ * n / PDM_FS) + ph);
                i1 += x - y;
                i2 += i1 - y;
                y = i2 >= 0.0f ? 1.0f : -1.0f;
                w = (uint16_t)((w << 1) | (y > 0.0f ? 1 : 0));
            }
            raw[f * channels + c] = w;
        }
    }
}

/* Amplitude (dBFS, full-scale sine = 0) of one tone in one channel of an
 * interleaved buffer. */
static double tone_dbfs(const int16_t *pcm, size_t frames, int channels, int slot,
                        double hz, double fs)
{
    double re = 0.0, im = 0.0;
    size_t n = 0;
    for (size_t i = SKIP; i < frames; i++, n++) {
        double a = 2.0 * M_PI * hz * (double)i / fs;
        double v = pcm[(size_t)channels * i + (size_t)slot];
        re += v * cos(a);
        im += v * sin(a);
    }
    double amp = 2.0 * sqrt(re * re + im * im) / (double)n;
    return 20.0 * log10(amp / 32768.0 + 1e-12);
}

static uint32_t cycles_now(void)
{
    return esp_cpu_get_cycle_count();
}

#define BENCH_CPU_MHZ CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ
#define PCM_RATE_HZ   300000.0

/* Cycles per frame as a share of one core at the 300 kHz output rate. */
static double core_pct(double cycles_per_frame)
{
    return 100.0 * cycles_per_frame * PCM_RATE_HZ / (BENCH_CPU_MHZ * 1e6);
}

int cmd_audio_pdm_bench(int argc, char **argv)
{
    int frames = 1024;
    int channels = 2;
    if (argc >= 2 && (cli_parse_int_arg(argv[1], &frames) < 0 || frames < 256 || frames > 4096)) {
        STRRES_ERROR(STR_AUDIO_BENCH_RANGE);
        return -1;
    }
    if (argc >= 3 && (cli_parse_int_arg(argv[2], &channels) < 0 || channels < 2 ||
                      channels > 8 || (channels & 1))) {
        STRRES_ERROR(STR_AUDIO_BENCH_CHANNELS);
        return -1;
    }
    const size_t n = (size_t)frames;
    const size_t fbytes = (size_t)channels * 2;
    int rc = -1;
    uint16_t *raw = heap_caps_malloc(n * fbytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *o1 = heap_caps_aligned_alloc(16, n * fbytes + 64, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *o2s = heap_caps_aligned_alloc(16, n * fbytes + 64, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *o2p = heap_caps_aligned_alloc(16, n * fbytes + 64, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *pair = heap_caps_aligned_alloc(16, n * 4 + 64, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    pdm_decimator_t *d1 = NULL;
    pdm_decimator_config_t cfg1 = {
        .filter = PDM_DECIMATOR_SINC6, .channels = channels, .ratio = 16,
    };
    if (pdm_decimator_create(&cfg1, &d1) != ESP_OK) {
        d1 = NULL;
    }
    pdm_decimator_t *d2 = NULL;
    size_t p1 = 0, p2s = 0, p2p = 0;
    uint32_t c1 = 0, c2s = 0, c2p = 0;
    if (!raw || !o1 || !o2s || !o2p || !pair || !d1) {
        STRRES_ERROR(STR_AUDIO_BENCH_NO_MEMORY);
        goto done;
    }

    modulate(raw, n, channels);

    uint32_t t = cycles_now();
    pdm_decimator_process(d1, raw, n, o1, &p1);
    c1 = cycles_now() - t;

    /* One decimator instance at a time: each holds a per-channel work buffer
     * of its own, and eight channels of it do not fit twice beside the rest.
     * Pass 0 is the portable C stage 2, pass 1 the vector one. */
    for (int pass = 0; pass < 2; pass++) {
        pdm_decimator_config_t cfg2 = {
            .filter = PDM_DECIMATOR_TWO_STAGE, .channels = channels, .allow_simd = pass == 1,
        };
        if (pdm_decimator_create(&cfg2, &d2) != ESP_OK) {
            d2 = NULL;
            STRRES_ERROR(STR_AUDIO_BENCH_NO_MEMORY);
            goto done;
        }
        /* Size the work buffer first so the timed call does not allocate. */
        if (pdm_decimator_reserve(d2, n) != ESP_OK) {
            STRRES_ERROR(STR_AUDIO_BENCH_NO_MEMORY);
            goto done;
        }
        t = cycles_now();
        size_t p = 0;
        esp_err_t derr = pdm_decimator_process(d2, raw, n, pass ? o2p : o2s, &p);
        uint32_t c = cycles_now() - t;
        pdm_decimator_destroy(d2);
        d2 = NULL;
        if (derr != ESP_OK) {
            STRRES_ERROR(STR_AUDIO_BENCH_NO_MEMORY);
            goto done;
        }
        if (pass) { p2p = p; c2p = c; } else { p2s = p; c2s = c; }
    }

    if (p1 == 0 || p2s == 0 || p2p == 0) {
        STRRES_ERROR(STR_AUDIO_BENCH_NO_MEMORY);
        goto done;
    }
    double f1 = (double)c1 / (double)n, fs_ = (double)c2s / (double)n;
    double fp = (double)c2p / (double)n;
    STRRES_PRINTF(STR_AUDIO_BENCH_DECIMATORS, channels, frames, f1, core_pct(f1), fs_, fp,
                  core_pct(fp));

    int maxdiff = 0;
    for (size_t i = 0; i < (size_t)channels * p2s && i < (size_t)channels * p2p; i++) {
        int dd = abs((int)o2s[i] - (int)o2p[i]);
        if (dd > maxdiff) maxdiff = dd;
    }
    if (maxdiff == 0 && p2s == p2p) {
        STRRES_PRINTF(STR_AUDIO_BENCH_PIE_MATCH);
    } else {
        STRRES_PRINTF(STR_AUDIO_BENCH_PIE_MISMATCH, maxdiff);
    }

    for (int slot = 0; slot < channels; slot++) {
        STRRES_PRINTF(STR_AUDIO_BENCH_TONES, slot,
                      tone_dbfs(o1, p1, channels, slot, TONE1_HZ, PCM_RATE_HZ),
                      tone_dbfs(o2p, p2p, channels, slot, TONE1_HZ, PCM_RATE_HZ),
                      tone_dbfs(o1, p1, channels, slot, TONE2_HZ, PCM_RATE_HZ),
                      tone_dbfs(o2p, p2p, channels, slot, TONE2_HZ, PCM_RATE_HZ));
    }

    /* Statistics are stereo; run one pass per channel pair and add them up. */
    audio_stats16_t sr, si, sp;
    uint32_t cr = 0, ci = 0, cp = 0;
    bool same = true;
    for (int pr = 0; pr < channels / 2; pr++) {
        for (size_t i = 0; i < p1; i++) {
            pair[2 * i] = o1[(size_t)channels * i + (size_t)(2 * pr)];
            pair[2 * i + 1] = o1[(size_t)channels * i + (size_t)(2 * pr) + 1];
        }
        audio_stats16_init(&sr);
        audio_stats16_init(&si);
        audio_stats16_init(&sp);
        t = cycles_now();
        audio_stats16_ref(&sr, pair, p1, 32767);
        cr += cycles_now() - t;
        t = cycles_now();
        audio_stats16_i64(&si, pair, p1, 32767);
        ci += cycles_now() - t;
        t = cycles_now();
        audio_stats16_pie(&sp, pair, p1, 32767);
        cp += cycles_now() - t;
        for (int c = 0; c < 2; c++) {
            same = same && sr.min[c] == sp.min[c] && sr.max[c] == sp.max[c] &&
                   sr.sum[c] == sp.sum[c] && si.sumsq[c] == sp.sumsq[c] &&
                   (double)si.sumsq[c] == sr.sumsq_f[c] && sr.clipped[c] == sp.clipped[c];
        }
    }
    double sfr = (double)cr / (double)p1, sfi = (double)ci / (double)p1;
    double sfp = (double)cp / (double)p1;
    STRRES_PRINTF(STR_AUDIO_BENCH_STATS, channels, (int)p1, sfr, sfi, sfp, core_pct(sfp));
    if (same) {
        STRRES_PRINTF(STR_AUDIO_BENCH_STATS_MATCH);
    } else {
        STRRES_PRINTF(STR_AUDIO_BENCH_STATS_MISMATCH);
    }
    rc = same && maxdiff == 0 ? 0 : -1;

done:
    free(raw); free(o1); free(o2s); free(o2p); free(pair);
    pdm_decimator_destroy(d1);
    pdm_decimator_destroy(d2);
    return rc;
}
