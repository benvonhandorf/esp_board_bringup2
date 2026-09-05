#ifndef INA237_CMD_H
#define INA237_CMD_H

/*
 * Console commands for the INA237.
 *
 * The device itself is the ina237 shared component, pinned in
 * main/idf_component.yml; which knows registers and
 * returns facts. This file is the other half: it parses arguments, tracks which
 * parts the user has configured, calls the driver, and turns those facts back
 * into the prose documented in docs/i2c.md. Nothing here touches a register.
 */

int cmd_ina237_config(int argc, char **argv);
int cmd_ina237_read(int argc, char **argv);
int cmd_ina237_list(int argc, char **argv);

#endif
