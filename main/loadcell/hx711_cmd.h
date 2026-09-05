#ifndef HX711_CMD_H
#define HX711_CMD_H

/*
 * Console commands for the HX711.
 *
 * The device itself is the hx711 shared component, pinned in
 * main/idf_component.yml; which knows the pulse counts and
 * the timing and returns facts. This file is the other half: it parses
 * arguments, calls the driver, and turns those facts back into the prose
 * documented in docs/loadcell.md. Nothing here touches a pin.
 */

int cmd_hx711_init(int argc, char **argv);
int cmd_hx711_gain(int argc, char **argv);
int cmd_hx711_input(int argc, char **argv);
int cmd_hx711_power(int argc, char **argv);
int cmd_hx711_raw(int argc, char **argv);
int cmd_hx711_read(int argc, char **argv);
int cmd_hx711_tare(int argc, char **argv);
int cmd_hx711_calibrate(int argc, char **argv);
int cmd_hx711_scale(int argc, char **argv);
int cmd_hx711_weight(int argc, char **argv);
int cmd_hx711_status(int argc, char **argv);
int cmd_hx711_close(int argc, char **argv);

#endif
