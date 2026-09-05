#ifndef APP_BRINGUP_H
#define APP_BRINGUP_H

/*
 * The handful of includes every command module in this project wants.
 *
 * It exists so a module's own includes are the ones that say something about
 * the module. Argument parsing lives in cli.h (cli_parse_pin_list() and
 * friends) and output in diag.h (diag_printf(), diag_error()); both are shared
 * components, and this header is only the convenience of pulling them in
 * together with the FreeRTOS and error-handling names that no command module
 * gets far without.
 *
 * Never printf(). diag_printf() reaches the serial port and every attached
 * browser; printf() reaches only the serial port, silently.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"

#include "cli.h"
#include "diag.h"

#endif /* APP_BRINGUP_H */
