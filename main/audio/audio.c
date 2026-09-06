/*
 * The `audio` group.
 *
 * This is a capability group rather than a bus group -- it owns the I2S
 * transport, the test signals, and whichever codec is currently attached. `sd`
 * is the existing example of the same shape: named after what it tests, not
 * after the peripheral it happens to drive.
 *
 * The generic commands here never mention a specific part. They work through
 * the audio_codec_t vtable in audio.h, so a codec with sixty I2C registers and
 * an amplifier with one enable pin are driven by the same `audio tone`,
 * `audio volume` and `audio mute`.
 */
#include "app_bringup.h"
#include "audio.h"
#include "codec_nau8822.h"
#include "codec_ns4168.h"
#include "codec_sph0645.h"

#include "driver/gpio.h"

#include <math.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#define DEFAULT_RATE_HZ  48000
#define DEFAULT_BITS     16
#define DEFAULT_LEVEL_PCT 25
#define DEFAULT_SECONDS   3.0

#define MIN_RATE_HZ  8000
#define MAX_RATE_HZ  192000
#define MAX_SECONDS  600.0
#define MIN_TONE_HZ  1.0

/* Every part the firmware knows how to drive. Adding one is a new file, one
 * row here, and one group in app_console.c. */
static const audio_codec_t *const codec_registry[] = {
    &nau8822_codec,
    &ns4168_codec,
    &sph0645_codec,
};
#define CODEC_COUNT (sizeof(codec_registry) / sizeof(codec_registry[0]))

/* One slot per direction; see audio_codec_attach() in audio.h for why. */
static const audio_codec_t *tx_codec;
static const audio_codec_t *rx_codec;
static int last_volume_pct = -1;

/* ------------------------------------------------------------------ */
/* Codec attachment                                                    */
/* ------------------------------------------------------------------ */

static void release(const audio_codec_t *codec)
{
    if (!codec) {
        return;
    }
    if (codec->detach) {
        codec->detach();
    }
    /* Clear both slots: a bidirectional part holds two and must not be left
     * half-attached, pointing at hardware it has just powered down. */
    if (tx_codec == codec) {
        tx_codec = NULL;
    }
    if (rx_codec == codec) {
        rx_codec = NULL;
    }
}

void audio_codec_attach(const audio_codec_t *codec)
{
    if (!codec) {
        return;
    }

    /* Displace only what contends for a direction this part needs. */
    if ((codec->directions & AUDIO_DIR_TX) && tx_codec != codec) {
        release(tx_codec);
    }
    if ((codec->directions & AUDIO_DIR_RX) && rx_codec != codec) {
        release(rx_codec);
    }

    if (codec->directions & AUDIO_DIR_TX) {
        tx_codec = codec;
    }
    if (codec->directions & AUDIO_DIR_RX) {
        rx_codec = codec;
    }
    last_volume_pct = -1;
}

void audio_codec_detach(void)
{
    release(tx_codec);
    release(rx_codec);
    last_volume_pct = -1;
}

const audio_codec_t *audio_codec_output(void)
{
    return tx_codec;
}

const audio_codec_t *audio_codec_input(void)
{
    return rx_codec;
}

/* ------------------------------------------------------------------ */
/* audio bus                                                           */
/* ------------------------------------------------------------------ */

/*
 * Consume the optional trailing keyword pairs. Pins are positional because
 * three of them are always required; everything else is a keyword pair, so
 * options can be given in any order and none of them can be confused with a
 * pin. This is the same idiom as `sd spi ... khz <freq>`.
 */
static int take_bus_options(int argc, char **argv, int index,
                            int *din, audio_format_t *fmt)
{
    while (index < argc) {
        if (index + 1 >= argc) {
            diag_error("'%s' needs a value", argv[index]);
            return -1;
        }

        const char *key = argv[index];
        const char *value = argv[index + 1];

        if (strcasecmp(key, "din") == 0) {
            if (cli_parse_int_arg(value, din) < 0 || !GPIO_IS_VALID_GPIO(*din)) {
                diag_error("DIN must be a valid pin number, not '%s'", value);
                return -1;
            }
        } else if (strcasecmp(key, "mclk") == 0) {
            if (cli_parse_int_arg(value, &fmt->mclk_pin) < 0 ||
                !GPIO_IS_VALID_OUTPUT_GPIO(fmt->mclk_pin)) {
                diag_error("MCLK must be an output-capable pin, not '%s'", value);
                return -1;
            }
        } else if (strcasecmp(key, "rate") == 0) {
            int rate = 0;
            if (cli_parse_int_arg(value, &rate) < 0 ||
                rate < MIN_RATE_HZ || rate > MAX_RATE_HZ) {
                diag_error("Sample rate must be %d-%d Hz", MIN_RATE_HZ, MAX_RATE_HZ);
                return -1;
            }
            fmt->rate_hz = (uint32_t)rate;
        } else if (strcasecmp(key, "bits") == 0) {
            int bits = 0;
            if (cli_parse_int_arg(value, &bits) < 0 ||
                (bits != 16 && bits != 24 && bits != 32)) {
                diag_error("Bit width must be 16, 24 or 32");
                return -1;
            }
            fmt->bits = (uint8_t)bits;
        } else if (strcasecmp(key, "mclkmult") == 0) {
            int mult = 0;
            if (cli_parse_int_arg(value, &mult) < 0 ||
                (mult != 128 && mult != 192 && mult != 256 && mult != 384 &&
                 mult != 512 && mult != 768)) {
                diag_error("MCLK multiple must be 128, 192, 256, 384, 512 or 768");
                return -1;
            }
            fmt->mclk_multiple = mult;
        } else {
            diag_error("Unexpected argument '%s'; options are 'din <pin>', "
                     "'mclk <pin>', 'rate <hz>', 'bits <16|24|32>' and "
                     "'mclkmult <n>'", key);
            return -1;
        }
        index += 2;
    }
    return 0;
}

static bool pins_distinct(const int *pins, const char *const *names, int count)
{
    for (int i = 0; i < count; i++) {
        if (pins[i] < 0) {
            continue;
        }
        for (int j = i + 1; j < count; j++) {
            if (pins[i] == pins[j]) {
                diag_error("%s and %s cannot both be GPIO %d", names[i], names[j],
                         pins[i]);
                return false;
            }
        }
    }
    return true;
}

static void print_format(void)
{
    const audio_format_t *fmt = audio_bus_format();
    if (!fmt) {
        return;
    }

    uint32_t configured = audio_bus_actual_rate();
    diag_printf("Format:  %lu Hz requested", (unsigned long)fmt->rate_hz);
    if (configured) {
        diag_printf(", %lu Hz configured", (unsigned long)configured);
    }
    diag_printf(", %u-bit, 2 slots\n", fmt->bits);

    /*
     * Report what the dividers actually produced, not what was asked for. The
     * clock tree divides an integer ratio out of a PLL, so a requested rate is
     * often unreachable exactly -- the same reason `gpio pwm set` reports the
     * frequency it achieved and `sd spi` reports its real clock.
     */
    uint32_t bclk = audio_bus_bclk_hz();
    uint32_t mclk = audio_bus_mclk_hz();
    diag_printf("Clocks:  BCLK %.3f MHz", bclk / 1e6);
    if (fmt->mclk_pin >= 0) {
        diag_printf(", MCLK %.3f MHz on GPIO %d (x%d)\n", mclk / 1e6,
                  fmt->mclk_pin, fmt->mclk_multiple);
    } else {
        diag_printf(", MCLK not routed to a pin\n");
    }
}

/* The receive side of `audio info`, and the tail of `audio pdm`. */
static void print_input(void)
{
    switch (audio_bus_rx_mode()) {
    case AUDIO_RX_NONE:
        diag_printf("Input:   none. Add 'din <pin>' to 'audio bus', or run "
                  "'audio pdm <clk> <data>'.\n");
        return;

    case AUDIO_RX_STD: {
        int din = -1;
        audio_bus_rx_pins(NULL, &din);
        diag_printf("Input:   I2S on DIN=%d at %lu Hz, %u-bit%s\n", din,
                  (unsigned long)audio_bus_rx_rate(), audio_bus_rx_bits(),
                  audio_bus_rx_internal()
                      ? " -- looped back inside the chip, nothing external" : "");
        break;
    }

    case AUDIO_RX_PDM: {
        int clk = -1;
        int din = -1;
        audio_bus_rx_pins(&clk, &din);
        uint32_t pdm_hz = audio_bus_pdm_clk_hz();
        diag_printf("Input:   PDM on CLK=%d, DATA=%d, decimated to %lu Hz "
                  "16-bit\n", clk, din, (unsigned long)audio_bus_rx_rate());
        diag_printf("         Microphone clock %.3f MHz\n", pdm_hz / 1e6);
        /*
         * The usual MEMS PDM part wants roughly 1 to 3.25 MHz and drops into a
         * low-power mode below that, where it returns something that looks
         * like a dead microphone. Saying so here is cheaper than debugging it.
         */
        if (pdm_hz && (pdm_hz < 1000000 || pdm_hz > 3250000)) {
            diag_printf("         Outside the 1-3.25 MHz most MEMS microphones "
                      "specify; try another rate if it reads silent.\n");
        }
        break;
    }
    }

    diag_printf("         %s\n", audio_bus_rx_enabled() ? "Receiver running"
                                                      : "Receiver stopped");
}

int cmd_audio_bus(int argc, char **argv)
{
    if (argc < 4) {
        diag_printf("Usage: bus <bclk> <ws> <dout> [din <pin>] [mclk <pin>] "
                  "[rate <hz>] [bits <16|24|32>] [mclkmult <n>]\n");
        return -1;
    }

    int bclk = 0;
    int ws = 0;
    int dout = 0;
    if (cli_parse_int_arg(argv[1], &bclk) < 0 || cli_parse_int_arg(argv[2], &ws) < 0 ||
        cli_parse_int_arg(argv[3], &dout) < 0) {
        diag_error("BCLK, WS and DOUT must be pin numbers");
        return -1;
    }

    int din = -1;
    audio_format_t fmt = {
        .rate_hz = DEFAULT_RATE_HZ,
        .bits = DEFAULT_BITS,
        .mclk_pin = -1,
        .mclk_multiple = 0,
    };
    if (take_bus_options(argc, argv, 4, &din, &fmt) < 0) {
        return -1;
    }

    /*
     * DIN on the same pin as DOUT is not a mistake: the I2S driver turns that
     * into an internal loopback, feeding the transmitter straight back to the
     * receiver through the pad. It is the one   test that needs no
     * hardware at all, which makes it the right first move when a microphone
     * reads silent -- it separates a broken receive path from a broken part.
     */
    bool internal = (din >= 0 && din == dout);

    const int pins[] = {bclk, ws, dout, internal ? -1 : din, fmt.mclk_pin};
    const char *const names[] = {"BCLK", "WS", "DOUT", "DIN", "MCLK"};
    for (int i = 0; i < 5; i++) {
        if (i == 3) {
            continue; /* DIN is an input, checked separately below */
        }
        if (pins[i] >= 0 && !GPIO_IS_VALID_OUTPUT_GPIO(pins[i])) {
            diag_error("%s: GPIO %d is not an output-capable pin on this chip",
                     names[i], pins[i]);
            return -1;
        }
    }
    if (din >= 0 && !GPIO_IS_VALID_GPIO(din)) {
        diag_error("DIN: GPIO %d is not a valid pin on this chip", din);
        return -1;
    }
    if (!pins_distinct(pins, names, 5)) {
        return -1;
    }

    /* Opening the standard bus releases a PDM microphone, because the two
     * share the module's sample rate and this call may be changing it. */
    bool had_pdm = (audio_bus_rx_mode() == AUDIO_RX_PDM);

    /* Reconfiguring the transport under a running tone would tear the channel
     * out from under the player task. */
    if (audio_playing()) {
        audio_play_stop();
    }

    esp_err_t err = audio_bus_open(bclk, ws, dout, din, &fmt);
    if (err != ESP_OK) {
        diag_error("Initializing I2S: %s", esp_err_to_name(err));
        return -1;
    }

    /*
     * Start the transmitter immediately and leave it running.
     *
     * With no data queued the DMA sends silence, so the bit clock and word
     * select run continuously from this point. That is what a codec wants:
     * most of them mute or reset themselves when their clocks stop, and
     * starting and stopping the clock around every tone makes an amplifier
     * pop. It also means the lines can be put on a scope before any signal is
     * played.
     */
    err = audio_bus_tx_enable(true);
    if (err != ESP_OK) {
        diag_error("Starting the I2S transmitter: %s", esp_err_to_name(err));
        return -1;
    }

    diag_printf("I2S initialized on BCLK=%d, WS=%d, DOUT=%d", bclk, ws, dout);
    if (din >= 0) {
        diag_printf(", DIN=%d", din);
    }
    diag_printf("\n");
    print_format();
    diag_printf("Transmitting silence, so the clocks are live for the codec.\n");
    if (internal) {
        diag_printf("DIN and DOUT are the same pin, so 'audio record' sees the "
                  "transmitter through the pad and nothing external.\n");
    }
    if (had_pdm) {
        diag_printf("The PDM microphone was released; re-run 'audio pdm' to "
                  "bring it back at the new rate.\n");
    }

    /* An attached part was configured for the old format; bring it in line. */
    const audio_codec_t *attached[] = {tx_codec, rx_codec};
    for (size_t i = 0; i < 2; i++) {
        const audio_codec_t *codec = attached[i];
        /* A part holding both slots appears twice; configure it once. */
        if (!codec || !codec->configure || (i == 1 && codec == attached[0])) {
            continue;
        }
        err = codec->configure(audio_bus_format());
        if (err == ESP_ERR_NOT_SUPPORTED) {
            return -1;   /* the driver has already explained */
        }
        if (err != ESP_OK) {
            diag_error("Reconfiguring %s for the new format: %s", codec->name,
                     esp_err_to_name(err));
            return -1;
        }
        diag_printf("Reconfigured %s for the new format.\n", codec->name);
    }

    return 0;
}

int cmd_audio_info(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!audio_bus_ready()) {
        diag_printf("I2S is not initialized. Run 'audio bus <bclk> <ws> <dout>'.\n");
    } else {
        int bclk = 0;
        int ws = 0;
        int dout = 0;
        int din = 0;
        int mclk = 0;
        audio_bus_pins(&bclk, &ws, &dout, &din, &mclk);

        diag_printf("Pins:    BCLK=%d, WS=%d, DOUT=%d", bclk, ws, dout);
        if (din >= 0) {
            diag_printf(", DIN=%d", din);
        }
        if (mclk >= 0) {
            diag_printf(", MCLK=%d", mclk);
        }
        diag_printf("\n");
        print_format();
        diag_printf("Output:  %s\n", audio_bus_tx_enabled()
                  ? (audio_playing() ? "playing" : "running, silent") : "stopped");
    }

    print_input();

    if (!tx_codec && !rx_codec) {
        diag_printf("Codec:   none attached (a bare I2S DAC, amplifier or "
                  "microphone needs none)\n");
        return 0;
    }

    const audio_codec_t *attached[] = {tx_codec, rx_codec};
    const char *role[] = {"Plays", "Records"};
    for (size_t i = 0; i < 2; i++) {
        const audio_codec_t *codec = attached[i];
        if (!codec) {
            continue;
        }
        if (i == 1 && codec == attached[0]) {
            continue;   /* one part holding both slots; already listed */
        }

        const char *label = (codec->directions == (AUDIO_DIR_TX | AUDIO_DIR_RX))
                            ? "Codec" : role[i];
        diag_printf("%-8s %s - %s\n", label, codec->name, codec->description);
        if (i == 0 && last_volume_pct >= 0) {
            diag_printf("Volume:  %d%%\n", last_volume_pct);
        }
        if (codec->status) {
            codec->status();
        }
    }
    return 0;
}

int cmd_audio_codecs(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    diag_printf("%-10s %-7s %-5s %s\n", "name", "dir", "mclk", "part");
    for (size_t i = 0; i < CODEC_COUNT; i++) {
        const audio_codec_t *c = codec_registry[i];
        const char *dir = (c->directions == (AUDIO_DIR_TX | AUDIO_DIR_RX)) ? "in/out"
                        : (c->directions & AUDIO_DIR_RX) ? "in" : "out";
        diag_printf("%-10s %-7s %-5s %s%s\n", c->name, dir,
                  c->needs_mclk ? "yes" : "no", c->description,
                  (c == tx_codec || c == rx_codec) ? "  <- attached" : "");
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Test signals                                                        */
/* ------------------------------------------------------------------ */

/*
 * Trailing options shared by `tone` and `sweep`.
 *
 * A bare number is the duration and may appear once; everything else is a
 * keyword. That keeps `tone 1000 3 level 50 left` readable while leaving no
 * argument that could be mistaken for another.
 */
static int take_signal_options(int argc, char **argv, int index,
                               audio_signal_t *sig, bool is_sweep)
{
    bool duration_set = false;

    while (index < argc) {
        const char *token = argv[index];

        if (strcasecmp(token, "level") == 0) {
            if (index + 1 >= argc) {
                diag_error("'level' needs a percentage");
                return -1;
            }
            int pct = 0;
            if (cli_parse_int_arg(argv[index + 1], &pct) < 0 || pct < 1 || pct > 100) {
                diag_error("Level must be 1-100 percent of full scale");
                return -1;
            }
            sig->level_pct = pct;
            index += 2;
            continue;
        }

        if (strcasecmp(token, "left") == 0) {
            sig->channel = AUDIO_CHANNEL_LEFT;
        } else if (strcasecmp(token, "right") == 0) {
            sig->channel = AUDIO_CHANNEL_RIGHT;
        } else if (strcasecmp(token, "both") == 0) {
            sig->channel = AUDIO_CHANNEL_BOTH;
        } else if (is_sweep && strcasecmp(token, "log") == 0) {
            sig->logarithmic = true;
        } else if (is_sweep && strcasecmp(token, "linear") == 0) {
            sig->logarithmic = false;
        } else if (!is_sweep && strcasecmp(token, "continuous") == 0) {
            sig->seconds = 0.0;
            duration_set = true;
        } else {
            double seconds = 0.0;
            if (duration_set || cli_parse_double_arg(token, &seconds) < 0) {
                diag_error("Unexpected argument '%s'", token);
                return -1;
            }
            if (seconds <= 0.0 || seconds > MAX_SECONDS) {
                diag_error("Duration must be greater than 0 and at most %.0f seconds",
                         MAX_SECONDS);
                return -1;
            }
            sig->seconds = seconds;
            duration_set = true;
        }
        index++;
    }

    return 0;
}

static bool frequency_ok(double hz, const char *what)
{
    /* Above Nyquist the tone aliases down to something else entirely, which
     * during bring-up looks like a fault in the hardware rather than in the
     * request. */
    const audio_format_t *fmt = audio_bus_format();
    double nyquist = (fmt ? fmt->rate_hz : DEFAULT_RATE_HZ) / 2.0;

    if (hz < MIN_TONE_HZ || hz >= nyquist) {
        diag_error("%s must be between %.0f Hz and %.0f Hz (half the sample rate)",
                 what, MIN_TONE_HZ, nyquist);
        return false;
    }
    return true;
}

int cmd_audio_tone(int argc, char **argv)
{
    if (!audio_bus_require()) {
        return -1;
    }
    if (argc < 2) {
        diag_printf("Usage: tone <hz> [seconds|continuous] [level <pct>] "
                  "[left|right|both]\n");
        return -1;
    }

    double hz = 0.0;
    if (cli_parse_double_arg(argv[1], &hz) < 0) {
        diag_error("Frequency must be a number, not '%s'", argv[1]);
        return -1;
    }
    if (!frequency_ok(hz, "Frequency")) {
        return -1;
    }

    audio_signal_t signal = {
        .start_hz = hz,
        .end_hz = hz,
        .logarithmic = false,
        .seconds = DEFAULT_SECONDS,
        .level_pct = DEFAULT_LEVEL_PCT,
        .channel = AUDIO_CHANNEL_BOTH,
    };
    if (take_signal_options(argc, argv, 2, &signal, false) < 0) {
        return -1;
    }

    return audio_play(&signal);
}

int cmd_audio_sweep(int argc, char **argv)
{
    if (!audio_bus_require()) {
        return -1;
    }
    if (argc < 3) {
        diag_printf("Usage: sweep <start_hz> <end_hz> [seconds] [level <pct>] "
                  "[log|linear] [left|right|both]\n");
        return -1;
    }

    double start = 0.0;
    double end = 0.0;
    if (cli_parse_double_arg(argv[1], &start) < 0 || cli_parse_double_arg(argv[2], &end) < 0) {
        diag_error("Start and end frequencies must be numbers");
        return -1;
    }
    if (!frequency_ok(start, "Start frequency") || !frequency_ok(end, "End frequency")) {
        return -1;
    }
    if (start == end) {
        diag_error("Start and end frequencies are the same; use 'audio tone' for that");
        return -1;
    }

    audio_signal_t signal = {
        .start_hz = start,
        .end_hz = end,
        /* Equal time per octave, which is how audio hardware is judged. */
        .logarithmic = true,
        .seconds = 5.0,
        .level_pct = DEFAULT_LEVEL_PCT,
        .channel = AUDIO_CHANNEL_BOTH,
    };
    if (take_signal_options(argc, argv, 3, &signal, true) < 0) {
        return -1;
    }

    return audio_play(&signal);
}

int cmd_audio_stop(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return audio_play_stop();
}

/* ------------------------------------------------------------------ */
/* Capture                                                             */
/* ------------------------------------------------------------------ */

#define DEFAULT_RECORD_SECONDS 2.0
#define DEFAULT_LEVEL_SECONDS  10.0
#define DEFAULT_CAPTURE_SECONDS 30.0
#define LEVEL_TICK_SECONDS     0.1

/*
 * A PDM microphone drives a clock pin of its own, and that pin has to be free.
 *
 * This is not a theoretical check. On the M5Stack Cardputer the microphone's
 * clock and the speaker's word-select are the same GPIO, so the two cannot run
 * at once -- and the failure without this check is not an error but a
 * plausible-looking silence, because the second peripheral quietly takes the
 * pad away from the first and the amplifier starts seeing a megahertz square
 * wave where its frame clock used to be.
 */
static bool pdm_pins_free(int clk, int din)
{
    if (!audio_bus_ready()) {
        return true;
    }

    int bclk = -1;
    int ws = -1;
    int dout = -1;
    int bus_din = -1;
    int mclk = -1;
    audio_bus_pins(&bclk, &ws, &dout, &bus_din, &mclk);

    const int used[] = {bclk, ws, dout, mclk};
    const char *const names[] = {"BCLK", "WS", "DOUT", "MCLK"};

    for (size_t i = 0; i < sizeof(used) / sizeof(used[0]); i++) {
        if (used[i] < 0) {
            continue;
        }
        const char *role = (used[i] == clk) ? "clock"
                         : (used[i] == din) ? "data line" : NULL;
        if (!role) {
            continue;
        }

        diag_error("GPIO %d is already the I2S %s, so it cannot also be the "
                 "microphone's %s", used[i], names[i], role);
        diag_printf("They share a pin here, so the microphone and the speaker "
                  "cannot both run. Run 'audio close', or 'audio pdm' on the "
                  "microphone's own pins.\n");
        return false;
    }

    return true;
}

int cmd_audio_pdm(int argc, char **argv)
{
    if (argc < 3) {
        diag_printf("Usage: pdm <clk> <data> [rate <hz>]\n");
        return -1;
    }

    int clk = 0;
    int din = 0;
    if (cli_parse_int_arg(argv[1], &clk) < 0 || cli_parse_int_arg(argv[2], &din) < 0) {
        diag_error("CLK and DATA must be pin numbers");
        return -1;
    }
    if (!GPIO_IS_VALID_OUTPUT_GPIO(clk)) {
        diag_error("CLK: GPIO %d is not an output-capable pin on this chip", clk);
        return -1;
    }
    if (!GPIO_IS_VALID_GPIO(din)) {
        diag_error("DATA: GPIO %d is not a valid pin on this chip", din);
        return -1;
    }
    if (clk == din) {
        diag_error("CLK and DATA cannot both be GPIO %d", clk);
        return -1;
    }

    /*
     * The rate matters more here than it looks. It sets the microphone's own
     * clock at 64 times itself, so it is also the knob that moves that clock
     * into the range the part specifies.
     */
    audio_format_t fmt = {
        .rate_hz = DEFAULT_RATE_HZ,
        .bits = 16,
        .mclk_pin = -1,
        .mclk_multiple = 0,
    };
    const audio_format_t *existing = audio_bus_format();
    if (existing) {
        fmt.rate_hz = existing->rate_hz;
    }

    int index = 3;
    while (index < argc) {
        if (strcasecmp(argv[index], "rate") != 0) {
            diag_error("Unexpected argument '%s'; the only option is 'rate <hz>'",
                     argv[index]);
            return -1;
        }
        if (index + 1 >= argc) {
            diag_error("'rate' needs a value");
            return -1;
        }
        int rate = 0;
        if (cli_parse_int_arg(argv[index + 1], &rate) < 0 ||
            rate < MIN_RATE_HZ || rate > MAX_RATE_HZ) {
            diag_error("Sample rate must be %d-%d Hz", MIN_RATE_HZ, MAX_RATE_HZ);
            return -1;
        }
        if (existing && (uint32_t)rate != existing->rate_hz) {
            diag_error("The I2S bus is already running at %lu Hz",
                     (unsigned long)existing->rate_hz);
            diag_printf("Capture and playback share one rate; re-run "
                      "'audio bus ... rate %d' to change both.\n", rate);
            return -1;
        }
        fmt.rate_hz = (uint32_t)rate;
        index += 2;
    }

    if (!pdm_pins_free(clk, din)) {
        return -1;
    }

    esp_err_t err = audio_bus_open_pdm(clk, din, &fmt);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return -1;   /* the transport has already explained */
    }
    if (err != ESP_OK) {
        diag_error("Opening the PDM microphone: %s", esp_err_to_name(err));
        return -1;
    }

    err = audio_bus_rx_enable(true);
    if (err != ESP_OK) {
        diag_error("Starting the receiver: %s", esp_err_to_name(err));
        audio_bus_rx_close();
        return -1;
    }

    print_input();
    diag_printf("Run 'audio record' to see what it hears.\n");
    return 0;
}

/* A bare trailing number is the duration, as it is for `tone`. */
static int take_seconds(int argc, char **argv, int index, double *seconds,
                        double most)
{
    if (index >= argc) {
        return 0;
    }
    if (index + 1 < argc) {
        diag_error("Unexpected argument '%s'", argv[index + 1]);
        return -1;
    }
    if (cli_parse_double_arg(argv[index], seconds) < 0 || *seconds <= 0.0 ||
        *seconds > most) {
        diag_error("Duration must be greater than 0 and at most %.0f seconds",
                 most);
        return -1;
    }
    return 0;
}

int cmd_audio_record(int argc, char **argv)
{
    if (!audio_bus_rx_require()) {
        return -1;
    }

    double seconds = DEFAULT_RECORD_SECONDS;
    if (take_seconds(argc, argv, 1, &seconds, 60.0) < 0) {
        return -1;
    }

    esp_err_t err = audio_bus_rx_enable(true);
    if (err != ESP_OK) {
        diag_error("Starting the receiver: %s", esp_err_to_name(err));
        return -1;
    }

    /* Whatever is in the DMA now was captured before this command was typed,
     * and on a freshly enabled receiver it is the part's start-up transient. */
    audio_capture_flush(0.1);

    audio_capture_req_t req = {
        .seconds = seconds,
        .detect_hz = 0.0,
        .spectrum = true,
    };
    audio_capture_t cap;
    audio_capture_run(&req, &cap);
    audio_capture_report(&cap);
    audio_capture_spectrum(&cap);

    return cap.error == ESP_OK ? 0 : -1;
}

int cmd_audio_capture_file(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!audio_bus_rx_require()) {
        return -1;
    }

    double seconds = DEFAULT_RECORD_SECONDS;
    if (take_seconds(argc, argv, 1, &seconds, 60.0) < 0) {
        return -1;
    }

    FILE *test = fopen("/sd/.mount_test", "w");
    if (!test) {
        diag_error("SD card not mounted at /sd. Run 'sd spi' or 'sd mmc' first.");
        return -1;
    }
    fclose(test);
    unlink("/sd/.mount_test");

    esp_err_t err = audio_bus_rx_enable(true);
    if (err != ESP_OK) {
        diag_error("Starting the receiver: %s", esp_err_to_name(err));
        return -1;
    }

    audio_capture_flush(0.1);

    const uint32_t sample_rate = 48000;
    const uint64_t samples_for_capture = sample_rate * seconds;
    const uint32_t samples_per_file = sample_rate * 10;
    const size_t frame_bytes = audio_bus_rx_frame_bytes();
    const size_t block_frames = audio_bus_block_frames();
    void *buffer = malloc(block_frames * frame_bytes);
    if (!buffer) {
        diag_error("Out of memory for capture buffer");
        return -1;
    }

    int file_num = 0;
    uint64_t samples_captured = 0;

    while (samples_captured < samples_for_capture) {
        char path[64];
        snprintf(path, sizeof(path), "/sd/capture%d.pcm", file_num);

        FILE *f = fopen(path, "wb");
        if (!f) {
            diag_error("Opening %s: %s", path, strerror(errno));
            free(buffer);
            return -1;
        }

        uint64_t samples_written = 0;
        int overruns = 0;

        while (samples_written < samples_per_file) {
            size_t want = block_frames;
            if (samples_written + want > samples_per_file) {
                want = (size_t)(samples_per_file - samples_written);
            }

            size_t got = 0;
            esp_err_t read_err = audio_bus_read(buffer, want * frame_bytes, &got, 1000);
            if (read_err != ESP_OK) {
                diag_error("Read error: %s", esp_err_to_name(read_err));
                fclose(f);
                free(buffer);
                return -1;
            }

            size_t frames = got / frame_bytes;
            if (frames == 0) {
                overruns++;
                continue;
            }

            if (fwrite(buffer, frame_bytes, frames, f) != frames) {
                diag_error("Writing to %s: %s", path, strerror(errno));
                fclose(f);
                free(buffer);
                return -1;
            }

            samples_written += frames;
            samples_captured += frames;
        }

        if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
            diag_error("Flushing %s: %s", path, strerror(errno));
            fclose(f);
            free(buffer);
            return -1;
        }

        fclose(f);

        diag_printf("%s: %.1f s, %llu samples, %d overruns\n",
                  path, 10.0, (unsigned long long)samples_written, overruns);

        file_num++;
    }

    free(buffer);

    diag_printf("%llu samples, %d files\n",
            samples_captured, file_num);


    return 0;
}

int cmd_audio_level(int argc, char **argv)
{
    if (!audio_bus_rx_require()) {
        return -1;
    }

    double seconds = DEFAULT_LEVEL_SECONDS;
    if (take_seconds(argc, argv, 1, &seconds, 120.0) < 0) {
        return -1;
    }

    esp_err_t err = audio_bus_rx_enable(true);
    if (err != ESP_OK) {
        diag_error("Starting the receiver: %s", esp_err_to_name(err));
        return -1;
    }
    audio_capture_flush(0.1);

    /*
     * A meter rather than a measurement. `record` answers "what is the input
     * doing"; this answers "does it react to me", which is the question you
     * actually have while tapping a microphone, and it needs the answer to
     * arrive while your finger is still moving.
     */
    diag_printf("RMS per slot, ten times a second for %.0f s. Bars run from "
              "-80 dBFS to 0.\n", seconds);

    int ticks = (int)(seconds / LEVEL_TICK_SECONDS);
    for (int i = 0; i < ticks; i++) {
        audio_capture_req_t req = {
            .seconds = LEVEL_TICK_SECONDS,
            .detect_hz = 0.0,
            .spectrum = false,
        };
        audio_capture_t cap;
        if (audio_capture_run(&req, &cap) != ESP_OK) {
            diag_error("Capture failed: %s", esp_err_to_name(cap.error));
            return -1;
        }

        char bar[2][33];
        double db[2];
        for (int ch = 0; ch < 2; ch++) {
            db[ch] = audio_dbfs(cap.stdev[ch], cap.full_scale);
            int cells = (int)lrint((db[ch] + 80.0) / 80.0 * 32);
            if (cells < 0) {
                cells = 0;
            }
            if (cells > 32) {
                cells = 32;
            }
            memset(bar[ch], '#', (size_t)cells);
            bar[ch][cells] = '\0';
        }
        diag_printf("L %-32s %6.1f   R %-32s %6.1f\n", bar[0], db[0], bar[1],
                  db[1]);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Loopback                                                            */
/* ------------------------------------------------------------------ */

#define DEFAULT_LOOPBACK_HZ      1000.0
#define DEFAULT_LOOPBACK_SECONDS 0.4
#define LOOPBACK_SETTLE_SECONDS  0.2
/* Below this the tone is indistinguishable from whatever was already there. */
#define LOOPBACK_MARGINAL_DB     6.0
#define LOOPBACK_HEARD_DB        12.0

static double measure_tone(double hz, double seconds, double *level)
{
    audio_capture_req_t req = {
        .seconds = seconds,
        .detect_hz = hz,
        .spectrum = false,
    };
    audio_capture_t cap;
    if (audio_capture_run(&req, &cap) != ESP_OK) {
        return NAN;
    }

    /* The louder slot is the answer: a mono microphone fills one of the two,
     * and which one is a property of the board. Both are reported below. */
    level[0] = cap.tone_dbfs[0];
    level[1] = cap.tone_dbfs[1];
    return fmax(cap.tone_dbfs[0], cap.tone_dbfs[1]);
}

int cmd_audio_loopback(int argc, char **argv)
{
    if (!audio_bus_require() || !audio_bus_rx_require()) {
        return -1;
    }

    int dout = -1;
    audio_bus_pins(NULL, NULL, &dout, NULL, NULL);
    if (dout < 0) {
        diag_error("The bus has no DOUT pin, so there is nothing to play");
        return -1;
    }

    double hz = DEFAULT_LOOPBACK_HZ;
    int index = 1;
    if (argc > 1 && cli_parse_double_arg(argv[1], &hz) == 0) {
        index = 2;
    }

    audio_signal_t signal = {
        .start_hz = hz,
        .end_hz = hz,
        .logarithmic = false,
        .seconds = 0.0,          /* continuous; this command ends it */
        .level_pct = DEFAULT_LEVEL_PCT,
        .channel = AUDIO_CHANNEL_BOTH,
        .quiet = true,
    };
    double window = DEFAULT_LOOPBACK_SECONDS;

    while (index < argc) {
        if (strcasecmp(argv[index], "level") == 0) {
            if (index + 1 >= argc) {
                diag_error("'level' needs a percentage");
                return -1;
            }
            int pct = 0;
            if (cli_parse_int_arg(argv[index + 1], &pct) < 0 || pct < 1 || pct > 100) {
                diag_error("Level must be 1-100 percent of full scale");
                return -1;
            }
            signal.level_pct = pct;
            index += 2;
            continue;
        }
        if (cli_parse_double_arg(argv[index], &window) < 0 || window <= 0.0 ||
            window > 10.0) {
            diag_error("Unexpected argument '%s'", argv[index]);
            return -1;
        }
        index++;
    }

    if (!frequency_ok(hz, "Frequency")) {
        return -1;
    }
    uint32_t rx_rate = audio_bus_rx_rate();
    if (rx_rate && hz >= rx_rate / 2.0) {
        diag_error("%.0f Hz is above the receiver's Nyquist limit of %.0f Hz",
                 hz, rx_rate / 2.0);
        return -1;
    }
    if (audio_playing()) {
        diag_error("Something is already playing. Run 'audio stop' first.");
        return -1;
    }

    esp_err_t err = audio_bus_rx_enable(true);
    if (err != ESP_OK) {
        diag_error("Starting the receiver: %s", esp_err_to_name(err));
        return -1;
    }

    diag_printf("Testing whether the input hears %.0f Hz at %d%% from the "
              "output.\n", hz, signal.level_pct);

    /*
     * Measure the same frequency twice, silent and then playing, and report
     * the difference.
     *
     * A single measurement with the tone running cannot tell a microphone that
     * hears the speaker from one sitting next to a switching supply that
     * happens to whine near the test frequency. The quiet measurement is the
     * control, and the difference between the two is the only part of this
     * that is evidence.
     */
    audio_capture_flush(LOOPBACK_SETTLE_SECONDS);
    double quiet[2];
    double quiet_db = measure_tone(hz, window, quiet);

    if (audio_play(&signal) != 0) {
        return -1;
    }

    /* Let the transmit buffer drain into the wire and the part respond before
     * believing anything the receiver says. */
    audio_capture_flush(LOOPBACK_SETTLE_SECONDS);
    double loud[2];
    double loud_db = measure_tone(hz, window, loud);

    audio_play_stop();

    if (isnan(quiet_db) || isnan(loud_db)) {
        diag_error("Capture failed during the measurement");
        return -1;
    }

    diag_printf("\n%-8s %10s %10s %8s\n", "slot", "quiet", "playing", "change");
    for (int ch = 0; ch < 2; ch++) {
        diag_printf("%-8s %7.1f dB %7.1f dB %6.1f dB\n",
                  ch == 0 ? "left" : "right", quiet[ch], loud[ch],
                  loud[ch] - quiet[ch]);
    }

    double delta = loud_db - quiet_db;
    int slot = (loud[1] - quiet[1]) > (loud[0] - quiet[0]) ? 1 : 0;

    diag_printf("\n");
    if (delta >= LOOPBACK_HEARD_DB) {
        diag_printf("Heard it: %.0f Hz rose %.1f dB in the %s slot when the "
                  "output started.\n", hz, delta, slot ? "right" : "left");
        if (audio_bus_rx_internal()) {
            diag_printf("Internal loopback, so this proves the transmit and "
                      "capture paths and nothing outside them.\n");
        }
        return 0;
    }

    if (delta >= LOOPBACK_MARGINAL_DB) {
        diag_printf("Marginal: %.0f Hz rose only %.1f dB -- something is "
                  "getting through, but not a working path.\n", hz, delta);
    } else {
        diag_printf("Not heard: %.0f Hz did not rise when the output started.\n",
                  hz);
    }

    /* The useful part of a negative result is knowing which half to suspect,
     * and these are the questions in the order they are cheapest to answer. */
    diag_printf("Try in order: 'audio tone %.0f 3', 'audio record', then "
              "'audio loopback %.0f level 80'.\n", hz, hz);
    return -1;
}

/* ------------------------------------------------------------------ */
/* Generic codec operations                                            */
/* ------------------------------------------------------------------ */

/*
 * Volume and mute are output ideas first, so the output part answers for them
 * when there is one, and an input-only part gets the question otherwise -- a
 * codec with a microphone preamp may well have a gain worth setting.
 */
static const audio_codec_t *require_codec(const char *operation)
{
    const audio_codec_t *codec = tx_codec ? tx_codec : rx_codec;
    if (!codec) {
        diag_error("No codec attached, so there is nothing to %s", operation);
        diag_printf("Run 'audio codecs' for the parts this firmware knows, then "
                  "attach one (for example 'audio-nau8822 init').\n");
        return NULL;
    }
    return codec;
}

int cmd_audio_volume(int argc, char **argv)
{
    const audio_codec_t *codec = require_codec("set the volume on");
    if (!codec) {
        return -1;
    }

    if (!codec->set_volume) {
        /*
         * Not a failure of the command so much as of the hardware, and worth
         * saying plainly: a fixed-gain amplifier has nowhere to put a volume
         * setting, but the generator can still be turned down.
         */
        diag_error("%s has no volume control", codec->name);
        diag_printf("Use the digital level instead: 'audio tone 1000 3 level 10'.\n");
        return -1;
    }

    if (argc < 2) {
        if (last_volume_pct < 0) {
            diag_printf("Volume has not been set this session.\n");
        } else {
            diag_printf("Volume %d%%\n", last_volume_pct);
        }
        return 0;
    }

    int pct = 0;
    if (cli_parse_int_arg(argv[1], &pct) < 0 || pct < 0 || pct > 100) {
        diag_error("Volume must be 0-100 percent");
        return -1;
    }

    esp_err_t err = codec->set_volume(pct);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return -1; /* the driver has already explained; see audio_codec_t */
    }
    if (err != ESP_OK) {
        diag_error("Setting the volume on %s: %s", codec->name, esp_err_to_name(err));
        return -1;
    }

    last_volume_pct = pct;
    diag_printf("Volume %d%%\n", pct);
    return 0;
}

int cmd_audio_mute(int argc, char **argv)
{
    const audio_codec_t *codec = require_codec("mute");
    if (!codec) {
        return -1;
    }

    if (!codec->set_mute) {
        diag_error("%s has no mute control", codec->name);
        return -1;
    }

    bool mute = true;
    if (argc > 1) {
        if (strcasecmp(argv[1], "on") == 0 || strcasecmp(argv[1], "true") == 0) {
            mute = true;
        } else if (strcasecmp(argv[1], "off") == 0 || strcasecmp(argv[1], "false") == 0) {
            mute = false;
        } else {
            diag_error("Expected 'on' or 'off', not '%s'", argv[1]);
            return -1;
        }
    }

    esp_err_t err = codec->set_mute(mute);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return -1; /* the driver has already explained; see audio_codec_t */
    }
    if (err != ESP_OK) {
        diag_error("%s %s: %s", mute ? "Muting" : "Unmuting", codec->name,
                 esp_err_to_name(err));
        return -1;
    }

    diag_printf("%s %s\n", codec->name, mute ? "muted" : "unmuted");
    return 0;
}

int cmd_audio_close(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (audio_playing()) {
        audio_play_stop();
    }

    /*
     * The codec goes first. An amplifier still enabled when its clocks stop
     * thumps the speaker, and a codec left powered on a dead bus is the kind
     * of state that makes the next bring-up attempt confusing.
     */
    if (tx_codec) {
        diag_printf("Detaching %s\n", tx_codec->name);
    }
    if (rx_codec && rx_codec != tx_codec) {
        diag_printf("Detaching %s\n", rx_codec->name);
    }
    audio_codec_detach();

    bool had_rx = audio_bus_rx_ready();
    if (!audio_bus_ready() && !had_rx) {
        diag_printf("I2S was not initialized.\n");
        return 0;
    }

    audio_bus_close();
    diag_printf("I2S released%s.\n", had_rx ? ", receiver included" : "");
    return 0;
}
