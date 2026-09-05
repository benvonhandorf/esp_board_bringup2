#ifndef I2C_H
#define I2C_H

#include <stdbool.h>

#include "driver/i2c_master.h"

int cmd_i2c_bus(int argc, char** argv);
int cmd_i2c_scan(int argc, char** argv);
int cmd_i2c_read(int argc, char** argv);

/*
 * Bus access shared with device-specific drivers (see ina237_cmd.c).
 *
 * The bus handle and the per-address device-handle cache live in i2c.c, so
 * drivers built on top of the bus reuse them rather than keeping their own.
 */

/* True if 'i2c bus' has been run. Reports the usual error to the user if not. */
bool i2c_require_bus(void);

/* Fetch (creating and caching on first use) a device handle for an address. */
esp_err_t i2c_device_handle(uint8_t address, i2c_master_dev_handle_t *out);

#endif
