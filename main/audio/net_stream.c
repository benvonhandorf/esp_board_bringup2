/*
 * `audio-net`: stream the open microphone to an HTTP listener. See net_stream.h.
 *
 * The rate is the receiver's, never converted: see usb_stream.c. A listener that asks
 * for another rate is refused, and told what the microphone runs at.
 */

#include "app_bringup.h"

#include "audio.h"
#include "net_stream.h"
#include "stream_common.h"

#include "audio_stream.h"
#include "audio_stream_http.h"

#include "esp_heap_caps.h"

#define OWNER                "audio-net"
#define BUFFER_MS_DEFAULT    1000
#define BUFFER_MS_MAX        10000
#define BUFFER_MS_MIN        20
/* Below this the drain task has nothing worth sending; the queue would refuse it. */
#define BUFFER_BYTES_MIN     16384
#define BUFFER_BYTES_MAX     (8u * 1024 * 1024)
/* What audio_stream_queue keeps free in internal RAM when it has to buffer there. */
#define INTERNAL_RESERVE     40960

/* What `config` last accepted: the defaults a listener gets without a query. A listener
 * reads these from the HTTP task while the console may be writing them; a torn read costs
 * one listener a request for a format the user was in the middle of changing. */
static audio_stream_want_t want;
static uint32_t buffer_ms = BUFFER_MS_DEFAULT;

static bool have_psram(void)
{
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
}

static size_t buffer_bytes(const audio_stream_fmt_t *fmt, uint32_t ms)
{
    uint64_t bytes = (uint64_t)fmt->rate_hz * fmt->channels * fmt->bytes * ms / 1000;
    if (bytes < BUFFER_BYTES_MIN) {
        bytes = BUFFER_BYTES_MIN;
    }
    if (bytes > BUFFER_BYTES_MAX) {
        bytes = BUFFER_BYTES_MAX;
    }
    return (size_t)bytes;
}

/* ---- config ---------------------------------------------------------------------------- */

static int buffer_option(int argc, char **argv, int i, void *ctx)
{
    if (strcasecmp(argv[i], "buffer") != 0) {
        return 0;
    }
    int ms;
    if (i + 1 >= argc || cli_parse_int_arg(argv[i + 1], &ms) < 0 || ms < BUFFER_MS_MIN ||
        ms > BUFFER_MS_MAX) {
        STRRES_ERROR(STR_AUDIO_NET_BUFFER_INVALID, BUFFER_MS_MIN, BUFFER_MS_MAX);
        return -1;
    }
    *(uint32_t *)ctx = (uint32_t)ms;
    return 2;
}

int cmd_audio_net_config(int argc, char **argv)
{
    /* Each call states the whole configuration; an omitted option goes back to
     * following the microphone, and the buffer to its default. */
    audio_stream_want_t next;
    const char *ch_text;
    uint32_t ms = BUFFER_MS_DEFAULT;
    if (stream_parse_options(argc, argv, 1, &next, &ch_text, buffer_option, &ms) < 0) {
        return -1;
    }

    audio_stream_source_t src;
    if (!stream_source_get(&src) || !stream_apply_channels(&src, ch_text, &next)) {
        return -1;
    }
    audio_stream_fmt_t fmt;
    audio_stream_why_t why;
    if (audio_stream_resolve(&src, &next, &fmt, &why) != ESP_OK) {
        stream_report(&why, ch_text);
        return -1;
    }

    const unsigned bytes_per_s = fmt.rate_hz * fmt.channels * fmt.bytes;
    const size_t bytes = buffer_bytes(&fmt, ms);
    STRRES_PRINTF(STR_AUDIO_NET_CONFIG_OK, (unsigned)fmt.channels, (unsigned)fmt.rate_hz,
                  fmt.bytes * 8u, bytes_per_s * 8u / 1000u);
    if (have_psram()) {
        STRRES_PRINTF(STR_AUDIO_NET_CONFIG_BUFFER_PSRAM, (unsigned)ms, (unsigned)bytes);
    } else {
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t room = largest > INTERNAL_RESERVE ? largest - INTERNAL_RESERVE : 0;
        if (bytes > room) {
            STRRES_PRINTF(STR_AUDIO_NET_CONFIG_BUFFER_SHRUNK, (unsigned)ms, (unsigned)room,
                          bytes_per_s ? (unsigned)((uint64_t)room * 1000 / bytes_per_s) : 0u);
        } else {
            STRRES_PRINTF(STR_AUDIO_NET_CONFIG_BUFFER_INTERNAL, (unsigned)ms, (unsigned)bytes);
        }
    }
    want = next;
    buffer_ms = ms;
    STRRES_PRINTF(STR_AUDIO_NET_CONFIG_LISTEN);
    return 0;
}

/* ---- the listener ---------------------------------------------------------------------- */

static esp_err_t reply(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, message);
}

/* HTTP responses are not console prose: they stay literals, like the ones in app_http.c. */
static esp_err_t refuse(httpd_req_t *req, const char *status, const audio_stream_why_t *why)
{
    char msg[160];
    switch (why->code) {
    case AUDIO_STREAM_ERR_BAD_RATE:
        if (why->raw_rates[0]) {
            snprintf(msg, sizeof msg,
                     "%u Hz is not offered; this microphone can run at %u, %u, %u or %u Hz\n",
                     (unsigned)why->want_rate, (unsigned)why->raw_rates[0],
                     (unsigned)why->raw_rates[1], (unsigned)why->raw_rates[2],
                     (unsigned)why->raw_rates[3]);
        } else if (why->want_rate) {
            snprintf(msg, sizeof msg,
                     "The microphone runs at %u Hz and cannot be resampled, so %u Hz is not possible\n",
                     (unsigned)why->src_rate, (unsigned)why->want_rate);
        } else {
            snprintf(msg, sizeof msg, "rate needs a number of hertz\n");
        }
        break;
    case AUDIO_STREAM_ERR_BAD_CHANNEL:
        snprintf(msg, sizeof msg, "Channel %d does not exist: the microphone has %d, numbered from 0\n",
                 why->channel, why->channels_available);
        break;
    case AUDIO_STREAM_ERR_BAD_BITS:
        snprintf(msg, sizeof msg, "bits must be 16, or 24 from a source with 24 or more data bits\n");
        break;
    case AUDIO_STREAM_ERR_BAD_LIST:
        snprintf(msg, sizeof msg, "ch must be a comma-separated list of channel numbers\n");
        break;
    case AUDIO_STREAM_ERR_DUP_CHANNEL:
        snprintf(msg, sizeof msg, "Channel %d is listed twice\n", why->channel);
        break;
    default:
        snprintf(msg, sizeof msg, "The microphone cannot be streamed\n");
        break;
    }
    return reply(req, status, msg);
}

esp_err_t net_stream_handle(httpd_req_t *req)
{
    audio_stream_source_t src;
    if (!stream_source_describe(&src)) {
        return reply(req, "409 Conflict", "No microphone is open, or it cannot be streamed\n");
    }

    audio_stream_want_t w = want;
    audio_stream_why_t why;
    if (audio_stream_http_parse_query(req, &src, &w, &why) != ESP_OK) {
        return refuse(req, "400 Bad Request", &why);
    }
    audio_stream_fmt_t fmt;
    if (audio_stream_resolve(&src, &w, &fmt, &why) != ESP_OK) {
        return refuse(req, "409 Conflict", &why);
    }

    const audio_stream_http_options_t opt = {
        .stream = {.owner = OWNER, .core = stream_task_core()},
        .queue = {.bytes = buffer_bytes(&fmt, buffer_ms), .use_psram = true,
                  .reserve_internal_bytes = INTERNAL_RESERVE},
    };
    esp_err_t err = audio_stream_http_serve(req, &src, &fmt, &opt);
    if (err == ESP_OK) {
        return ESP_OK;                  /* the response is the component's now */
    }
    if (err == AUDIO_STREAM_ERR_BUSY) {
        const char *owner = audio_stream_owner();
        char msg[96];
        if (owner) {
            snprintf(msg, sizeof msg, "The microphone is already being streamed (%s)\n", owner);
        } else {
            snprintf(msg, sizeof msg, "The previous listener is still closing; try again\n");
        }
        return reply(req, "409 Conflict", msg);
    }
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        err == ESP_ERR_NO_MEM ? "Out of memory" : "Could not start the stream");
    return ESP_OK;
}

/* ---- status and stop ------------------------------------------------------------------- */

int cmd_audio_net_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    audio_stream_http_stats_t hs;
    audio_stream_http_get_stats(&hs);
    audio_stream_stats_t as;
    audio_stream_get_stats(&as);
    /* The runner's counters are whoever streamed last; show them only if it was us. */
    const bool ours = as.owner && strcmp(as.owner, OWNER) == 0;
    const bool had_listener = hs.peer[0] || hs.bytes_sent > 0;

    if (!had_listener) {
        STRRES_PRINTF(STR_AUDIO_NET_STATUS_IDLE);
        return 0;
    }
    if (hs.active) {
        STRRES_PRINTF(STR_AUDIO_NET_STATUS_LISTENER, hs.peer, (unsigned)as.channels,
                      (unsigned)as.rate_hz, as.bytes * 8u, (unsigned)(hs.elapsed_ms / 1000));
    } else {
        STRRES_PRINTF(STR_AUDIO_NET_STATUS_LAST, hs.peer, (unsigned)(hs.elapsed_ms / 1000));
    }
    /* bits per millisecond is kilobits per second. */
    const unsigned avg_kbps = hs.elapsed_ms ? (unsigned)(hs.bytes_sent * 8 / hs.elapsed_ms) : 0;
    STRRES_PRINTF(STR_AUDIO_NET_STATUS_SENT, (unsigned long long)hs.bytes_sent, avg_kbps);

    /* What the microphone delivered, whatever the network did with it: a rate below the
     * nominal one here is the capture side falling behind, not WiFi. */
    if (ours && hs.elapsed_ms) {
        STRRES_PRINTF(STR_AUDIO_NET_STATUS_FRAMES, (unsigned long long)as.frames,
                      (double)as.frames / (double)hs.elapsed_ms, (unsigned)as.rate_hz);
    }

    const audio_stream_queue_stats_t *q = &hs.queue;
    const unsigned bytes_per_s = ours ? as.rate_hz * as.channels * as.bytes : 0;
    STRRES_PRINTF(STR_AUDIO_NET_STATUS_BUFFER, (unsigned)q->fill_bytes, (unsigned)q->capacity_bytes,
                  q->in_psram ? "PSRAM" : "internal RAM", (unsigned)q->high_water_bytes,
                  bytes_per_s ? (unsigned)((uint64_t)q->high_water_bytes * 1000 / bytes_per_s) : 0u);
    STRRES_PRINTF(STR_AUDIO_NET_STATUS_COUNTS, (unsigned long long)q->bytes_dropped,
                  (unsigned)q->drops, ours ? (unsigned)as.read_errors : 0u);

    /* The software decimator of a raw source reports its own cost; a hardware-filtered
     * ultrasonic microphone has the load the receiver measured. */
    uint64_t cycles = 0, frames = 0;
    if (ours && as.dec_frames > 0) {
        cycles = as.dec_cycles;
        frames = as.dec_frames;
    } else if (!ours || !audio_bus_pdm_ultra_load(&cycles, &frames)) {
        frames = 0;
    }
    if (frames > 0) {
        double per_frame = (double)cycles / (double)frames;
        double load = per_frame * (double)as.rate_hz /
                      ((double)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1e6) * 100.0;
        STRRES_PRINTF(STR_AUDIO_NET_STATUS_DECIMATOR, per_frame, load);
    }
    if (!hs.active) {
        if (hs.send_failed) {
            STRRES_PRINTF(STR_AUDIO_NET_STATUS_ENDED_LISTENER);
        } else if (ours && as.has_ended && as.ended == AUDIO_STREAM_END_SOURCE_GONE) {
            STRRES_PRINTF(STR_AUDIO_NET_STATUS_ENDED_GONE);
        } else if (ours && as.has_ended && as.ended == AUDIO_STREAM_END_READ_ERRORS) {
            STRRES_PRINTF(STR_AUDIO_NET_STATUS_ENDED_ERRORS);
        } else {
            STRRES_PRINTF(STR_AUDIO_NET_STATUS_ENDED_STOPPED);
        }
    }
    return 0;
}

int cmd_audio_net_stop(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (audio_stream_http_stop() != ESP_OK) {
        STRRES_ERROR(STR_AUDIO_NET_NOT_RUNNING);
        return -1;
    }
    STRRES_PRINTF(STR_AUDIO_NET_STOPPED);
    return 0;
}
