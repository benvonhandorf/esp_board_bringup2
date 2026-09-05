# net

[← Command reference](README.md) · [Project README](../README.md)

The link this device joins on its own, and the services that run over it.

Its counterpart is [`wifi`](wifi.md), which is about the radio in front of you —
ad-hoc joins, throughput, turning the transmitter off for a quiet measurement.
This group is about the *configured* link: the known networks in `config.json`,
the access point the device falls back to, and the reconnect logic, all of which
run without being told to.

That autonomy is the reason this group has no connect command of its own. `wifi_manager` joins the
best known network at boot, retries on a timer when it cannot, and raises the
configuration access point when nothing is joinable. Every service that needs
connectivity — mDNS, NTP, MQTT, the HTTP server and with it the web console —
subscribes to `NET_EVENT` and starts itself when a link appears. Nothing waits
for the network; see [Bring-up order](startup.md).

## `status`

The manager's state, the address it holds and the current RSSI; then whether MQTT
is connected, and the topic prefix it is publishing under.

For the radio's own view — SSID, BSSID, PHY mode, bandwidth, gateway and netmask
— use [`wifi status`](wifi.md), which reads the driver rather than the manager.

## `scan`

Asks the manager to scan and join the best known network it can see. Results
arrive asynchronously, so the command returns as soon as the scan is under way;
`net status` says where it got to.

This is not [`wifi scan`](wifi.md), which lists what is nearby and joins nothing.

## `ap`

Serves the configuration access point named by `ap_ssid` and `ap_password` in
`config.json` — the same one the device raises by itself when no known network is
reachable, for when you want it without waiting for that to be decided.

A station association is dropped first: one radio cannot sit on two channels.
