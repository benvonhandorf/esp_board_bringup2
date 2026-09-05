# SPI

[← Command reference](README.md) · [Project README](../README.md)

## `bus <clk> <mosi> <miso> [cs]`

Initializes an SPI bus with the specified clock, MOSI, and MISO pins.

The optional chip-select pin is an addition to the original specification:
without one the driver cannot select a peripheral, so reads return nothing but
bus noise. If it is omitted you must drive chip select yourself with `gpio set`.

## `read <addr> <len>`

Reads the specified number of bytes from the given address on the SPI bus.
The address is transmitted as the first byte of a full-duplex transaction, and
the bytes clocked back during it are reported.

## `write <addr> <data> [data...]`

Writes data to the specified address on the SPI bus. Data bytes may be given in
decimal or as `0x` hex.

## `free`

Releases the SPI host and the pins, without a reset. The SD menu competes for
the same host — on chips with only one general purpose SPI host, such as the
ESP32-C3, there would otherwise be no way to move from `spi bus` to `sd spi`
within a session.
