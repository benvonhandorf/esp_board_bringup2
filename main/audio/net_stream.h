#ifndef NET_STREAM_H
#define NET_STREAM_H

/*
 * The `audio-net` group: stream the open microphone over WiFi, as an endless WAV
 * response that `curl | ffplay`, VLC, sox or a browser plays as it arrives.
 *
 * The streaming is the audio_stream component and its audio_stream_http sink. This file
 * is the console half, the HTTP handler that applies the defaults `config` stored, and
 * every line of prose. The route itself is a row in app_http.c.
 *
 * There is no `start`: the listener's GET starts the stream, and the microphone is held
 * for as long as it stays connected.
 */

#include "esp_err.h"
#include "esp_http_server.h"

int cmd_audio_net_config(int argc, char **argv);
int cmd_audio_net_status(int argc, char **argv);
int cmd_audio_net_stop(int argc, char **argv);

/* GET /api/audio/stream[?rate=&bits=&ch=]. Query parameters override what `config` set. */
esp_err_t net_stream_handle(httpd_req_t *req);

#endif
