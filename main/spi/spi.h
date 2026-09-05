#ifndef SPI_H
#define SPI_H

#include <stdbool.h>

#include "driver/spi_master.h"

/*
 * The one SPI host this project uses. It is shared with the SD card module, and
 * on chips such as the ESP32-C3 it is the only general purpose host there is,
 * so the two claim it exclusively and refuse rather than fight over it.
 */
#define BP_SPI_HOST_ID SPI2_HOST

int cmd_spi_bus(int argc, char** argv);
int cmd_spi_read(int argc, char** argv);
int cmd_spi_write(int argc, char** argv);
int cmd_spi_free(int argc, char** argv);

/* True while the spi group holds BP_SPI_HOST_ID. The mirror of
 * sd_owns_spi_host(). */
bool spi_group_owns_host(void);

#endif
