#ifndef AUDIO_H
#define AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "strres.h"

/*
 * Audio bring-up: an I2S transport, a test signal generator, and codec drivers.
 *
 * The layering mirrors the rest of this project. i2s_bus.c owns the peripheral
 * the way i2c.c owns the I2C bus, and part drivers build on it rather than
 * opening their own -- see i2c_require_bus() / i2c_device_handle() for the
 * pattern being followed.
 *
 * Deliberately no I2S driver types appear in this header. i2s_bus.c is the only
 * file that includes driver/i2s_std.h, which is what lets the whole module be
 * compiled out on a chip with no I2S peripheral without touching anything else.
 */

/* ------------------------------------------------------------------ */
/* Stream format                                                       */
/* ------------------------------------------------------------------ */

/*
 * Shared by playback and capture, so a microphone reuses the bus configuration
 * rather than describing its own.
 */
typedef struct {
    uint32_t rate_hz;      /* requested sample rate */
    uint8_t bits;          /* 16, 24 or 32 valid data bits per sample */
    int mclk_pin;          /* -1 when the board does not wire one */
    int mclk_multiple;     /* 256 or 384; 0 picks one to suit the bit width */
} audio_format_t;

/*
 * The wire is always two slots wide.
 *
 * I2S_SLOT_MODE_MONO exists, but it changes the frame the codec sees, and a
 * stereo part fed one-slot frames plays everything an octave down at half
 * speed. Every part this tool targets expects a two-slot frame, so the slot
 * count is not a user-visible option; put the signal in one channel with the
 * left/right argument to `audio tone` instead, which is also how you find out
 * which slot a mono amplifier is listening to.
 */

/* Which directions a part can work in. */
typedef enum {
    AUDIO_DIR_TX = 1 << 0,
    AUDIO_DIR_RX = 1 << 1,
} audio_dir_t;

/*
 * How the receiver is wired, which is not a detail the analysis code can
 * ignore: a PDM microphone brings its own clock and its own sample width, and
 * an I2S receiver shares the transmitter's.
 */
typedef enum {
    AUDIO_RX_NONE,
    AUDIO_RX_STD,   /* I2S standard mode, sharing BCLK and WS with the transmitter */
    AUDIO_RX_PDM,   /* PDM microphone: one clock out, one data line in */
} audio_rx_mode_t;

/* ------------------------------------------------------------------ */
/* Codec interface                                                     */
/* ------------------------------------------------------------------ */

/*
 * What every audio part looks like to the generic commands.
 *
 * The optional entries really are optional: the NS4168 is an amplifier with no
 * control interface at all, so it leaves set_volume NULL and the `audio volume`
 * command explains that rather than failing obscurely. Adding a part is a new
 * file exporting one of these, a row in the registry in audio.c, and a group
 * in app_console.c.
 */
typedef struct audio_codec {
    const char *name;          /* what the user types; an identifier, not prose */
    /*
     * One line, shown by `audio codecs`. An id rather than a literal, and the
     * same id the part's command group uses for its own help -- the sentence
     * was written twice and is now stored once.
     */
    strres_id_t description;
    audio_dir_t directions;
    bool needs_mclk;           /* refuse to attach on a bus with no MCLK pin */

    /*
     * Every call below may return ESP_ERR_NOT_SUPPORTED to mean "I have
     * already told the user why this cannot work". The generic commands stay
     * quiet in that case rather than appending an error name that says less
     * than the driver's own message did.
     */

    /* Evidence a real part is present. NULL when the part cannot be probed. */
    esp_err_t (*probe)(void);
    /* Match the part to the bus format. Called on attach and after `audio bus`. */
    esp_err_t (*configure)(const audio_format_t *fmt);
    esp_err_t (*set_volume)(int percent);  /* NULL: no volume control */
    esp_err_t (*set_mute)(bool mute);      /* NULL: no mute control */
    void (*status)(void);                  /* part-specific detail */
    void (*detach)(void);                  /* release pins, power down */
} audio_codec_t;

/*
 * Make this part the target of the generic commands.
 *
 * There are two slots, one per direction, because a board can perfectly well
 * have an amplifier and a microphone on the same bus and testing one against
 * the other is the point of `audio loopback`. Attaching only displaces parts
 * that need a direction the incoming one needs; a part that works both ways,
 * like the NAU8822, occupies both slots on its own.
 *
 * A single slot would be actively wrong rather than merely limiting: detach()
 * powers a part down -- the NS4168's drives its enable pin low -- so attaching
 * a microphone would silently stop the amplifier.
 */
void audio_codec_attach(const audio_codec_t *codec);
void audio_codec_detach(void);
const audio_codec_t *audio_codec_output(void);
const audio_codec_t *audio_codec_input(void);

/* ------------------------------------------------------------------ */
/* Transport (i2s_bus.c)                                               */
/* ------------------------------------------------------------------ */

bool audio_bus_ready(void);

/* True if the bus is up; otherwise reports the usual error and returns false.
 * The mirror of i2c_require_bus(). */
bool audio_bus_require(void);

/* (Re)open the bus. din < 0 means no receive line. Tears down any existing
 * configuration first, so the command is re-runnable. */
esp_err_t audio_bus_open(int bclk, int ws, int dout, int din,
                         const audio_format_t *fmt);
void audio_bus_close(void);

const audio_format_t *audio_bus_format(void);
void audio_bus_pins(int *bclk, int *ws, int *dout, int *din, int *mclk);

/* Bytes per frame: slots x bits/8. Frames are always two slots wide -- mono is
 * carried as the same sample in both, because a codec expecting a stereo frame
 * plays a one-slot frame at half speed and an octave down. */
size_t audio_bus_frame_bytes(void);

/* Clocks the driver actually programmed, from i2s_channel_get_info(). The
 * sample rate is derived from BCLK, so it is what the hardware is doing rather
 * than what was asked for; the dividers quantise. Zero when unknown. */
uint32_t audio_bus_actual_rate(void);
uint32_t audio_bus_bclk_hz(void);
uint32_t audio_bus_mclk_hz(void);

/* Frames the DMA holds when its queue is full. The signal generator needs it
 * to know when writes have become rate-limited by the hardware rather than by
 * the buffer filling up, which is where an honest rate measurement starts. */
uint64_t audio_bus_buffered_frames(void);

/*
 * Frames the generator should write at a time: one DMA descriptor.
 *
 * This is not a free choice. The DMA frees space one whole descriptor at a
 * time, so a write of any other size returns with the queue at an occupancy
 * that depends on where the block boundary happened to fall. Matching the
 * descriptor makes every write return at the same queue state, which is what
 * lets the rate measurement in tone.c cancel the buffer exactly instead of
 * carrying a fixed offset of up to one descriptor.
 */
size_t audio_bus_block_frames(void);

esp_err_t audio_bus_tx_enable(bool enable);
bool audio_bus_tx_enabled(void);
esp_err_t audio_bus_write(const void *data, size_t bytes, size_t *written,
                          uint32_t timeout_ms);

/* ------------------------------------------------------------------ */
/* Receive (i2s_bus.c)                                                 */
/* ------------------------------------------------------------------ */

/*
 * Open a PDM microphone. This allocates a receiver of its own rather than
 * joining the standard bus, because PDM is a different mode: the part is
 * clocked at a few megahertz on a single line and the peripheral decimates it
 * down to PCM. Refuses if either pin is already carrying a standard-mode
 * signal, which is not a hypothetical -- on the Cardputer the microphone clock
 * and the speaker word-select are the same GPIO.
 */
esp_err_t audio_bus_open_pdm(int clk, int din, const audio_format_t *fmt);
void audio_bus_rx_close(void);

audio_rx_mode_t audio_bus_rx_mode(void);
bool audio_bus_rx_ready(void);
bool audio_bus_rx_require(void);

/* True when DIN and DOUT are the same pin, which the I2S driver turns into an
 * internal loopback: the transmitter feeds the receiver through the pad with
 * nothing connected. Worth reporting, because a capture that succeeds this way
 * says nothing at all about anything outside the chip. */
bool audio_bus_rx_internal(void);

void audio_bus_rx_pins(int *clk, int *din);
uint32_t audio_bus_rx_rate(void);        /* achieved, from the driver */

/* The clock on the microphone's own pin, which is what its datasheet puts
 * limits on -- a MEMS PDM part typically wants 1 to 3.25 MHz and sleeps below
 * that. Zero unless a PDM microphone is open. */
uint32_t audio_bus_pdm_clk_hz(void);

uint8_t audio_bus_rx_bits(void);         /* valid data bits per sample */
size_t audio_bus_rx_frame_bytes(void);   /* two slots, as on the transmit side */

esp_err_t audio_bus_rx_enable(bool enable);
bool audio_bus_rx_enabled(void);
esp_err_t audio_bus_read(void *data, size_t bytes, size_t *read,
                         uint32_t timeout_ms);

/* ------------------------------------------------------------------ */
/* Test signals (tone.c)                                               */
/* ------------------------------------------------------------------ */

typedef enum {
    AUDIO_CHANNEL_BOTH,
    AUDIO_CHANNEL_LEFT,
    AUDIO_CHANNEL_RIGHT,
} audio_channel_t;

typedef struct {
    double start_hz;
    double end_hz;      /* equal to start_hz for a steady tone */
    bool logarithmic;   /* sweeps only */
    double seconds;     /* 0 means continuous, ended by audio_play_stop() */
    int level_pct;      /* digital amplitude, 1-100 */
    audio_channel_t channel;
    /* Suppress the per-run report. `audio loopback` drives the generator as one
     * step of a larger measurement and prints its own conclusion; a playback
     * summary arriving in the middle of that is noise. */
    bool quiet;
} audio_signal_t;

/*
 * Play a signal. A finite duration blocks until it finishes and then reports;
 * a continuous one starts a task and returns. Reports its own errors, and
 * returns 0 or -1 like a command.
 */
int audio_play(const audio_signal_t *signal);
int audio_play_stop(void);
bool audio_playing(void);

/* ------------------------------------------------------------------ */
/* Capture and analysis (capture.c)                                    */
/* ------------------------------------------------------------------ */

/*
 * Half-octave probe frequencies for the ASCII spectrum, from 31.25 Hz up to
 * whatever falls below Nyquist. Twenty of them reach 22.6 kHz, which covers
 * every sample rate this tool offers.
 */
#define AUDIO_BANDS 20

typedef struct {
    double seconds;
    double detect_hz;   /* 0 skips the single-frequency detector */
    bool spectrum;      /* run the probe bank */
} audio_capture_req_t;

/*
 * Both slots are analysed separately and always reported. A mono microphone
 * only fills one of them, and which one is a board fact worth discovering the
 * same way `audio tone ... left` discovers which slot a mono amplifier plays.
 *
 * Amplitudes are in raw sample counts, with full_scale saying what 0 dBFS is,
 * because that is the number a datasheet is written in. dBFS is derived for
 * display.
 */
typedef struct {
    uint64_t frames;
    uint32_t rate;
    uint8_t bits;
    double full_scale;      /* counts at 0 dBFS */

    int32_t min[2];
    int32_t max[2];
    double mean[2];         /* DC offset, counts */
    double stdev[2];        /* RMS about the mean, counts */
    double peak[2];         /* largest excursion from zero, counts */
    uint64_t clipped[2];    /* samples at or beyond full scale */

    /*
     * Frequency-domain results are in dBFS rather than counts. They come from a
     * fixed-length window analysed at 16-bit resolution whatever the stream's
     * width, so counts would be in different units from the ones above; a
     * relative level is the same number either way.
     */
    double detect_hz;
    double tone_dbfs[2];

    uint32_t band_count;
    double band_hz[AUDIO_BANDS];
    double band_dbfs[2][AUDIO_BANDS];
    uint64_t analysed;            /* frames the probe bank actually saw */

    int overruns;           /* reads that timed out; the DMA lost samples */
    esp_err_t error;
} audio_capture_t;

/* Reported instead of -inf, so a silent channel still lines up in a column. */
#define AUDIO_DBFS_FLOOR (-120.0)

esp_err_t audio_capture_run(const audio_capture_req_t *req, audio_capture_t *out);
void audio_capture_report(const audio_capture_t *cap);
void audio_capture_spectrum(const audio_capture_t *cap);

/* Throw away whatever the DMA has queued, so a measurement starts on samples
 * taken after whatever just changed rather than before it. */
void audio_capture_flush(double seconds);

/* Counts to dBFS, floored rather than running to -inf so a column stays a
 * column. Returns the floor for silence. */
double audio_dbfs(double amplitude, double full_scale);

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

int cmd_audio_bus(int argc, char **argv);
int cmd_audio_pdm(int argc, char **argv);
int cmd_audio_record(int argc, char **argv);
int cmd_audio_capture_file(int argc, char **argv);
int cmd_audio_level(int argc, char **argv);
int cmd_audio_loopback(int argc, char **argv);
int cmd_audio_info(int argc, char **argv);
int cmd_audio_codecs(int argc, char **argv);
int cmd_audio_tone(int argc, char **argv);
int cmd_audio_sweep(int argc, char **argv);
int cmd_audio_stop(int argc, char **argv);
int cmd_audio_volume(int argc, char **argv);
int cmd_audio_mute(int argc, char **argv);
int cmd_audio_close(int argc, char **argv);

#endif
