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
#include "stream_common.h"
#include "usb_stream.h"

#include "audio_stream.h"
#include "audio_stream_usb.h"
#include "usb_mic.h"

#include "esp_rom_sys.h"
#include "esp_timer.h"

/* What the user asked for with `config`; zero fields mean "follow the source". */
static audio_stream_want_t want;

static esp_timer_handle_t reset_timer;    /* resets the chip when `start timeout` expires */

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

static bool have_usb_chip(const usb_mic_controller_t **ctrl)
{
    *ctrl = usb_mic_controller();
    if (!*ctrl) {
        STRRES_ERROR(STR_AUDIO_USB_NO_USB_CHIP, CONFIG_IDF_TARGET);
        return false;
    }
    return true;
}

/* Runs the bandwidth check and prints the verdict; true when the stream fits. */
static bool check_and_report(const audio_stream_fmt_t *fmt)
{
    usb_mic_check_t chk;
    esp_err_t err = audio_stream_usb_check(fmt, &chk);
    if (err == ESP_OK) {
        STRRES_PRINTF(STR_AUDIO_USB_CONFIG_OK, (unsigned)fmt->channels, (unsigned)fmt->rate_hz,
                      fmt->bytes * 8u, (unsigned)chk.needed, (unsigned)chk.budget);
        return true;
    }
    STRRES_ERROR(STR_AUDIO_USB_CONFIG_OVER, (unsigned)fmt->channels, (unsigned)fmt->rate_hz,
                 fmt->bytes * 8u, (unsigned)chk.needed, (unsigned)chk.budget);
    if (chk.max_channels > 0) {
        STRRES_PRINTF(STR_AUDIO_USB_CONFIG_FIT_CHANNELS, (unsigned)fmt->rate_hz, chk.max_channels);
    } else {
        STRRES_PRINTF(STR_AUDIO_USB_CONFIG_FIT_NONE, (unsigned)fmt->rate_hz);
    }
    if (chk.max_rate_hz > 0) {
        STRRES_PRINTF(STR_AUDIO_USB_CONFIG_FIT_RATE, (unsigned)fmt->channels,
                      (unsigned)chk.max_rate_hz);
    }
    return false;
}

int cmd_audio_usb_config(int argc, char **argv)
{
    /* Each call states the whole configuration; an omitted option goes back to
     * following the microphone. */
    audio_stream_want_t next;
    const char *ch_text;
    if (stream_parse_options(argc, argv, 1, &next, &ch_text, NULL, NULL) < 0) {
        return -1;
    }

    const usb_mic_controller_t *ctrl;
    audio_stream_source_t src;
    if (!have_usb_chip(&ctrl) || !stream_source_get(&src) ||
        !stream_apply_channels(&src, ch_text, &next)) {
        return -1;
    }

    audio_stream_fmt_t fmt;
    audio_stream_why_t why;
    if (audio_stream_resolve(&src, &next, &fmt, &why) != ESP_OK) {
        stream_report(&why, ch_text);
        return -1;
    }
    if (!check_and_report(&fmt)) {
        return -1;
    }
    want = next;
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
    audio_stream_source_t src;
    bool have_src = stream_source_describe(&src);
    if (have_src && src.pdm_raw) {
        STRRES_PRINTF(STR_AUDIO_USB_BUDGET_RAW_HINT, (unsigned)(src.pdm_clk_hz / 64),
                      (unsigned)(src.pdm_clk_hz / 48), (unsigned)(src.pdm_clk_hz / 32),
                      (unsigned)(src.pdm_clk_hz / 16));
    } else if (have_src) {
        bool known = false;
        for (size_t i = 0; i < n; i++) {
            known |= rates[i] == src.rate_hz;
        }
        if (!known) {
            rates[n++] = src.rate_hz;
        }
        STRRES_PRINTF(STR_AUDIO_USB_BUDGET_OPEN_HINT, (unsigned)src.rate_hz);
    }
    for (size_t i = 0; i < n; i++) {
        STRRES_PRINTF(STR_AUDIO_USB_BUDGET_ROW, (unsigned)rates[i],
                      usb_mic_max_channels(ctrl, rates[i], 2),
                      usb_mic_max_channels(ctrl, rates[i], 3));
    }
    return 0;
}

/* Called by the sink as the last moment the serial console is still there: say so first,
 * and give the line time to leave. */
static void before_usb(void *ctx)
{
    const int timeout_s = *(const int *)ctx;

    STRRES_PRINTF(STR_AUDIO_USB_START_WARN);
    /* Logging goes to the same console. */
    esp_log_level_set("*", ESP_LOG_NONE);
    if (timeout_s) {
        STRRES_PRINTF(STR_AUDIO_USB_START_TIMEOUT, (unsigned)timeout_s);
        if (!reset_timer) {
            const esp_timer_create_args_t targs = {.callback = reset_cb, .name = "usb_reset"};
            esp_timer_create(&targs, &reset_timer);
        }
        esp_timer_start_once(reset_timer, (uint64_t)timeout_s * 1000000 + 300000);
    }
    vTaskDelay(pdMS_TO_TICKS(200));
}

int cmd_audio_usb_start(int argc, char **argv)
{
    if (audio_stream_usb_active()) {
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
            STRRES_ERROR(STR_AUDIO_STREAM_UNEXPECTED_ARGUMENT, argv[i]);
            return -1;
        }
    }

    const usb_mic_controller_t *ctrl;
    audio_stream_source_t src;
    audio_stream_fmt_t fmt;
    audio_stream_why_t why;
    if (!have_usb_chip(&ctrl) || !stream_source_get(&src)) {
        return -1;
    }
    if (audio_stream_resolve(&src, &want, &fmt, &why) != ESP_OK) {
        stream_report(&why, NULL);
        return -1;
    }
    if (!check_and_report(&fmt)) {
        return -1;
    }

    const audio_stream_usb_options_t opt = {
        .stream = {.owner = "audio-usb", .core = stream_task_core()},
        .before_usb = before_usb,
        .before_ctx = &timeout_s,
        .usb_task_core = 0,
    };
    esp_err_t err = audio_stream_usb_start(&src, &fmt, &opt);
    if (err != ESP_OK) {
        /* A reset timer armed by before_usb() is left running: if USB took the PHY before
         * failing, the reset is the only way the console comes back. */
        if (!stream_report_start_error(err)) {
            STRRES_ERROR(STR_AUDIO_USB_START_FAILED, esp_err_to_name(err));
        }
        return -1;
    }
    STRRES_PRINTF(STR_AUDIO_USB_STARTED, (unsigned)fmt.channels, (unsigned)fmt.rate_hz);
    return 0;
}

int cmd_audio_usb_stop(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!audio_stream_usb_active()) {
        STRRES_ERROR(STR_AUDIO_USB_NOT_RUNNING);
        return -1;
    }
    if (reset_timer) {
        esp_timer_stop(reset_timer);
    }
    audio_stream_usb_stop();
    STRRES_PRINTF(STR_AUDIO_USB_STOPPED);
    return 0;
}

int cmd_audio_usb_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!audio_stream_usb_active()) {
        STRRES_PRINTF(STR_AUDIO_USB_STATUS_IDLE);
        return 0;
    }
    usb_mic_stats_t st;
    usb_mic_get_stats(&st);
    audio_stream_stats_t as;
    audio_stream_get_stats(&as);

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
                  (unsigned)as.read_errors);
    STRRES_PRINTF(STR_AUDIO_USB_STATUS_BUFFER, (unsigned)st.buffered_bytes,
                  (unsigned)st.buffer_bytes);

    /* The software decimator of a raw source reports its own cost; a hardware-filtered
     * ultrasonic microphone has the load the receiver measured. */
    uint64_t cycles = 0, frames = 0;
    if (as.dec_frames > 0) {
        cycles = as.dec_cycles;
        frames = as.dec_frames;
    } else if (!audio_bus_pdm_ultra_load(&cycles, &frames)) {
        frames = 0;
    }
    if (frames > 0) {
        double per_frame = (double)cycles / (double)frames;
        double load = per_frame * (double)st.rate_hz /
                      ((double)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1e6) * 100.0;
        STRRES_PRINTF(STR_AUDIO_USB_STATUS_DECIMATOR, per_frame, load);
    }
    if (as.has_ended && !as.running) {
        if (as.ended == AUDIO_STREAM_END_SOURCE_GONE) {
            STRRES_PRINTF(STR_AUDIO_USB_STATUS_ENDED_GONE);
        } else if (as.ended == AUDIO_STREAM_END_READ_ERRORS) {
            STRRES_PRINTF(STR_AUDIO_USB_STATUS_ENDED_ERRORS);
        }
    }
    return 0;
}
