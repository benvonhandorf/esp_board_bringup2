#ifndef WIFI_H
#define WIFI_H

/*
 * WiFi commands for bringing a board up, as opposed to the `net` group, which
 * reports and steers the link this device joins on its own.
 *
 * The division is by who decides. `net` is about the configured link:
 * wifi_manager works from config.json, joins the best known network, falls back
 * to its own access point and reconnects, all without being told to. This group
 * is about the radio in front of you -- join something that is not in the
 * configuration, host an AP with a chosen SSID, take the radio away entirely to
 * see whether it was the source of the noise in an analog reading, and measure
 * what the link can actually carry.
 *
 * None of it owns the driver. wifi_manager does, and reaching around it to call
 * esp_wifi_stop() or esp_wifi_connect() directly would leave its state machine
 * describing a radio that is no longer in that state. Everything here either
 * asks wifi_manager to do something, or reads what the driver reports.
 */

int cmd_wifi_scan(int argc, char **argv);
int cmd_wifi_connect(int argc, char **argv);
int cmd_wifi_ap(int argc, char **argv);
int cmd_wifi_status(int argc, char **argv);
int cmd_wifi_off(int argc, char **argv);
int cmd_wifi_on(int argc, char **argv);
int cmd_wifi_iperf(int argc, char **argv);
int cmd_wifi_netstats(int argc, char **argv);

#endif /* WIFI_H */
