# wifi

[← Command reference](README.md) · [Project README](../README.md)

The radio itself. Its counterpart is [`net`](net.md), which reports and steers
the link this device joins on its own — the known networks in `config.json`, the
access point it falls back to, and the reconnect logic. The division is by who
decides: `net` is about the configured link, this group is about the radio in
front of you.

Nothing here owns the driver. `wifi_manager` does, and reaching around it would
leave its state machine describing a radio that is no longer in that state — so
these commands ask it to change state, or read what the driver reports. That is
also why `wifi scan` calls `wifi_manager_scan()` rather than
`esp_wifi_scan_start()`: the manager's own `SCAN_DONE` handler consumes the
records first, and a scan started outside it comes back empty every time.

The radio is either a station or an access point, never both: a single radio
cannot sit on two channels, and an AP+station setup would silently drag the AP
onto whatever channel the station associated on. `connect` therefore stops a
running AP and `ap` drops a station association, each saying so as it happens.

## `scan`

Lists nearby access points with SSID, RSSI, channel, security and BSSID.

Scanning is a station-mode operation, so it is refused while an access point is
running; stop it with `wifi ap stop` first.

## `connect <SSID> [password]`

Joins an access point that is not in the configuration. Omit the password for an
open network. A running access point is stopped first.

The network is handed to `wifi_manager` and it does the joining, rather than this
command calling `esp_wifi_connect()` itself. Two things follow, and both are
wanted: the credentials land in the reconnect logic, so a link that drops comes
back on its own; and the manager's state stays true, which is what `net status`,
the published status message and every `NET_EVENT` subscriber read.

**The network is added in memory only.** `config.json` is the authored
configuration and a console command has no business rewriting it; `POST
/api/config` is how a network becomes permanent. So a `connect` does not survive
a reboot — which is the right default for a rig that is carried from board to
board.

The command waits up to 20 s for the link and reports the address it got. On a
timeout it says so and names the state the manager reached; the manager keeps
retrying regardless, so `wifi status` is the follow-up.

## `ap [stop]`

Raises the configured access point, so a laptop or phone can join the board
directly and reach the web console with no network in between — the point on a
bench with no infrastructure, or a board that cannot join anything.

**It takes no SSID.** The access point's identity is configuration: `ap_ssid`,
`ap_password` and `ap_channel` in `config.json`. A console command that could
change them would be a second place the device's identity is decided.

There is no `autostart`. `wifi_manager` does that at boot without being asked —
it joins the best known network, and raises this access point when nothing is
joinable — which is why the old command has no counterpart here.

`ap stop` returns to station mode and starts a scan.

## `status`

Reports the current association — SSID, BSSID, RSSI, channel, security, the
negotiated PHY mode (e.g. `802.11b/g/n`) and channel bandwidth (`20 MHz` or
`40 MHz`), and the configured TX power ceiling — plus the IP address, gateway
and netmask.

While an access point is running it reports that instead: the SSID, channel,
security and address being advertised, followed by each connected client with
its MAC address, DHCP-leased IP and signal strength.

## `off` and `on`

`off` powers the radio down: it stops the WiFi driver, and with it the PHY.
This is a real power-down rather than a disconnect — a station that has merely
disassociated still scans and still transmits, so a measurement taken against
one proves nothing.

The point is measurement, not power saving. On a board that shares a supply
between the radio and an analog front end, the radio is a suspect whenever a
reading is noisier than the part's datasheet says it should be, and the only
way to convict or clear it is to take it away and look again:

```
i2c-nau7802 read 50         # with the radio up
wifi off
i2c-nau7802 read 50         # with it genuinely silent
wifi on
```

The radio stays off until `on`. Commands that need it — `scan`, `connect`,
`ap`, `iperf` — refuse while it is off and name `wifi on` rather
than restarting it themselves, because silently powering the radio back up
would spoil the measurement the `off` was taken for. `status` reports the off
state instead of refusing.

**`off` takes the web console down with it**, since the address it is bound to
goes away. A serial session is the only way back in, which is worth knowing
before running it over the web console.

`on` powers the radio up in station mode and starts a scan, so the manager
rejoins a known network or falls back to the access point exactly as it does at
boot.

## `iperf <server>[:<port>]`

Runs an iperf2 TCP test against the specified server, defaulting to port 5001.
Before starting the transfer it prints a `Link:` line with the current RSSI,
PHY mode and bandwidth, so a slow run can be checked against the link it ran
over. Reports back throughput numbers. Optionally, the user may specify `continuous` which will cause the test to run continually, reporting results every 5 seconds.

A continuous run is ended with `wifi iperf stop`. Because commands are executed
one at a time, the test runs in the background and the prompt stays usable while
it reports.

## `netstats`

Reports lwIP's per-layer packet counters — received count, dropped count and
a summed error count (checksum, length, out-of-memory, routing, protocol and
option errors combined into one number) — for the link, IP, TCP and UDP
layers. Counts are cumulative since boot, not since the last command, so read
`netstats` before and after an `iperf` run and compare: a rising `drop` or
`err` count during the run points at packet loss or buffer pressure in the
network stack rather than the radio link (compare against `wifi status`'s
RSSI/PHY/bandwidth, which cover the radio side).
