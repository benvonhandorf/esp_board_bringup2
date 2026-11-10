#ifndef STREAM_COMMON_H
#define STREAM_COMMON_H

/*
 * What `audio-usb` and `audio-net` share: the open microphone as an audio_stream source,
 * the options both groups parse, and the words for a refusal.
 *
 * The streaming itself is the audio_stream component and its sinks; this is the part that
 * is specific to this project -- the I2S receiver behind audio_bus_*, and the prose.
 */

#include <stdbool.h>

#include "audio_stream.h"
#include "esp_err.h"

/* The open microphone, without saying anything. False when none is open or it is not
 * something that can be streamed. Fills a snapshot too: the stream ends (SOURCE_GONE)
 * if the receiver is closed or reopened as another format while it runs. */
bool stream_source_describe(audio_stream_source_t *src);

/* As stream_source_describe(), but prints the reason when it fails. */
bool stream_source_get(audio_stream_source_t *src);

/* Prints a refusal from audio_stream_resolve(), audio_stream_parse_channels() or
 * audio_stream_http_parse_query(). `ch_text` is the channel list the user typed, for the
 * message about a malformed one; NULL if there was none. */
void stream_report(const audio_stream_why_t *why, const char *ch_text);

/* Prints why audio_stream_start() refused, for the two reasons that are the same whichever
 * sink asked: another stream holds the microphone, or there is no memory. False for
 * anything else, which the caller words for its own sink. */
bool stream_report_start_error(esp_err_t err);

/*
 * Parses the options both groups take -- rate <hz>, ch <list>, bits <16|24> -- from
 * argv[first] on into `want` (zeroed first). `ch` is only noted in *ch_text, because
 * checking it needs the open microphone: pass it to stream_apply_channels() once there is
 * one. A word that is none of those goes to `extra` (may be NULL), which returns how many
 * tokens it used, 0 if it does not know the word, or -1 after reporting an error.
 * Reports its own errors; returns 0 or -1.
 */
typedef int (*stream_option_fn)(int argc, char **argv, int i, void *ctx);
int stream_parse_options(int argc, char **argv, int first, audio_stream_want_t *want,
                         const char **ch_text, stream_option_fn extra, void *ctx);

/* Checks `ch_text` against the microphone and stores it in `want`. True when there was
 * none, or it was good; otherwise reports and returns false. */
bool stream_apply_channels(const audio_stream_source_t *src, const char *ch_text,
                           audio_stream_want_t *want);

/* The core to run the stream task on: the one the radio and the USB stack are not on,
 * where the chip has two. -1 for any. */
int stream_task_core(void);

#endif
