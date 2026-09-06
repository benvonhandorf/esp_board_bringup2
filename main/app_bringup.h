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
 *
 * Prose comes from strres.h rather than from a literal: STRRES_PRINTF() and
 * STRRES_ERROR() name a string in the catalogue on the `res` partition, so the
 * words are not in the image. The generated ids are in strres_ids.h. Short
 * layout fragments -- a column separator, a lone "\n" -- stay literals, because
 * a two-byte id plus a lookup costs more than the four bytes it would save.
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
#include "strres.h"
#include "strres_ids.h"

/*
 * Resolve a string into a caller-owned buffer, for the few places a sentence
 * has to be a %s inside another one -- why a pin cannot be driven, which input
 * a codec is on. Returns `buf`, which holds the id in [str:XXXX] form if the
 * lookup missed, so the line is never silently blank. Prefer STRRES_PRINTF():
 * this exists for text that must be a value, not merely printed.
 */
const char *app_str(strres_id_t id, char *buf, size_t len);

/* Long enough for the reasons and names this is used for. */
#define APP_STR_LEN 64

#endif /* APP_BRINGUP_H */
