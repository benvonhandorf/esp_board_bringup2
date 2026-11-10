#include "app_bringup.h"

#include "audio.h"
#include "stream_common.h"

#include "soc/soc_caps.h"

/* ---- the source ------------------------------------------------------------------------ */

/* What the receiver was when the stream began. The stream ends if it changes: reading
 * frames laid out for a different channel count would shift every channel. */
static struct {
    audio_rx_mode_t mode;
    int channels;
    uint8_t bits;
    uint32_t rate;
    uint32_t pdm_clk;
} snap;

static esp_err_t bus_read(void *ctx, void *buf, size_t len, size_t *got, uint32_t timeout_ms)
{
    (void)ctx;
    return audio_bus_read(buf, len, got, timeout_ms);
}

static bool bus_alive(void *ctx)
{
    (void)ctx;
    return audio_bus_rx_ready() && audio_bus_rx_mode() == snap.mode &&
           audio_bus_rx_channels() == snap.channels && audio_bus_rx_bits() == snap.bits &&
           audio_bus_rx_rate() == snap.rate && audio_bus_pdm_clk_hz() == snap.pdm_clk;
}

bool stream_source_describe(audio_stream_source_t *src)
{
    if (!audio_bus_rx_ready()) {
        return false;
    }
    memset(src, 0, sizeof *src);
    const typeof(snap) now = {
        .mode = audio_bus_rx_mode(),
        .channels = audio_bus_rx_channels(),
        .bits = audio_bus_rx_bits(),
        .rate = audio_bus_rx_rate(),
        .pdm_clk = audio_bus_pdm_clk_hz(),
    };
    /* The snapshot a running stream is judged against is the one taken when it started.
     * `status` and `budget` describe the microphone too, and must not move it: a
     * microphone reopened under a stream would then look unchanged. */
    if (!audio_stream_owner()) {
        snap = now;
    }

    const bool raw = (now.mode == AUDIO_RX_PDM_RAW);
    const size_t frame_bytes = audio_bus_rx_frame_bytes();
    src->channels = (uint8_t)now.channels;
    src->slot_bytes = now.channels ? (uint8_t)(frame_bytes / (size_t)now.channels) : 0;
    src->data_bits = now.bits;
    src->pdm_raw = raw;
    src->pdm_clk_hz = raw ? now.pdm_clk : 0;
    src->pdm_lsb_first = false;     /* the raw slot format audio_bus_open_pdm_raw() asks for */
    /* A raw source has no rate of its own until a ratio is chosen. */
    src->rate_hz = raw ? 0 : now.rate;
    src->read = bus_read;
    src->alive = bus_alive;
    return audio_stream_source_valid(src);
}

bool stream_source_get(audio_stream_source_t *src)
{
    if (!audio_bus_rx_ready()) {
        STRRES_ERROR(STR_AUDIO_STREAM_NO_RECEIVER);
        return false;
    }
    if (!stream_source_describe(src)) {
        STRRES_ERROR(STR_AUDIO_STREAM_NO_SOURCE);
        return false;
    }
    return true;
}

/* ---- refusals -------------------------------------------------------------------------- */

void stream_report(const audio_stream_why_t *why, const char *ch_text)
{
    switch (why->code) {
    case AUDIO_STREAM_ERR_NO_SOURCE:
        STRRES_ERROR(STR_AUDIO_STREAM_NO_SOURCE);
        break;
    case AUDIO_STREAM_ERR_BAD_RATE:
        if (why->raw_rates[0]) {
            STRRES_ERROR(STR_AUDIO_STREAM_RAW_RATE_OPTIONS, (unsigned)why->want_rate,
                         (unsigned)why->raw_rates[0], (unsigned)why->raw_rates[1],
                         (unsigned)why->raw_rates[2], (unsigned)why->raw_rates[3]);
        } else if (why->want_rate) {
            STRRES_ERROR(STR_AUDIO_STREAM_RATE_MISMATCH, (unsigned)why->src_rate,
                         (unsigned)why->want_rate);
        } else {
            STRRES_ERROR(STR_AUDIO_STREAM_RATE_INVALID);
        }
        break;
    case AUDIO_STREAM_ERR_BAD_CHANNEL:
        STRRES_ERROR(STR_AUDIO_STREAM_CH_RANGE, why->channel, why->channels_available);
        break;
    case AUDIO_STREAM_ERR_BAD_BITS:
        STRRES_ERROR(STR_AUDIO_STREAM_BITS_UNAVAILABLE, (int)why->src_bits);
        break;
    case AUDIO_STREAM_ERR_BAD_LIST:
        STRRES_ERROR(STR_AUDIO_STREAM_CH_INVALID, ch_text ? ch_text : "");
        break;
    case AUDIO_STREAM_ERR_DUP_CHANNEL:
        STRRES_ERROR(STR_AUDIO_STREAM_CH_DUPLICATE, why->channel);
        break;
    default:
        STRRES_ERROR(STR_AUDIO_STREAM_START_FAILED, esp_err_to_name(why->code));
        break;
    }
}

bool stream_report_start_error(esp_err_t err)
{
    if (err == AUDIO_STREAM_ERR_BUSY) {
        const char *owner = audio_stream_owner();
        if (owner) {
            STRRES_ERROR(STR_AUDIO_STREAM_BUSY, owner, owner);
        } else {
            STRRES_ERROR(STR_AUDIO_STREAM_BUSY_CLOSING);
        }
        return true;
    }
    if (err == ESP_ERR_NO_MEM) {
        STRRES_ERROR(STR_AUDIO_STREAM_NO_MEMORY);
        return true;
    }
    return false;
}

/* ---- options --------------------------------------------------------------------------- */

int stream_parse_options(int argc, char **argv, int first, audio_stream_want_t *want,
                         const char **ch_text, stream_option_fn extra, void *ctx)
{
    memset(want, 0, sizeof *want);
    *ch_text = NULL;
    for (int i = first; i < argc;) {
        if (strcasecmp(argv[i], "rate") == 0) {
            int hz;
            if (i + 1 >= argc || cli_parse_int_arg(argv[i + 1], &hz) < 0 || hz <= 0) {
                STRRES_ERROR(STR_AUDIO_STREAM_RATE_INVALID);
                return -1;
            }
            want->rate = (uint32_t)hz;
            i += 2;
        } else if (strcasecmp(argv[i], "bits") == 0) {
            int bits;
            if (i + 1 >= argc || cli_parse_int_arg(argv[i + 1], &bits) < 0 ||
                (bits != 16 && bits != 24)) {
                STRRES_ERROR(STR_AUDIO_STREAM_BITS_INVALID);
                return -1;
            }
            want->bits = (uint8_t)bits;
            i += 2;
        } else if (strcasecmp(argv[i], "ch") == 0) {
            if (i + 1 >= argc) {
                STRRES_ERROR(STR_AUDIO_STREAM_CH_INVALID, "");
                return -1;
            }
            *ch_text = argv[i + 1];
            i += 2;
        } else {
            int used = extra ? extra(argc, argv, i, ctx) : 0;
            if (used < 0) {
                return -1;
            }
            if (used == 0) {
                STRRES_ERROR(STR_AUDIO_STREAM_UNEXPECTED_ARGUMENT, argv[i]);
                return -1;
            }
            i += used;
        }
    }
    return 0;
}

bool stream_apply_channels(const audio_stream_source_t *src, const char *ch_text,
                           audio_stream_want_t *want)
{
    if (!ch_text) {
        return true;
    }
    int n = 0;
    audio_stream_why_t why;
    if (audio_stream_parse_channels(ch_text, src->channels, want->chan, &n, &why) != ESP_OK) {
        stream_report(&why, ch_text);
        return false;
    }
    want->nch = n;
    return true;
}

int stream_task_core(void)
{
#if SOC_CPU_CORES_NUM > 1
    return 1;
#else
    return -1;
#endif
}
