#ifndef CODEC_SPH0645_H
#define CODEC_SPH0645_H

#include "audio.h"

/*
 * Knowles SPH0645LM4H-B I2S MEMS microphone.
 *
 * Like the NS4168 it has no control bus, and like the NS4168 it earns a driver
 * anyway: the part has hard constraints that are invisible from the command
 * line and produce silence or nonsense rather than an error when broken. See
 * codec_sph0645.c for what they are and where they come from.
 */
extern const audio_codec_t sph0645_codec;

int cmd_sph0645_init(int argc, char **argv);
int cmd_sph0645_status(int argc, char **argv);

#endif
