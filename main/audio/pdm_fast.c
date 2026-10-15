/*
 * Faster PDM decimation and capture statistics, kept beside the originals so
 * they can be timed and checked against them (`audio pdmbench`).
 *
 * Decimator (pdm_dec2): the same 4.8 MHz -> 300 kHz job as pdm_dec, in two
 * stages instead of one long table walk.
 *
 *   stage 1  sinc^4 over bytes, R=8: four 256-entry int16 tables, four lookups
 *            per byte, giving 600 kHz samples of at most +-4096;
 *   stage 2  24-tap FIR, decimate by 2, a dot product on the S3's PIE vector
 *            unit (8 x 16-bit multiply-accumulate per instruction).
 *
 * It is a different filter from pdm_dec's sinc^6 x16, not a restructuring of
 * it: a bit-exact split would still need 12 lookups per output. The stage-2
 * coefficients were designed (Kaiser, beta 6, 95 kHz cut at 600 kHz) so the
 * overall response matches within about 2 dB to 80 kHz on real microphone data.
 */
#include <stdlib.h>
#include <string.h>
#include "app_bringup.h"
#include "audio.h"
#include "esp_heap_caps.h"

#define S1_TAPS   24            /* stage-2 FIR length: three 8-lane vectors */
#define S1_HIST   24            /* samples carried between calls */
#define S1_PAD    32            /* readable slack past the end: realign loads */

static const int16_t s2_coef[S1_TAPS] __attribute__((aligned(16))) = {
    -12, -40, -3, 176, 348, 113, -709, -1459, -767, 2227, 6612, 9899,
    9897, 6612, 2227, -767, -1459, -709, 113, 348, 176, -3, -40, -12,
};

struct pdm_dec2 {
    int channels;
    int16_t stage1[4][256];             /* by byte age, 0 newest */
    uint8_t prev[8][3];                 /* last three bytes per channel, newest first */
    int16_t *buf;                       /* [channels][S1_HIST + cap + S1_PAD] */
    size_t cap;                         /* new samples per channel it can hold */
    int16_t hist[8][S1_HIST] __attribute__((aligned(16)));
    bool use_pie;
};

static inline int32_t dot24_scalar(const int16_t *x, const int16_t *c)
{
    int32_t acc = 0;
    for (int k = 0; k < S1_TAPS; k++) {
        acc += (int32_t)x[k] * c[k];
    }
    return acc;
}

/* x need not be aligned; c must be 16-byte aligned. Reads up to 48 bytes from
 * x rounded down to 16, which the buffer padding covers. */
static inline int32_t dot24_pie(const int16_t *x, const int16_t *c)
{
    int32_t lo;
    asm volatile(
        "ee.zero.accx\n"
        "ee.ld.128.usar.ip q0, %[x], 16\n"
        "ee.ld.128.usar.ip q1, %[x], 16\n"
        "ee.vld.128.ip q4, %[c], 16\n"
        "ee.src.q q5, q0, q1\n"
        "ee.vmulas.s16.accx q4, q5\n"
        "ee.ld.128.usar.ip q0, %[x], 16\n"
        "ee.vld.128.ip q4, %[c], 16\n"
        "ee.src.q q5, q1, q0\n"
        "ee.vmulas.s16.accx q4, q5\n"
        "ee.ld.128.usar.ip q1, %[x], 16\n"
        "ee.vld.128.ip q4, %[c], 16\n"
        "ee.src.q q5, q0, q1\n"
        "ee.vmulas.s16.accx q4, q5\n"
        "rur.accx_0 %[lo]\n"
        : [x] "+r"(x), [c] "+r"(c), [lo] "=r"(lo)
        :
        : "memory");
    return lo;
}

pdm_dec2_t *pdm_dec2_new(int channels, bool use_pie)
{
    if (channels < 1 || channels > 8) {
        return NULL;
    }
    pdm_dec2_t *d = calloc(1, sizeof(*d));
    if (!d) {
        return NULL;
    }
    d->channels = channels;
    d->use_pie = use_pie;

    /* box8 convolved four times: 29 taps, gain 8^4 = 4096. */
    int h[32] = {0};
    h[0] = 1;
    int len = 1;
    for (int stage = 0; stage < 4; stage++) {
        int t[32] = {0};
        for (int i = 0; i < len; i++) {
            for (int j = 0; j < 8; j++) {
                t[i + j] += h[i];
            }
        }
        len += 7;
        memcpy(h, t, sizeof(h));
    }
    for (int a = 0; a < 4; a++) {
        for (int v = 0; v < 256; v++) {
            int sum = 0;
            for (int bit = 0; bit < 8; bit++) {
                int pos = 8 * a + bit;
                int w = pos < len ? h[pos] : 0;
                sum += (v & (1 << bit)) ? w : -w;
            }
            d->stage1[a][v] = (int16_t)sum;
        }
    }
    return d;
}

void pdm_dec2_free(pdm_dec2_t *d)
{
    if (d) {
        free(d->buf);
        free(d);
    }
}

size_t pdm_dec2_process(pdm_dec2_t *d, const uint16_t *raw, size_t frames, int16_t *out)
{
    const int ch = d->channels;
    const size_t n1 = 2 * frames;                    /* stage-1 samples per channel */

    if (n1 > d->cap) {
        free(d->buf);
        d->buf = heap_caps_aligned_alloc(16, (size_t)ch * (S1_HIST + n1 + S1_PAD) * sizeof(int16_t),
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!d->buf) {
            d->cap = 0;
            return 0;
        }
        d->cap = n1;
    }
    const size_t cstride = S1_HIST + d->cap + S1_PAD;

    size_t produced = 0;
    for (int c = 0; c < ch; c++) {
        int16_t *x = d->buf + (size_t)c * cstride;
        memcpy(x, d->hist[c], S1_HIST * sizeof(int16_t));
        int16_t *s1 = x + S1_HIST;

        /* p1..p3 are the three bytes before the current one, newest first. */
        uint8_t p1 = d->prev[c][0], p2 = d->prev[c][1], p3 = d->prev[c][2];
        const int16_t *t0 = d->stage1[0], *t1 = d->stage1[1];
        const int16_t *t2 = d->stage1[2], *t3 = d->stage1[3];
        for (size_t f = 0; f < frames; f++) {
            uint16_t w = raw[f * ch + c];
            uint8_t hi = (uint8_t)(w >> 8), lo = (uint8_t)w;
            s1[2 * f] = (int16_t)(t0[hi] + t1[p1] + t2[p2] + t3[p3]);
            s1[2 * f + 1] = (int16_t)(t0[lo] + t1[hi] + t2[p1] + t3[p2]);
            p3 = p1;
            p2 = hi;
            p1 = lo;
        }
        d->prev[c][0] = p1;
        d->prev[c][1] = p2;
        d->prev[c][2] = p3;

        /* An output after every second stage-1 sample: the low byte of a word. */
        size_t o = 0;
        for (size_t j = 1; j < n1; j += 2) {
            const int16_t *win = s1 + j - (S1_TAPS - 1);
            int32_t acc = d->use_pie ? dot24_pie(win, s2_coef) : dot24_scalar(win, s2_coef);
            acc >>= 12;
            if (acc > 32767) acc = 32767;
            if (acc < -32768) acc = -32768;
            out[o * ch + c] = (int16_t)acc;
            o++;
        }
        produced = o;
        memcpy(d->hist[c], s1 + n1 - S1_HIST, S1_HIST * sizeof(int16_t));
    }
    return produced;
}

/* ------------------------------------------------------------------ */
/* Capture statistics                                                  */
/* ------------------------------------------------------------------ */

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
