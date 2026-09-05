#ifndef AW9523B_CMD_H
#define AW9523B_CMD_H

/*
 * Console commands for the AW9523B 16-bit I/O expander.
 *
 * The device itself is the aw9523b shared component, pinned in
 * main/idf_component.yml: it knows registers and returns facts. This file
 * parses arguments, keeps one handle, and turns those facts into prose.
 */

int cmd_aw9523b_init(int argc, char **argv);
int cmd_aw9523b_read(int argc, char **argv);
int cmd_aw9523b_write(int argc, char **argv);
int cmd_aw9523b_set(int argc, char **argv);

#endif
