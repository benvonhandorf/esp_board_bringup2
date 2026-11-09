/*
 * `audio-usb`: stream the open microphone to a USB host. See usb_stream.h.
 *
 * The rate is the receiver's, never converted: this does no resampling, and a
 * decimating PDM receiver already offers the rates a stream wants. Choosing another
 * rate is reopening the microphone at it (`audio pdm ... rate`, `audio pdmultra ...
 * maxhz`), which is why `config` validates `rate` rather than setting it.
 */

#include "app_bringup.h"

#include "audio.h"
#include "usb_stream.h"

#include "soc/soc_caps.h"
#include "pdm_decimator.h"
#include "usb_mic.h"

#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/semphr.h"

#define STREAM_BLOCK_MS         5
#define STREAM_MAX_READ_ERRORS  20
#define STREAM_TASK_STACK       4096
#define STREAM_TASK_PRIORITY    6

/* What the open microphone offers. */
typedef struct {
    audio_rx_mode_t mode;
    uint32_t rate;
    int channels;
    uint8_t in_bits;       /* data bits per sample: 16, 24 or 32 */
    size_t slot_bytes;     /* 2 or 4 */
    size_t frame_bytes;
    /* An undecimated PDM receiver (audio pdmraw, audio pdmultra ... raw): the stream
     * task decimates it, so the rate is chosen here, as the clock over a ratio. */
    bool raw;
    uint32_t pdm_clk;
} source_t;

/* The ratios pdm_decimator's sixth-order sinc offers, bit clocks per output sample.
 * 64 is the standard PDM oversampling, so it is the default: a 3.072 MHz clock gives
 * 48 kHz. */
static const int raw_ratios[] = {64, 48, 32, 16};
#define RAW_RATIO_DEFAULT 64


/* What the user asked for with `config`; zero fields mean "follow the source". */
static struct {
    uint32_t rate;
    uint8_t bits;
    int nch;
    uint8_t chan[USB_MIC_MAX_CHANNELS];
} want;

static struct {
    bool running;
    volatile bool stop;
    volatile bool ended_gone;
    volatile bool ended_errors;
    TaskHandle_t task;
    SemaphoreHandle_t exited;
    source_t src;
    usb_mic_config_t mic;
    uint8_t chan[USB_MIC_MAX_CHANNELS];
    uint8_t *in;
    uint8_t *out;
    size_t block_frames;
    uint32_t read_errors;
    pdm_decimator_t *dec;     /* raw sources only */
    uint8_t *raw;
    size_t raw_frames;        /* raw frames per block */
    uint64_t dec_cycles, dec_frames;
    esp_timer_handle_t reset_timer;   /* resets the chip when `start timeout` expires */
} run;

/* On a timer rather than in the stream task: a stream that has stalled or died is
 * exactly when the serial console needs to come back. */
static void reset_cb(void *arg)
{
    (void)arg;
    /* Not esp_restart(): its shutdown handlers flush the console, and the console is
     * USB-Serial-JTAG, whose pins the USB device now owns -- a flush with no host can
     * wait forever. This is the reset that cannot get stuck. */
    esp_rom_software_reset_system();
}

/* ---- the source ------------------------------------------------------------------ */

/* Describes the open receiver without saying anything; false when there is none or
 * it cannot be streamed. */
static bool source_describe(source_t *s)
{
    if (!audio_bus_rx_ready()) {
        return false;
    }
    s->mode = audio_bus_rx_mode();
    s->raw = (s->mode == AUDIO_RX_PDM_RAW);
    s->channels = audio_bus_rx_channels();
    s->in_bits = audio_bus_rx_bits();
    s->frame_bytes = audio_bus_rx_frame_bytes();
    s->slot_bytes = s->channels ? s->frame_bytes / (size_t)s->channels : 0;
    s->pdm_clk = s->raw ? audio_bus_pdm_clk_hz() : 0;
    /* A raw source has no rate of its own until a ratio is chosen. */
    s->rate = s->raw ? 0 : audio_bus_rx_rate();
    if (s->raw) {
        return s->pdm_clk > 0 && s->channels > 0 && s->channels <= PDM_DECIMATOR_MAX_CHANNELS;
    }
    return s->rate > 0 && s->channels > 0 && (s->slot_bytes == 2 || s->slot_bytes == 4);
}

/* As source_describe(), reporting the reason when it fails. */
static bool source_get(source_t *s)
{
    if (!audio_bus_rx_ready()) {
        STRRES_ERROR(STR_AUDIO_USB_NO_RECEIVER);
        return false;
    }
    return source_describe(s);
}

/* ---- configuration ---------------------------------------------------------------- */

/* Parses "0,1,3" into out[]; reports its own errors. Returns the count or -1. */
static int parse_channels(const char *text, int nch_available, uint8_t *out)
{
    int count = 0;
    const char *p = text;
    while (*p) {
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p || (*end != ',' && *end != '\0') || v < 0 || v > 255) {
            STRRES_ERROR(STR_AUDIO_USB_CH_INVALID, text);
            return -1;
        }
        if (v >= nch_available) {
            STRRES_ERROR(STR_AUDIO_USB_CH_RANGE, (int)v, nch_available);
            return -1;
        }
        for (int i = 0; i < count; i++) {
            if (out[i] == v) {
                STRRES_ERROR(STR_AUDIO_USB_CH_DUPLICATE, (int)v);
                return -1;
            }
        }
        if (count >= USB_MIC_MAX_CHANNELS) {
            STRRES_ERROR(STR_AUDIO_USB_CH_INVALID, text);
            return -1;
        }
        out[count++] = (uint8_t)v;
        p = (*end == ',') ? end + 1 : end;
        if (*end == ',' && *p == '\0') {
            STRRES_ERROR(STR_AUDIO_USB_CH_INVALID, text);
            return -1;
        }
    }
    if (count == 0) {
        STRRES_ERROR(STR_AUDIO_USB_CH_INVALID, text);
        return -1;
    }
    return count;
}

/* Turns `want` and the source into a stream. Reports its own errors. */
static bool resolve(const source_t *src, usb_mic_config_t *mic, uint8_t *chan, int *ratio)
{
    uint32_t rate = src->rate;
    *ratio = 0;
    if (src->raw) {
        *ratio = want.rate ? 0 : RAW_RATIO_DEFAULT;
        for (size_t i = 0; want.rate && i < sizeof(raw_ratios) / sizeof(raw_ratios[0]); i++) {
            if (src->pdm_clk / (uint32_t)raw_ratios[i] == want.rate &&
                src->pdm_clk % (uint32_t)raw_ratios[i] == 0) {
                *ratio = raw_ratios[i];
            }
        }
        if (*ratio == 0) {
            STRRES_ERROR(STR_AUDIO_USB_RAW_RATE_OPTIONS, (unsigned)want.rate,
                         (unsigned)(src->pdm_clk / 64), (unsigned)(src->pdm_clk / 48),
                         (unsigned)(src->pdm_clk / 32), (unsigned)(src->pdm_clk / 16));
            return false;
        }
        rate = src->pdm_clk / (uint32_t)*ratio;
    } else if (want.rate && want.rate != src->rate) {
        STRRES_ERROR(STR_AUDIO_USB_RATE_MISMATCH, (unsigned)src->rate, (unsigned)want.rate);
        return false;
    }
    int nch = want.nch ? want.nch : src->channels;
    for (int i = 0; i < nch; i++) {
        chan[i] = want.nch ? want.chan[i] : (uint8_t)i;
        if (chan[i] >= src->channels) {      /* the microphone changed since `config` */
            STRRES_ERROR(STR_AUDIO_USB_CH_RANGE, chan[i], src->channels);
            return false;
        }
    }
    uint8_t bits = want.bits ? want.bits : 16;
    if (bits == 24 && src->in_bits < 24) {
        STRRES_ERROR(STR_AUDIO_USB_BITS_UNAVAILABLE, src->in_bits);
        return false;
    }
    mic->rate_hz = rate;
    mic->channels = (uint8_t)nch;
    mic->bytes = (uint8_t)(bits / 8);
    return true;
}

/* Runs the bandwidth check and prints the verdict; true when the stream fits. */
static bool check_and_report(const usb_mic_controller_t *ctrl, const usb_mic_config_t *mic)
{
    usb_mic_check_t chk;
    esp_err_t err = usb_mic_check(ctrl, mic, &chk);
    if (err == ESP_OK) {
        STRRES_PRINTF(STR_AUDIO_USB_CONFIG_OK, mic->channels, (unsigned)mic->rate_hz,
                      mic->bytes * 8u, (unsigned)chk.needed, (unsigned)chk.budget);
        return true;
    }
    STRRES_ERROR(STR_AUDIO_USB_CONFIG_OVER, mic->channels, (unsigned)mic->rate_hz,
                 mic->bytes * 8u, (unsigned)chk.needed, (unsigned)chk.budget);
    if (chk.max_channels > 0) {
        STRRES_PRINTF(STR_AUDIO_USB_CONFIG_FIT_CHANNELS, (unsigned)mic->rate_hz, chk.max_channels);
    } else {
        STRRES_PRINTF(STR_AUDIO_USB_CONFIG_FIT_NONE, (unsigned)mic->rate_hz);
    }
    if (chk.max_rate_hz > 0) {
        STRRES_PRINTF(STR_AUDIO_USB_CONFIG_FIT_RATE, mic->channels, (unsigned)chk.max_rate_hz);
    }
    return false;
}

static bool have_usb_chip(const usb_mic_controller_t **ctrl)
{
    *ctrl = usb_mic_controller();
    if (!*ctrl) {
        STRRES_ERROR(STR_AUDIO_USB_NO_USB_CHIP, CONFIG_IDF_TARGET);
        return false;
    }
    return true;
}

int cmd_audio_usb_config(int argc, char **argv)
{
    /* Each call states the whole configuration; an omitted option goes back to
     * following the microphone. */
    typeof(want) next = {0};
    for (int i = 1; i < argc;) {
        if (strcasecmp(argv[i], "rate") == 0) {
            int hz;
            if (i + 1 >= argc || cli_parse_int_arg(argv[i + 1], &hz) < 0 || hz <= 0) {
                STRRES_ERROR(STR_AUDIO_USB_RATE_INVALID);
                return -1;
            }
            next.rate = (uint32_t)hz;
            i += 2;
        } else if (strcasecmp(argv[i], "bits") == 0) {
            int bits;
            if (i + 1 >= argc || cli_parse_int_arg(argv[i + 1], &bits) < 0 ||
                (bits != 16 && bits != 24)) {
                STRRES_ERROR(STR_AUDIO_USB_BITS_INVALID);
                return -1;
            }
            next.bits = (uint8_t)bits;
            i += 2;
        } else if (strcasecmp(argv[i], "ch") == 0) {
            if (i + 1 >= argc) {
                STRRES_ERROR(STR_AUDIO_USB_CH_INVALID, "");
                return -1;
            }
            i += 2;
        } else {
            STRRES_ERROR(STR_AUDIO_USB_UNEXPECTED_ARGUMENT, argv[i]);
            return -1;
        }
    }

    const usb_mic_controller_t *ctrl;
    source_t src;
    if (!have_usb_chip(&ctrl) || !source_get(&src)) {
        return -1;
    }

    for (int i = 1; i + 1 < argc; i++) {
        if (strcasecmp(argv[i], "ch") == 0) {
            next.nch = parse_channels(argv[i + 1], src.channels, next.chan);
            if (next.nch < 0) {
                return -1;
            }
            break;
        }
    }

    usb_mic_config_t mic;
    uint8_t chan[USB_MIC_MAX_CHANNELS];
    int ratio;
    typeof(want) previous = want;
    want = next;
    if (!resolve(&src, &mic, chan, &ratio)) {
        want = previous;
        return -1;
    }
    if (!check_and_report(ctrl, &mic)) {
        want = previous;
        return -1;
    }
    STRRES_PRINTF(STR_AUDIO_USB_CONFIG_START_NEXT);
    return 0;
}

int cmd_audio_usb_budget(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const usb_mic_controller_t *ctrl;
    if (!have_usb_chip(&ctrl)) {
        return -1;
    }
    STRRES_PRINTF(STR_AUDIO_USB_BUDGET_HEADER, (unsigned)usb_mic_budget_bytes(ctrl),
                  ctrl->speed == USB_MIC_SPEED_HIGH ? "high" : "full",
                  usb_mic_interval_us(ctrl));

    uint32_t rates[16] = {8000, 16000, 32000, 44100, 48000, 96000, 100000, 150000, 192000, 300000};
    size_t n = 10;
    source_t src;
    bool have_src = source_describe(&src);
    if (have_src && src.raw) {
        STRRES_PRINTF(STR_AUDIO_USB_BUDGET_RAW_HINT, (unsigned)(src.pdm_clk / 64),
                      (unsigned)(src.pdm_clk / 48), (unsigned)(src.pdm_clk / 32),
                      (unsigned)(src.pdm_clk / 16));
    } else if (have_src) {
        bool known = false;
        for (size_t i = 0; i < n; i++) {
            known |= rates[i] == src.rate;
        }
        if (!known) {
            rates[n++] = src.rate;
        }
        STRRES_PRINTF(STR_AUDIO_USB_BUDGET_OPEN_HINT, (unsigned)src.rate);
    }
    for (size_t i = 0; i < n; i++) {
        STRRES_PRINTF(STR_AUDIO_USB_BUDGET_ROW, (unsigned)rates[i],
                      usb_mic_max_channels(ctrl, rates[i], 2),
                      usb_mic_max_channels(ctrl, rates[i], 3));
    }
    return 0;
}

/* ---- the stream task ------------------------------------------------------------- */

/* One input sample to the output width. `slot` is the receiver's slot, 16 or 32 bits
 * with the data left-justified; the output is 16-bit or 24-bit little-endian. */
static inline uint8_t *put_sample(uint8_t *out, const uint8_t *in, size_t slot, uint8_t out_bytes)
{
    if (slot == 2) {
        int16_t v;
        memcpy(&v, in, 2);
        memcpy(out, &v, 2);      /* 24-bit output from a 16-bit source is refused earlier */
        return out + 2;
    }
    int32_t v;
    memcpy(&v, in, 4);
    if (out_bytes == 2) {
        int16_t s = (int16_t)(v >> 16);
        memcpy(out, &s, 2);
        return out + 2;
    }
    int32_t s = v >> 8;          /* the top 24 bits */
    out[0] = (uint8_t)s;
    out[1] = (uint8_t)(s >> 8);
    out[2] = (uint8_t)(s >> 16);
    return out + 3;
}

static void stream_task(void *arg)
{
    (void)arg;
    const size_t out_frame = (size_t)run.mic.channels * run.mic.bytes;
    unsigned consecutive = 0;

    while (!run.stop) {
        if (!audio_bus_rx_ready()) {
            run.ended_gone = true;
            break;
        }
        size_t frames = 0;
        esp_err_t err;
        if (run.dec) {
            /* Undecimated source: raw 16-bit words in, PCM out. One raw frame is one
             * word per channel, the same size as one decimated frame. */
            size_t got = 0;
            err = audio_bus_read(run.raw, run.raw_frames * run.src.frame_bytes, &got, 100);
            size_t raw_frames = got / run.src.frame_bytes;
            if (err == ESP_OK && raw_frames > 0) {
                uint32_t t0 = esp_cpu_get_cycle_count();
                err = pdm_decimator_process(run.dec, (const uint16_t *)run.raw, raw_frames,
                                            (int16_t *)run.in, &frames);
                run.dec_cycles += (uint32_t)(esp_cpu_get_cycle_count() - t0);
                run.dec_frames += frames;
            }
        } else {
            size_t got = 0;
            err = audio_bus_read(run.in, run.block_frames * run.src.frame_bytes, &got, 100);
            frames = got / run.src.frame_bytes;
        }
        if (err != ESP_OK || frames == 0) {
            run.read_errors++;
            if (++consecutive >= STREAM_MAX_READ_ERRORS) {
                run.ended_errors = true;
                break;
            }
            continue;
        }
        consecutive = 0;

        uint8_t *dst = run.out;
        for (size_t f = 0; f < frames; f++) {
            const uint8_t *frame = run.in + f * run.src.frame_bytes;
            for (int c = 0; c < run.mic.channels; c++) {
                dst = put_sample(dst, frame + run.chan[c] * run.src.slot_bytes,
                                 run.src.slot_bytes, run.mic.bytes);
            }
        }
        usb_mic_write(run.out, frames * out_frame);
    }
    xSemaphoreGive(run.exited);
    vTaskDelete(NULL);
}

static void release_buffers(void)
{
    pdm_decimator_destroy(run.dec);
    run.dec = NULL;
    free(run.raw);
    free(run.in);
    free(run.out);
    run.raw = run.in = run.out = NULL;
}

int cmd_audio_usb_start(int argc, char **argv)
{
    if (run.running) {
        STRRES_ERROR(STR_AUDIO_USB_ALREADY_RUNNING);
        return -1;
    }
    int timeout_s = 0;
    for (int i = 1; i < argc;) {
        if (strcasecmp(argv[i], "timeout") == 0) {
            if (i + 1 >= argc || cli_parse_int_arg(argv[i + 1], &timeout_s) < 0 ||
                timeout_s < 1 || timeout_s > 86400) {
                STRRES_ERROR(STR_AUDIO_USB_TIMEOUT_INVALID);
                return -1;
            }
            i += 2;
        } else {
            STRRES_ERROR(STR_AUDIO_USB_UNEXPECTED_ARGUMENT, argv[i]);
            return -1;
        }
    }
    const usb_mic_controller_t *ctrl;
    source_t src;
    usb_mic_config_t mic;
    uint8_t chan[USB_MIC_MAX_CHANNELS];
    int ratio;
    if (!have_usb_chip(&ctrl) || !source_get(&src) || !resolve(&src, &mic, chan, &ratio) ||
        !check_and_report(ctrl, &mic)) {
        return -1;
    }

    run.src = src;
    run.mic = mic;
    memcpy(run.chan, chan, sizeof run.chan);
    run.block_frames = mic.rate_hz * STREAM_BLOCK_MS / 1000;
    if (run.block_frames < 8) {
        run.block_frames = 8;
    }
    run.dec_cycles = run.dec_frames = 0;
    size_t in_frames = run.block_frames;
    if (src.raw) {
        pdm_decimator_config_t dcfg = {
            .filter = PDM_DECIMATOR_SINC6,
            .channels = src.channels,
            .ratio = ratio,
            .lsb_first = false,        /* the raw slot format audio_bus_open_pdm_raw() asks for */
            .allow_simd = true,
        };
        esp_err_t derr = pdm_decimator_create(&dcfg, &run.dec);
        if (derr != ESP_OK) {
            STRRES_ERROR(STR_AUDIO_USB_DECIMATOR_FAILED, esp_err_to_name(derr));
            return -1;
        }
        run.raw_frames = pdm_decimator_input_frames(run.dec, run.block_frames);
        in_frames = pdm_decimator_max_output(run.dec, run.raw_frames);
        pdm_decimator_reserve(run.dec, run.raw_frames);
        run.raw = malloc(run.raw_frames * src.frame_bytes);
        if (!run.raw) {
            release_buffers();
            STRRES_ERROR(STR_AUDIO_USB_START_NO_TASK);
            return -1;
        }
    }
    run.in = malloc(in_frames * src.frame_bytes);
    run.out = malloc(in_frames * mic.channels * mic.bytes);
    if (!run.in || !run.out) {
        release_buffers();
        STRRES_ERROR(STR_AUDIO_USB_START_NO_TASK);
        return -1;
    }
    if (!run.exited) {
        run.exited = xSemaphoreCreateBinary();
    }
    run.stop = run.ended_gone = run.ended_errors = false;
    run.read_errors = 0;

    /* The serial console ends when USB takes the PHY: say so first, and give the
     * line time to leave. */
    STRRES_PRINTF(STR_AUDIO_USB_START_WARN);
    /* Logging goes to the same console. */
    esp_log_level_set("*", ESP_LOG_NONE);
    if (timeout_s) {
        STRRES_PRINTF(STR_AUDIO_USB_START_TIMEOUT, (unsigned)timeout_s);
        if (!run.reset_timer) {
            const esp_timer_create_args_t targs = {.callback = reset_cb, .name = "usb_reset"};
            esp_timer_create(&targs, &run.reset_timer);
        }
        esp_timer_start_once(run.reset_timer, (uint64_t)timeout_s * 1000000 + 300000);
    }
    vTaskDelay(pdMS_TO_TICKS(200));

    usb_mic_device_config_t dev = {.stream = mic, .task_core = 0};
    esp_err_t err = usb_mic_start(&dev, NULL);
    if (err != ESP_OK) {
        release_buffers();
        STRRES_ERROR(STR_AUDIO_USB_START_FAILED, esp_err_to_name(err));
        return -1;
    }
    /* The stream task on the other core from TinyUSB's, so reading and decimating
     * never wait for the USB stack. */
    if (xTaskCreatePinnedToCore(stream_task, "usb_stream", STREAM_TASK_STACK, NULL,
                                STREAM_TASK_PRIORITY, &run.task, 1) != pdPASS) {
        usb_mic_stop();
        release_buffers();
        STRRES_ERROR(STR_AUDIO_USB_START_NO_TASK);
        return -1;
    }
    run.running = true;
    STRRES_PRINTF(STR_AUDIO_USB_STARTED, mic.channels, (unsigned)mic.rate_hz);
    return 0;
}

int cmd_audio_usb_stop(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!run.running) {
        STRRES_ERROR(STR_AUDIO_USB_NOT_RUNNING);
        return -1;
    }
    if (run.reset_timer) {
        esp_timer_stop(run.reset_timer);
    }
    run.stop = true;
    xSemaphoreTake(run.exited, portMAX_DELAY);
    usb_mic_stop();
    release_buffers();
    run.running = false;
    STRRES_PRINTF(STR_AUDIO_USB_STOPPED);
    return 0;
}

int cmd_audio_usb_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!run.running) {
        STRRES_PRINTF(STR_AUDIO_USB_STATUS_IDLE);
        return 0;
    }
    usb_mic_stats_t st;
    usb_mic_get_stats(&st);
    STRRES_PRINTF(STR_AUDIO_USB_STATUS_RUNNING, st.channels, (unsigned)st.rate_hz, st.bytes * 8u,
                  (unsigned)st.packet_bytes,
                  (unsigned)usb_mic_budget_bytes(usb_mic_controller()));
    if (st.streaming) {
        STRRES_PRINTF(STR_AUDIO_USB_STATUS_HOST_STREAMING);
    } else if (st.mounted) {
        STRRES_PRINTF(STR_AUDIO_USB_STATUS_HOST_MOUNTED);
    } else {
        STRRES_PRINTF(STR_AUDIO_USB_STATUS_HOST_IDLE);
    }
    STRRES_PRINTF(STR_AUDIO_USB_STATUS_COUNTS, (unsigned long long)st.bytes_written,
                  (unsigned long long)st.bytes_dropped, (unsigned)st.overruns,
                  (unsigned)run.read_errors);
    STRRES_PRINTF(STR_AUDIO_USB_STATUS_BUFFER, (unsigned)st.buffered_bytes,
                  (unsigned)st.buffer_bytes);

    uint64_t cycles = 0, frames = 0;
    if (run.dec) {
        cycles = run.dec_cycles;
        frames = run.dec_frames;
    } else if (!audio_bus_pdm_ultra_load(&cycles, &frames)) {
        frames = 0;
    }
    if (frames > 0) {
        double per_frame = (double)cycles / (double)frames;
        double load = per_frame * (double)st.rate_hz /
                      ((double)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1e6) * 100.0;
        STRRES_PRINTF(STR_AUDIO_USB_STATUS_DECIMATOR, per_frame, load);
    }
    if (run.ended_gone) {
        STRRES_PRINTF(STR_AUDIO_USB_STATUS_ENDED_GONE);
    } else if (run.ended_errors) {
        STRRES_PRINTF(STR_AUDIO_USB_STATUS_ENDED_ERRORS);
    }
    return 0;
}
