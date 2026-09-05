#ifndef RX8130CE_CMD_H
#define RX8130CE_CMD_H

/*
 * Console commands for the RX8130CE real-time clock.
 *
 * The device itself is the rx8130ce shared component, pinned in
 * main/idf_component.yml: it knows registers and returns facts. This file
 * parses arguments, keeps one handle, and turns those facts into prose.
 */

int cmd_rx8130ce_time(int argc, char **argv);
int cmd_rx8130ce_set(int argc, char **argv);

#endif
