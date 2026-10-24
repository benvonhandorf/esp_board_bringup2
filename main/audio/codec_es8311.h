#ifndef CODEC_ES8311_H
#define CODEC_ES8311_H

#include "audio.h"

extern const audio_codec_t es8311_codec;

int cmd_es8311_init(int argc, char **argv);
int cmd_es8311_status(int argc, char **argv);
int cmd_es8311_reg(int argc, char **argv);
int cmd_es8311_gain(int argc, char **argv);

#endif
