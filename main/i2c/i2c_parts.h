#ifndef I2C_PARTS_H
#define I2C_PARTS_H

#include <stdint.h>

/*
 * What might be sitting at an I2C address.
 *
 * A scan reports numbers; this turns a number back into the parts this
 * firmware can drive there, and into the command that would drive them. It is
 * a lookup in a static table -- a suggestion, never an identification. Several
 * parts share 0x40-0x4f and nothing about an address distinguishes them; the
 * driver's own ID check is what settles it, which is why every candidate is
 * offered rather than one being picked.
 *
 * Only parts with a driver here are listed. An address no group claims says so
 * rather than guessing at a part this firmware could not drive anyway.
 */

/* The column header the row printers below line up under. */
void i2c_parts_print_header(void);

/* One row per part that claims `address`, tightest address range first, or a
 * single "no driver" row when none does. */
void i2c_parts_print_address(uint8_t address);

/* Every part in the catalogue, in address order, with ranges rather than one
 * concrete address. */
void i2c_parts_print_all(void);

#endif /* I2C_PARTS_H */
