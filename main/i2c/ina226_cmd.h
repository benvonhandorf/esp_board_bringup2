#ifndef INA226_CMD_H
#define INA226_CMD_H

/*
 * Console commands for the INA226.
 *
 * The device itself is the ina226 shared component, pinned in
 * main/idf_component.yml: it knows registers and returns facts. This file is
 * the other half: it parses arguments, tracks which parts the user has
 * configured, calls the driver, and turns those facts into prose.
 */

int cmd_ina226_config(int argc, char **argv);
int cmd_ina226_read(int argc, char **argv);
int cmd_ina226_list(int argc, char **argv);

#endif
