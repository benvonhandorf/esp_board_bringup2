#ifndef DIAG_H_STUB
#define DIAG_H_STUB
#include <stdarg.h>
#include <stddef.h>
void diag_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void diag_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void diag_vprintf(const char *fmt, va_list args);

/* Test-only helpers, not part of the real diag interface. */
void diag_capture_reset(void);
const char *diag_capture_text(void);
#endif
