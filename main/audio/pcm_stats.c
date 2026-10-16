/*
 * Capture statistics for interleaved stereo int16 PCM: the original
 * double-precision version, an integer one, and one on the ESP32-S3 vector unit,
 * kept side by side so `audio pdmbench` can time them and check they agree.
 *
 * Nothing here is specific to PDM; it summarises whatever PCM capture.c hands it.
 * The decimators that used to share a file with it are the pdm_decimator
 * component. Without the vector unit (every chip but the S3) the "pie" entry
 * point runs the integer version.
 */
#include <stdlib.h>
#include <string.h>
#include "app_bringup.h"
#include "audio.h"

void audio_stats16_init(audio_stats16_t *s)
{
    memset(s, 0, sizeof(*s));
    s->min[0] = s->min[1] = INT32_MAX;
    s->max[0] = s->max[1] = INT32_MIN;
}

/* What capture.c did originally: per sample, sum of squares in a double
 * (software floating point on this chip). */
void audio_stats16_ref(audio_stats16_t *s, const int16_t *x, size_t frames, int32_t limit)
{
    for (size_t i = 0; i < frames; i++) {
        for (int c = 0; c < 2; c++) {
            int32_t v = x[2 * i + c];
            if (v < s->min[c]) s->min[c] = v;
            if (v > s->max[c]) s->max[c] = v;
            s->sum[c] += v;
            s->sumsq_f[c] += (double)v * (double)v;
            if (v >= limit || v <= -limit) s->clipped[c]++;
        }
    }
}

void audio_stats16_i64(audio_stats16_t *s, const int16_t *x, size_t frames, int32_t limit)
{
    for (size_t i = 0; i < frames; i++) {
        for (int c = 0; c < 2; c++) {
            int32_t v = x[2 * i + c];
            if (v < s->min[c]) s->min[c] = v;
            if (v > s->max[c]) s->max[c] = v;
            s->sum[c] += v;
            s->sumsq[c] += (uint64_t)((int64_t)v * v);
            if (v >= limit || v <= -limit) s->clipped[c]++;
        }
    }
}

#if CONFIG_IDF_TARGET_ESP32S3

/*
 * PIE: eight lanes are four frames, even lanes left and odd lanes right.
 * Per-lane min/max keep the extremes; the sums and sums of squares go through
 * the 40-bit ACCX with a lane mask selecting one channel. A 256-frame chunk
 * keeps ACCX (+-5.5e11) clear of overflow: 256 * 2^30 = 2.7e11.
 */
static inline int64_t accx_read(void)
{
    uint32_t lo, hi;
    asm volatile("rur.accx_0 %0\nrur.accx_1 %1" : "=r"(lo), "=r"(hi));
    return (int64_t)((uint64_t)(int64_t)(int8_t)hi << 32 | lo);
}

static void stats16_pie_vectors(audio_stats16_t *s, const int16_t *x, size_t vecs)
{
    static const int16_t maskl[8] __attribute__((aligned(16))) = {-1, 0, -1, 0, -1, 0, -1, 0};
    static const int16_t maskr[8] __attribute__((aligned(16))) = {0, -1, 0, -1, 0, -1, 0, -1};
    static const int16_t onel[8] __attribute__((aligned(16))) = {1, 0, 1, 0, 1, 0, 1, 0};
    static const int16_t oner[8] __attribute__((aligned(16))) = {0, 1, 0, 1, 0, 1, 0, 1};
    int16_t mn[8] __attribute__((aligned(16)));
    int16_t mx[8] __attribute__((aligned(16)));
    const int16_t *p = x;
    size_t n = vecs;

    /* x is 16-byte aligned by the caller's contract, so vld never straddles. */
    asm volatile(
        "ee.vld.128.ip q1, %[p], 16\n"
        "ee.vmax.s16 q2, q1, q1\n"
        "ee.vmin.s16 q3, q1, q1\n"
        "addi %[n], %[n], -1\n"
        "beqz %[n], 2f\n"
        "1:\n"
        "ee.vld.128.ip q0, %[p], 16\n"
        "ee.vmax.s16 q2, q2, q0\n"
        "ee.vmin.s16 q3, q3, q0\n"
        "addi %[n], %[n], -1\n"
        "bnez %[n], 1b\n"
        "2:\n"
        "ee.vst.128.ip q2, %[mx], 0\n"
        "ee.vst.128.ip q3, %[mn], 0\n"
        : [p] "+r"(p), [n] "+r"(n)
        : [mx] "r"(mx), [mn] "r"(mn)
        : "memory");
    for (int l = 0; l < 8; l++) {
        int c = l & 1;
        if (mn[l] < s->min[c]) s->min[c] = mn[l];
        if (mx[l] > s->max[c]) s->max[c] = mx[l];
    }

    /* One ACCX pass per quantity: lane mask in q3, samples in q0. */
    #define ACC_PASS(RESULT, MASK, BODY)                                      \
        do {                                                                  \
            const int16_t *q = x;                                             \
            size_t m = vecs;                                                  \
            asm volatile(                                                     \
                "ee.zero.accx\n"                                              \
                "ee.vld.128.ip q3, %[mask], 0\n"                              \
                "1:\n"                                                        \
                "ee.vld.128.ip q0, %[q], 16\n"                                \
                BODY                                                          \
                "addi %[m], %[m], -1\n"                                       \
                "bnez %[m], 1b\n"                                             \
                : [q] "+r"(q), [m] "+r"(m)                                    \
                : [mask] "r"(MASK)                                            \
                : "memory");                                                  \
            RESULT = accx_read();                                             \
        } while (0)
    int64_t r;
    ACC_PASS(r, onel, "ee.vmulas.s16.accx q0, q3\n");
    s->sum[0] += r;
    ACC_PASS(r, oner, "ee.vmulas.s16.accx q0, q3\n");
    s->sum[1] += r;
    ACC_PASS(r, maskl, "ee.andq q4, q0, q3\nee.vmulas.s16.accx q4, q4\n");
    s->sumsq[0] += (uint64_t)r;
    ACC_PASS(r, maskr, "ee.andq q4, q0, q3\nee.vmulas.s16.accx q4, q4\n");
    s->sumsq[1] += (uint64_t)r;
    #undef ACC_PASS
}

/* x must be 16-byte aligned. Any frame count; the tail runs scalar. */
void audio_stats16_pie(audio_stats16_t *s, const int16_t *x, size_t frames, int32_t limit)
{
    const size_t chunk = 256;
    size_t done = 0;
    while (frames - done >= 4) {
        size_t n = frames - done;
        if (n > chunk) {
            n = chunk;
        }
        n &= ~(size_t)3;
        stats16_pie_vectors(s, x + 2 * done, n / 4);
        done += n;
    }
    if (done < frames) {
        audio_stats16_i64(s, x + 2 * done, frames - done, INT32_MAX);
    }
    /* Clipping needs a per-sample compare; only pay for it when an extreme
     * actually reached the limit. */
    if (s->max[0] >= limit || s->min[0] <= -limit || s->max[1] >= limit || s->min[1] <= -limit) {
        for (size_t i = 0; i < frames; i++) {
            for (int c = 0; c < 2; c++) {
                int32_t v = x[2 * i + c];
                if (v >= limit || v <= -limit) s->clipped[c]++;
            }
        }
    }
}

#else /* no vector unit */

void audio_stats16_pie(audio_stats16_t *s, const int16_t *x, size_t frames, int32_t limit)
{
    audio_stats16_i64(s, x, frames, limit);
}

#endif /* CONFIG_IDF_TARGET_ESP32S3 */
