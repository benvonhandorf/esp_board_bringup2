/* Captures what strres emits so a test can assert on the exact user-visible
 * text, the same way cli/test/stubs/diag_capture.c does. */
#include "diag.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static char buf[4096];
static size_t used;

void diag_capture_reset(void) { used = 0; buf[0] = '\0'; }
const char *diag_capture_text(void) { return buf; }

static void append(const char *fmt, va_list args)
{
    int n = vsnprintf(buf + used, sizeof(buf) - used, fmt, args);
    if (n > 0) {
        used += (size_t)n < sizeof(buf) - used ? (size_t)n : sizeof(buf) - used - 1;
    }
}

void diag_printf(const char *fmt, ...)
{
    va_list a; va_start(a, fmt); append(fmt, a); va_end(a);
}

void diag_vprintf(const char *fmt, va_list args)
{
    append(fmt, args);
}

void diag_error(const char *fmt, ...)
{
    va_list a; va_start(a, fmt);
    used += (size_t)snprintf(buf + used, sizeof(buf) - used, "ERR: ");
    append(fmt, a);
    va_end(a);
    used += (size_t)snprintf(buf + used, sizeof(buf) - used, "\n");
}
