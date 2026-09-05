#ifndef SYS_HW_H
#define SYS_HW_H

#include "esp_chip_info.h"

/*
 * The parts of `sys` that are about the silicon rather than the firmware.
 *
 * The rest of the group -- status, restart, confirm, rollback -- is in
 * app_console.c, because it is about the application and the OTA image.
 */

/* Chip, features, flash, memory, MAC and reset reason. Called by `sys info`,
 * which adds the firmware and configuration lines around it. */
void sys_hw_print_chip_info(void);

/* Bring up the 32.768 kHz crystal and run the RTC slow clock from it. */
int cmd_sys_lfxtal(int argc, char **argv);

/*
 * Human-readable chip name, e.g. "ESP32-S3". Shared with the SD module, which
 * stamps it into the benchmark results file so a saved result can be tied back
 * to the board it came from.
 */
const char *app_chip_model_name(esp_chip_model_t model);

#endif /* SYS_HW_H */
