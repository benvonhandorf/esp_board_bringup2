#ifndef PI4IOE5V6408_CMD_H
#define PI4IOE5V6408_CMD_H

/*
 * Console commands for the PI4IOE5V6408 8-bit I/O expander.
 *
 * The device itself is the pi4ioe5v6408 shared component, pinned in
 * main/idf_component.yml: it knows registers and returns facts. This file
 * parses arguments, keeps one handle, and turns those facts into prose.
 */

int cmd_pi4ioe_init(int argc, char **argv);
int cmd_pi4ioe_read(int argc, char **argv);
int cmd_pi4ioe_write(int argc, char **argv);
int cmd_pi4ioe_set(int argc, char **argv);
int cmd_pi4ioe_interrupt(int argc, char **argv);

#endif
