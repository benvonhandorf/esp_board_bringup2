#ifndef CODEC_NS4168_H
#define CODEC_NS4168_H

#include "audio.h"

extern const audio_codec_t ns4168_codec;

int cmd_ns4168_init(int argc, char **argv);
int cmd_ns4168_status(int argc, char **argv);

#endif
