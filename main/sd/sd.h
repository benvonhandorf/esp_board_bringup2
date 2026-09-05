#ifndef SD_H
#define SD_H

#include <stdbool.h>

int cmd_sd_spi(int argc, char **argv);
int cmd_sd_mmc(int argc, char **argv);
int cmd_sd_info(int argc, char **argv);
int cmd_sd_bench(int argc, char **argv);
int cmd_sd_raw(int argc, char **argv);
int cmd_sd_sweep(int argc, char **argv);
int cmd_sd_results(int argc, char **argv);
int cmd_sd_close(int argc, char **argv);

/*
 * True while a card is up on the shared SPI host, so `spi bus` can refuse
 * rather than pull the bus out from under an initialized card. The mirror of
 * spi_group_owns_host() in spi.h.
 */
bool sd_owns_spi_host(void);

#endif
