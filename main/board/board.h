#ifndef BOARD_H
#define BOARD_H

/*
 * Named board pinouts.
 *
 * Every command in this project takes explicit pin numbers, which is right for
 * a tool whose job is to find out how a board is wired. Once a board *has*
 * been worked out, though, retyping its pinout is only a way to make mistakes.
 * A board entry records the answer, and a preset replays the commands that use
 * it.
 *
 * Each board is its own group and each subsystem it supports is a command in
 * that group, so `board-cardputer` lists what is available and changes nothing,
 * while `board-cardputer audio` runs the setup and echoes every line it runs.
 * Adding a board is an entry in board.c and a group in app_console.c.
 */

int cmd_board_list(int argc, char **argv);

int cmd_board_cardputer_pins(int argc, char **argv);
int cmd_board_cardputer_audio(int argc, char **argv);
int cmd_board_cardputer_mic(int argc, char **argv);
int cmd_board_cardputer_sd(int argc, char **argv);

int cmd_board_xiao_pins(int argc, char **argv);
int cmd_board_xiao_sd(int argc, char **argv);
int cmd_board_xiao_mic(int argc, char **argv);

int cmd_board_sensor_pins(int argc, char **argv);
int cmd_board_sensor_i2c(int argc, char **argv);

int cmd_board_minstro_pins(int argc, char **argv);
int cmd_board_minstro_audio(int argc, char **argv);
int cmd_board_minstro_i2c(int argc, char **argv);
int cmd_board_minstro_sd(int argc, char **argv);

int cmd_board_core_basic_pins(int argc, char **argv);
int cmd_board_core_basic_audio(int argc, char **argv);
int cmd_board_core_basic_mic(int argc, char **argv);
int cmd_board_core_basic_sd(int argc, char **argv);
int cmd_board_core_basic_i2c(int argc, char **argv);

#endif
