/*
 * The one thing app_bringup.h declares that needs code behind it.
 *
 * Almost every string in this firmware is printed and never held, which is what
 * STRRES_PRINTF() is for. The exception is a sentence that appears inside
 * another one as a %s -- the reason a pin cannot be driven, the name of a codec
 * input -- where the text has to exist as a value. strres_copy() is the API for
 * that; this only adds what every caller would otherwise repeat, which is
 * saying something rather than nothing when the lookup misses.
 */
#include "app_bringup.h"

const char *app_str(strres_id_t id, char *buf, size_t len)
{
    if (strres_copy(id, buf, len) < 0) {
        snprintf(buf, len, "[str:%04X]", (unsigned)id);
    }
    return buf;
}
