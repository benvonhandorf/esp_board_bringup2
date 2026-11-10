#ifndef USB_STREAM_H
#define USB_STREAM_H

/*
 * The `audio-usb` group: stream the open microphone to a USB host as a USB Audio
 * Class 2 device.
 *
 * The streaming is the audio_stream component and its audio_stream_usb sink; this file
 * is the console half -- arguments, the warning that the serial console is about to end,
 * the reset timer, and every line of prose. The open microphone as a source, and the
 * words for a refusal, are shared with `audio-net` in stream_common.c.
 *
 * Starting the device takes the USB PHY, which on a chip whose console is
 * USB-Serial-JTAG ends the serial console until reset. See docs/audio.md.
 */

int cmd_audio_usb_config(int argc, char **argv);
int cmd_audio_usb_start(int argc, char **argv);
int cmd_audio_usb_status(int argc, char **argv);
int cmd_audio_usb_stop(int argc, char **argv);
int cmd_audio_usb_budget(int argc, char **argv);

#endif
