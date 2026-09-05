#ifndef SHT4X_CMD_H
#define SHT4X_CMD_H

/*
 * Console commands for the SHT4x.
 *
 * The device itself is the sht4x shared component, pinned in
 * main/idf_component.yml; which knows the protocol and
 * returns facts. This file is the other half: it parses arguments, calls the
 * driver, and turns those facts back into the prose documented in docs/i2c.md.
 * Nothing here touches the wire.
 */

int cmd_sht4x_read(int argc, char **argv);
int cmd_sht4x_serial(int argc, char **argv);
int cmd_sht4x_heater(int argc, char **argv);
int cmd_sht4x_reset(int argc, char **argv);

#endif
