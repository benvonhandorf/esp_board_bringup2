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

/* Amplitude (dBFS, full-scale sine = 0) of one tone in a slot of a stereo buffer. */
static double tone_dbfs(const int16_t *pcm, size_t frames, int slot, double hz, double fs)
{
    double re = 0.0, im = 0.0;
    size_t n = 0;
    for (size_t i = SKIP; i < frames; i++, n++) {
        double a = 2.0 * M_PI * hz * (double)i / fs;
        double v = pcm[2 * i + slot];
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

int cmd_audio_pdm_bench(int argc, char **argv)
{
    int frames = 2048;
    if (argc >= 2 && (cli_parse_int_arg(argv[1], &frames) < 0 || frames < 512 || frames > 8192)) {
        STRRES_ERROR(STR_AUDIO_BENCH_RANGE);
        return -1;
    }
    const size_t n = (size_t)frames;
    uint16_t *raw = heap_caps_malloc(n * 4, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *o1 = heap_caps_aligned_alloc(16, n * 4 + 64, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *o2s = heap_caps_aligned_alloc(16, n * 4 + 64, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *o2p = heap_caps_aligned_alloc(16, n * 4 + 64, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    pdm_dec_t *d1 = pdm_dec_new(2, 16, false);
    pdm_dec2_t *d2s = pdm_dec2_new(2, false);
    pdm_dec2_t *d2p = pdm_dec2_new(2, true);
    if (!raw || !o1 || !o2s || !o2p || !d1 || !d2s || !d2p) {
        STRRES_ERROR(STR_AUDIO_BENCH_NO_MEMORY);
        free(raw); free(o1); free(o2s); free(o2p);
        pdm_dec_free(d1); pdm_dec2_free(d2s); pdm_dec2_free(d2p);
        return -1;
    }

    modulate(raw, n, 2);

    /* One warm-up pass so the first timed one does not pay for allocation. */
    pdm_dec2_process(d2s, raw, 8, o2s);
    pdm_dec2_process(d2p, raw, 8, o2p);
    pdm_dec2_free(d2s);
    pdm_dec2_free(d2p);
    d2s = pdm_dec2_new(2, false);
    d2p = pdm_dec2_new(2, true);

    uint32_t t = cycles_now();
    size_t p1 = pdm_dec_process(d1, raw, n, o1);
    uint32_t c1 = cycles_now() - t;
    t = cycles_now();
    size_t p2s = pdm_dec2_process(d2s, raw, n, o2s);
    uint32_t c2s = cycles_now() - t;
    t = cycles_now();
    size_t p2p = pdm_dec2_process(d2p, raw, n, o2p);
    uint32_t c2p = cycles_now() - t;

    if (p1 == 0 || p2s == 0 || p2p == 0) {
        STRRES_ERROR(STR_AUDIO_BENCH_NO_MEMORY);
        free(raw); free(o1); free(o2s); free(o2p);
        pdm_dec_free(d1); pdm_dec2_free(d2s); pdm_dec2_free(d2p);
        return -1;
    }
    STRRES_PRINTF(STR_AUDIO_BENCH_DECIMATORS, frames, (double)c1 / (double)n,
                  (double)c2s / (double)n, (double)c2p / (double)n);

    int maxdiff = 0;
    for (size_t i = 0; i < 2 * p2s && i < 2 * p2p; i++) {
        int dd = abs((int)o2s[i] - (int)o2p[i]);
        if (dd > maxdiff) maxdiff = dd;
    }
    if (maxdiff == 0 && p2s == p2p) {
        STRRES_PRINTF(STR_AUDIO_BENCH_PIE_MATCH);
    } else {
        STRRES_PRINTF(STR_AUDIO_BENCH_PIE_MISMATCH, maxdiff);
    }

    for (int slot = 0; slot < 2; slot++) {
        STRRES_PRINTF(STR_AUDIO_BENCH_TONES, slot,
                      tone_dbfs(o1, p1, slot, TONE1_HZ, 300000.0),
                      tone_dbfs(o2p, p2p, slot, TONE1_HZ, 300000.0),
                      tone_dbfs(o1, p1, slot, TONE2_HZ, 300000.0),
                      tone_dbfs(o2p, p2p, slot, TONE2_HZ, 300000.0));
    }

    /* Statistics, on the sinc^6 output. */
    audio_stats16_t sr, si, sp;
    audio_stats16_init(&sr);
    audio_stats16_init(&si);
    audio_stats16_init(&sp);
    t = cycles_now();
    audio_stats16_ref(&sr, o1, p1, 32767);
    uint32_t cr = cycles_now() - t;
    t = cycles_now();
    audio_stats16_i64(&si, o1, p1, 32767);
    uint32_t ci = cycles_now() - t;
    t = cycles_now();
    audio_stats16_pie(&sp, o1, p1, 32767);
    uint32_t cp = cycles_now() - t;
    STRRES_PRINTF(STR_AUDIO_BENCH_STATS, (int)p1, (double)cr / (double)p1,
                  (double)ci / (double)p1, (double)cp / (double)p1);

    bool same = true;
    for (int c = 0; c < 2; c++) {
        same = same && sr.min[c] == sp.min[c] && sr.max[c] == sp.max[c] &&
               sr.sum[c] == sp.sum[c] && si.sumsq[c] == sp.sumsq[c] &&
               (double)si.sumsq[c] == sr.sumsq_f[c] && sr.clipped[c] == sp.clipped[c];
    }
    if (same) {
        STRRES_PRINTF(STR_AUDIO_BENCH_STATS_MATCH);
    } else {
        STRRES_PRINTF(STR_AUDIO_BENCH_STATS_MISMATCH);
    }

    free(raw); free(o1); free(o2s); free(o2p);
    pdm_dec_free(d1); pdm_dec2_free(d2s); pdm_dec2_free(d2p);
    return same && maxdiff == 0 ? 0 : -1;
}
