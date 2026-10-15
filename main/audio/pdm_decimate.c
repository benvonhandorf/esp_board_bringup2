/*
 * Software PDM-to-PCM decimation for the raw receiver.
 *
 * The chip's hardware filter divides by a fixed 64, which at a microphone
 * clock of 4.8 MHz gives 75 kHz PCM -- not enough bandwidth for an ultrasonic
 * part. The raw receiver hands over the undecimated bits instead and this
 * turns them into PCM at 4.8 MHz / R.
 *
 * The filter is a sixth-order sinc (CIC) decimating by R. R is a multiple of
 * 16 so every output lands on a word boundary, which is what makes this cheap:
 * the impulse response is six boxcars convolved, so one output is a fixed
 * weighted sum over the last few words, and a weighted sum over the bits of a
 * byte is one table lookup. R=16 is twelve lookups per output per channel.
 *
 * Droop is the price of a plain sinc: about 6 dB at 80 kHz for R=16 at a
 * 4.8 MHz clock, which is why the spectrum is a relative measure.
 */
#include <stdlib.h>
#include <string.h>
#include "app_bringup.h"
#include "audio.h"

#define CIC_ORDER 6

struct pdm_dec {
    int channels;
    int r_words;             /* R / 16: input words per output */
    int nwords;              /* history length in words */
    int shift;               /* output scaling: R^4 -> 15 bits */
    int32_t *lut;            /* [nwords][2 bytes][256] */
    uint16_t *hist;          /* [channels][nwords], newest at hist_pos */
    int hist_pos;
    int phase;               /* words since the last output */
};

pdm_dec_t *pdm_dec_new(int channels, int r, bool lsb_first)
{
    if (channels < 1 || r < 16 || r > 64 || (r % 16) != 0) {
        return NULL;
    }

    /* Impulse response of CIC_ORDER cascaded boxcars of length r. */
    int len = CIC_ORDER * (r - 1) + 1;
    int32_t *h = calloc((size_t)len, sizeof(*h));
    if (!h) {
        return NULL;
    }
    int32_t *tmp = calloc((size_t)len, sizeof(*tmp));
    if (!tmp) {
        free(h);
        return NULL;
    }
    h[0] = 1;
    int cur = 1;
    for (int stage = 0; stage < CIC_ORDER; stage++) {
        memset(tmp, 0, (size_t)len * sizeof(*tmp));
        for (int i = 0; i < cur; i++) {
            for (int j = 0; j < r; j++) {
                tmp[i + j] += h[i];
            }
        }
        cur += r - 1;
        memcpy(h, tmp, (size_t)cur * sizeof(*h));
    }
    free(tmp);

    pdm_dec_t *d = calloc(1, sizeof(*d));
    if (!d) {
        free(h);
        return NULL;
    }
    d->channels = channels;
    d->r_words = r / 16;
    d->nwords = (len + 15) / 16;
    d->hist_pos = 0;

    /* Gain is r^4; scale so a full-scale +-1 stream is +-32767. */
    int64_t gain = 1;
    for (int i = 0; i < CIC_ORDER; i++) {
        gain *= r;
    }
    d->shift = 0;
    while ((gain >> d->shift) > 32768) {
        d->shift++;
    }

    d->lut = malloc((size_t)d->nwords * 2 * 256 * sizeof(int32_t));
    d->hist = calloc((size_t)channels * d->nwords, sizeof(uint16_t));
    if (!d->lut || !d->hist) {
        pdm_dec_free(d);
        free(h);
        return NULL;
    }

    /*
     * Word age a holds samples at positions 16a..16a+15 back from the newest.
     * MSB-first means the MSB is the oldest sample in the word, so bit b is
     * position 16a + b; LSB-first reverses that. Bit set is +1, clear is -1.
     */
    for (int a = 0; a < d->nwords; a++) {
        for (int half = 0; half < 2; half++) {          /* 0: low byte, 1: high */
            for (int v = 0; v < 256; v++) {
                int32_t sum = 0;
                for (int bit = 0; bit < 8; bit++) {
                    int b = half * 8 + bit;             /* bit within the word */
                    int pos = 16 * a + (lsb_first ? 15 - b : b);
                    int32_t w = pos < len ? h[pos] : 0;
                    sum += (v & (1 << bit)) ? w : -w;
                }
                d->lut[((size_t)a * 2 + half) * 256 + v] = sum;
            }
        }
    }
    free(h);
    return d;
}

void pdm_dec_free(pdm_dec_t *d)
{
    if (!d) {
        return;
    }
    free(d->lut);
    free(d->hist);
    free(d);
}

size_t pdm_dec_process(pdm_dec_t *d, const uint16_t *raw, size_t frames, int16_t *out)
{
    size_t produced = 0;
    const int n = d->nwords;
    const int ch = d->channels;
    const int shift = d->shift;
    const int32_t *lut = d->lut;

    for (size_t f = 0; f < frames; f++) {
        if (++d->hist_pos == n) {
            d->hist_pos = 0;
        }
        for (int c = 0; c < ch; c++) {
            d->hist[c * n + d->hist_pos] = raw[f * ch + c];
        }
        if (++d->phase < d->r_words) {
            continue;
        }
        d->phase = 0;

        for (int c = 0; c < ch; c++) {
            const uint16_t *hist = d->hist + c * n;
            /* The sum of every weight is R^order, which is below 2^31 for
             * every R this accepts at order 6 up to R=64. */
            int32_t acc = 0;
            int idx = d->hist_pos;
            const int32_t *t = lut;
            for (int a = 0; a < n; a++) {
                uint16_t w = hist[idx];
                acc += t[w & 0xFF] + t[256 + (w >> 8)];
                t += 512;
                if (--idx < 0) {
                    idx = n - 1;
                }
            }
            acc >>= shift;
            if (acc > 32767) acc = 32767;
            if (acc < -32768) acc = -32768;
            out[produced * ch + c] = (int16_t)acc;
        }
        produced++;
    }
    return produced;
}

int pdm_dec_r_words(const pdm_dec_t *d)
{
    return d->r_words;
}
