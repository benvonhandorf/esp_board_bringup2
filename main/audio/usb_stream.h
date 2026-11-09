#ifndef USB_STREAM_H
#define USB_STREAM_H

/*
 * The `audio-usb` group: stream the open microphone to a USB host as a USB Audio
 * Class 2 device, through the usb_mic component.
 *
 * Two halves, kept apart so another sink can reuse the first. The stream core reads
 * the open receiver (audio_bus_read()), picks channels and width, and produces
 * interleaved little-endian PCM. The sink is usb_mic_write(); a network sink would
 * replace only that call.
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
