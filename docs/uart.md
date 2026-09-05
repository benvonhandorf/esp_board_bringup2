# UART

[← Command reference](README.md) · [Project README](../README.md)

This is an auxiliary UART, always separate from the console port, so
initializing it can never take over the shell you are typing into.

## `init <tx> <rx> <baud>`

Initializes a UART interface with the specified TX pin, RX pin, and baud rate.
Re-running it tears the old configuration down first, so pins and baud rate can
be changed freely.

## `send <data>`

Sends the specified data string over the UART interface. Quote the argument to
send spaces (`send "hello world"`). The escapes `\n`, `\r`, `\t`, `\0`, `\\` and
`\xNN` are expanded, so devices expecting CR terminators can be driven directly.

## `receive`

Receives and displays data from the UART interface, as a hex dump with an ASCII
column.
