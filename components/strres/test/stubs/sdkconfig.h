/* Host-test stub for the Kconfig values strres.c reads.
 *
 * The cache is deliberately tiny so eviction and the all-slots-held refusal are
 * reachable with a handful of fixture sections rather than sixteen. */
#ifndef SDKCONFIG_H_STUB
#define SDKCONFIG_H_STUB
#define CONFIG_STRRES_CACHE_SECTIONS  2
#define CONFIG_STRRES_FORMAT_BUF_SIZE 256
#define CONFIG_STRRES_BASE_DIR        "fixtures-build"
#define CONFIG_STRRES_DEFAULT_LOCALE  "en-US"
#endif
