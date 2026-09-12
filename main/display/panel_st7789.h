#ifndef PANEL_ST7789_H
#define PANEL_ST7789_H

#include "display.h"

/*
 * ST7789 SPI TFT controller. The V2 silicon (M5Stack Cardputer) is the same
 * controller under a different marketing name -- what differs board to board
 * is glass size, gap, inversion and colour order, which is exactly what
 * `display bars`, `display edges` and `display gap` exist to measure.
 */
extern const display_panel_t st7789_panel;

int cmd_st7789_init(int argc, char **argv);

#endif
