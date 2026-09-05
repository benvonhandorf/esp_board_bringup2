#ifndef CODEC_NAU8822_H
#define CODEC_NAU8822_H

#include "audio.h"

extern const audio_codec_t nau8822_codec;

int cmd_nau8822_init(int argc, char **argv);
int cmd_nau8822_status(int argc, char **argv);
int cmd_nau8822_reg(int argc, char **argv);
int cmd_nau8822_route(int argc, char **argv);
int cmd_nau8822_input(int argc, char **argv);
int cmd_nau8822_gain(int argc, char **argv);

#endif
