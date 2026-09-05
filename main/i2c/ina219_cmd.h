#ifndef INA219_CMD_H
#define INA219_CMD_H

/*
 * Console commands for the INA219.
 *
 * The device itself is the ina219 shared component, pinned in
 * main/idf_component.yml: it knows registers and returns facts. This file is
 * the other half: it parses arguments, tracks which parts the user has
 * configured, calls the driver, and turns those facts into prose.
 *
 * The INA219 sits beside the INA237 and INA226 rather than replacing either.
 * All three measure current across a shunt; which one is fitted is a fact about
 * the board, and a bring-up rig has to be able to ask.
 */

int cmd_ina219_config(int argc, char **argv);
int cmd_ina219_read(int argc, char **argv);
int cmd_ina219_list(int argc, char **argv);

#endif
