#ifndef NAU7802_CMD_H
#define NAU7802_CMD_H

/*
 * Console commands for the NAU7802.
 *
 * The device itself is the nau7802 shared component, pinned in
 * main/idf_component.yml: it knows registers and
 * returns facts. This file is the other half: it parses arguments, calls the
 * driver, and turns those facts back into the prose documented in
 * docs/i2c.md. Nothing here touches a register.
 */

int cmd_nau7802_init(int argc, char **argv);
int cmd_nau7802_gain(int argc, char **argv);
int cmd_nau7802_rate(int argc, char **argv);
int cmd_nau7802_read(int argc, char **argv);
int cmd_nau7802_tare(int argc, char **argv);
int cmd_nau7802_calibrate(int argc, char **argv);
int cmd_nau7802_scale(int argc, char **argv);
int cmd_nau7802_weight(int argc, char **argv);
int cmd_nau7802_status(int argc, char **argv);
int cmd_nau7802_input(int argc, char **argv);
int cmd_nau7802_drdy(int argc, char **argv);
int cmd_nau7802_ldomode(int argc, char **argv);
int cmd_nau7802_pgacap(int argc, char **argv);
int cmd_nau7802_raw(int argc, char **argv);
int cmd_nau7802_registers(int argc, char **argv);

#endif
