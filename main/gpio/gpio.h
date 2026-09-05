#ifndef GPIO_H
#define GPIO_H

#include <stdbool.h>

int cmd_gpio_set(int argc, char** argv);
int cmd_gpio_read(int argc, char** argv);
int cmd_gpio_aread(int argc, char** argv);
int cmd_gpio_blink(int argc, char** argv);
int cmd_gpio_short(int argc, char** argv);
int cmd_gpio_rc(int argc, char** argv);
int cmd_gpio_survey(int argc, char** argv);

/*
 * True if a pin may be driven as an output. Shared with modules outside this
 * one (see main/loadcell/hx711_cmd.c) because the interesting part is not the
 * input-only check -- it is that this refuses the pins carrying the console,
 * and driving one of those ends the session that typed the command.
 *
 * On false, *why is set to a short reason suitable for "Skipping GPIO %d: %s".
 */
bool app_pin_is_drivable(int pin, const char **why);

#endif
