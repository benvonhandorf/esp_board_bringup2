#ifndef LM75BDP_CMD_H
#define LM75BDP_CMD_H

/*
 * Console commands for the LM75B temperature sensor and thermal watchdog.
 *
 * The device itself is the lm75bdp shared component, pinned in
 * main/idf_component.yml: it knows registers and returns facts. This file
 * parses arguments, keeps one handle per address, and turns those facts into
 * prose.
 */

int cmd_lm75bdp_read(int argc, char **argv);
int cmd_lm75bdp_limits(int argc, char **argv);

#endif
