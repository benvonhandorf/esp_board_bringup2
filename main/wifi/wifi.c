/*
 * The bring-up half of WiFi: see wifi.h for why this is separate from `net`.
 *
 * wifi_manager owns the driver, the netifs and the event handlers. This file
 * owns none of that -- it asks wifi_manager to change state and reads what the
 * driver reports. The old version of this project did own it all, and could
 * not have coexisted with a manager doing the same job.
 */
#include "app_bringup.h"
#include "wifi.h"

#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_ap_get_sta_list.h"
#include "wifi_manager.h"

#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/stats.h"

#include "iperf.h"

#define MAX_SCAN_RESULTS 32

/* How long to wait for wifi_manager to reach CONNECTED after being asked to
 * join. Long enough for a slow AP plus DHCP, short enough not to wedge the
 * console. */
#define CONNECT_TIMEOUT_MS 20000
#define CONNECT_POLL_MS    250

/* docs/wifi.md: continuous mode reports every 5 seconds until interrupted. */
#define IPERF_CONTINUOUS_INTERVAL_S 5
#define IPERF_SINGLE_DURATION_S 10
/* iperf's own time field is uint32; this stands in for "keep going". */
#define IPERF_CONTINUOUS_DURATION_S 86400

static iperf_id_t running_iperf = -1;

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

static const char *auth_mode_name(wifi_auth_mode_t mode)
{
    switch (mode) {
    case WIFI_AUTH_OPEN:            return "open";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENT";
    case WIFI_AUTH_WAPI_PSK:        return "WAPI";
    case WIFI_AUTH_OWE:             return "OWE";
    default:                        return "unknown";
    }
}

/* Renders the negotiated PHY mode from a wifi_ap_record_t's bitfields, so a
 * link stuck on an older/narrower mode than the AP supports is visible at a
 * glance -- useful when throughput is lower than expected and RF is one of
 * the suspects. */
static const char *phy_mode_name(const wifi_ap_record_t *ap)
{
    static char name[32];
    char *p = name;
    memcpy(p, "802.11", 6);
    p += 6;
    bool first = true;

    const struct { bool set; const char *tag; } modes[] = {
        {ap->phy_11b,  "b"},
        {ap->phy_11g,  "g"},
        {ap->phy_11n,  "n"},
        {ap->phy_11ax, "ax"},
    };

    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        if (!modes[i].set) {
            continue;
        }
        if (!first) {
            *p++ = '/';
        }
        size_t len = strlen(modes[i].tag);
        memcpy(p, modes[i].tag, len);
        p += len;
        first = false;
    }

    if (first) {
        return "unknown";
    }

    if (ap->phy_lr) {
        memcpy(p, "+LR", 3);
        p += 3;
    }

    *p = '\0';
    return name;
}

static const char *bandwidth_name(wifi_bandwidth_t bw)
{
    switch (bw) {
    case WIFI_BW20: return "20 MHz";
    case WIFI_BW40: return "40 MHz";
    default:        return "unknown";
    }
}

static const char *state_name(wifi_manager_state_t state)
{
    switch (state) {
    case WIFI_MANAGER_STA_DISCONNECTED: return "disconnected";
    case WIFI_MANAGER_STA_CONNECTING:   return "connecting";
    case WIFI_MANAGER_STA_CONNECTED:    return "connected";
    case WIFI_MANAGER_AP_MODE:          return "access point";
    case WIFI_MANAGER_POWERED_OFF:      return "powered off";
    default:                            return "unknown";
    }
}

/*
 * Refuse while the radio is off rather than switching it back on.
 *
 * `wifi off` exists to take the transmitter out of a measurement; a command
 * that silently restored it would undo the thing being measured and the
 * reading would look fine. Every such command names `wifi on` instead.
 */
static bool require_radio_on(void)
{
    if (wifi_manager_get_state() == WIFI_MANAGER_POWERED_OFF) {
        STRRES_ERROR(STR_WIFI_RADIO_OFF);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* scan                                                                */
/* ------------------------------------------------------------------ */

int cmd_wifi_scan(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!require_radio_on()) {
        return -1;
    }

    /* The radio can only scan as a station, and AP mode is exclusive -- one
     * radio cannot sit on two channels. Say so rather than surfacing a bare
     * ESP_ERR_WIFI_MODE. */
    if (wifi_manager_get_state() == WIFI_MANAGER_AP_MODE) {
        STRRES_ERROR(STR_WIFI_SCAN_NEEDS_STATION);
        return -1;
    }

    wifi_ap_record_t *records = calloc(MAX_SCAN_RESULTS, sizeof(wifi_ap_record_t));
    if (!records) {
        STRRES_ERROR(STR_WIFI_OUT_OF_MEMORY);
        return -1;
    }

    STRRES_PRINTF(STR_WIFI_SCANNING);

    /*
     * wifi_manager_scan(), not esp_wifi_scan_start(). wifi_manager's SCAN_DONE
     * handler consumes or clears the records before a blocking scan started
     * here would return, so this command used to report no access points while
     * the manager's own log listed the ones it had just found. Scanning belongs
     * to whoever owns the event handler.
     */
    uint16_t count = 0;
    esp_err_t err = wifi_manager_scan(records, MAX_SCAN_RESULTS, &count);
    if (err != ESP_OK) {
        STRRES_ERROR(STR_WIFI_SCAN_FAILED, esp_err_to_name(err));
        free(records);
        return -1;
    }

    if (count == 0) {
        STRRES_PRINTF(STR_WIFI_NO_APS);
        free(records);
        return 0;
    }

    STRRES_PRINTF(STR_WIFI_SCAN_HEADER, "SSID", "RSSI", "CH", "SECURITY", "BSSID");
    for (uint16_t i = 0; i < count; i++) {
        const wifi_ap_record_t *ap = &records[i];
        STRRES_PRINTF(STR_WIFI_SCAN_ROW,
                      (const char *)ap->ssid, ap->rssi, ap->primary,
                      auth_mode_name(ap->authmode), ap->bssid[0], ap->bssid[1], ap->bssid[2],
                      ap->bssid[3], ap->bssid[4], ap->bssid[5]);
    }
    STRRES_PRINTF(STR_WIFI_SCAN_COUNT, count, count == 1 ? "" : "s");

    free(records);
    return 0;
}

/* ------------------------------------------------------------------ */
/* connect                                                             */
/* ------------------------------------------------------------------ */

int cmd_wifi_connect(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        STRRES_PRINTF(STR_WIFI_USAGE_CONNECT);
        return -1;
    }

    if (!require_radio_on()) {
        return -1;
    }

    const char *ssid = argv[1];
    const char *password = (argc > 2) ? argv[2] : "";

    /*
     * Adding the network and letting wifi_manager join it, rather than calling
     * esp_wifi_connect() here. Two things fall out of that and both are wanted:
     * the credentials survive into the reconnect logic, so a link that drops
     * comes back on its own; and the manager's state stays true, which is what
     * `net status`, the status message and every NET_EVENT subscriber read.
     *
     * The network is added to the in-memory list only. config.json is the
     * authored configuration and a console command has no business editing it;
     * `POST /api/config` is how a network becomes permanent.
     */
    esp_err_t err = wifi_manager_add_known_network(ssid, password);
    if (err != ESP_OK) {
        STRRES_ERROR(STR_WIFI_ADD_FAILED, ssid, esp_err_to_name(err));
        return -1;
    }

    /* Station mode first: joining from AP mode has to drop the AP, since one
     * radio cannot serve two channels. */
    if (wifi_manager_get_state() == WIFI_MANAGER_AP_MODE) {
        STRRES_PRINTF(STR_WIFI_STOPPING_AP);
        err = wifi_manager_start_station_mode();
        if (err != ESP_OK) {
            STRRES_ERROR(STR_WIFI_STATION_MODE_FAILED, esp_err_to_name(err));
            return -1;
        }
    }

    STRRES_PRINTF(STR_WIFI_JOINING, ssid);
    err = wifi_manager_scan_and_connect();
    if (err != ESP_OK) {
        STRRES_ERROR(STR_WIFI_CONNECT_START_FAILED, esp_err_to_name(err));
        return -1;
    }

    /* Poll rather than subscribe: this runs on the executor task, so blocking
     * here blocks only the shell, and the alternative -- a NET_EVENT handler
     * signalling a semaphore -- is more machinery than a bench command needs. */
    for (int waited = 0; waited < CONNECT_TIMEOUT_MS; waited += CONNECT_POLL_MS) {
        vTaskDelay(pdMS_TO_TICKS(CONNECT_POLL_MS));
        if (wifi_manager_get_state() == WIFI_MANAGER_STA_CONNECTED) {
            char ip[16] = "";
            wifi_manager_get_address(ip, sizeof(ip));
            STRRES_PRINTF(STR_WIFI_JOINED, ssid, ip[0] ? ip : "(pending)");
            return 0;
        }
    }

    STRRES_ERROR(STR_WIFI_JOIN_TIMEOUT,
                 ssid, CONNECT_TIMEOUT_MS / 1000, state_name(wifi_manager_get_state()));
    return -1;
}

/* ------------------------------------------------------------------ */
/* ap                                                                  */
/* ------------------------------------------------------------------ */

int cmd_wifi_ap(int argc, char **argv)
{
    if (argc > 2) {
        STRRES_PRINTF(STR_WIFI_USAGE_AP);
        STRRES_PRINTF(STR_WIFI_AP_CONFIG_NOTE);
        return -1;
    }

    if (!require_radio_on()) {
        return -1;
    }

    if (argc == 2) {
        if (strcasecmp(argv[1], "stop") != 0) {
            STRRES_ERROR(STR_WIFI_UNKNOWN_ARGUMENT_AP, argv[1]);
            return -1;
        }
        if (wifi_manager_get_state() != WIFI_MANAGER_AP_MODE) {
            STRRES_PRINTF(STR_WIFI_NO_AP);
            return 0;
        }
        esp_err_t err = wifi_manager_start_station_mode();
        if (err != ESP_OK) {
            STRRES_ERROR(STR_WIFI_STATION_MODE_RETURN_FAILED, esp_err_to_name(err));
            return -1;
        }
        STRRES_PRINTF(STR_WIFI_AP_STOPPED);
        wifi_manager_scan_and_connect();
        return 0;
    }

    esp_err_t err = wifi_manager_start_ap_mode();
    if (err != ESP_OK) {
        STRRES_ERROR(STR_WIFI_AP_START_FAILED, esp_err_to_name(err));
        return -1;
    }
    return cmd_wifi_status(0, NULL);
}

/* ------------------------------------------------------------------ */
/* off / on                                                            */
/* ------------------------------------------------------------------ */

int cmd_wifi_off(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (wifi_manager_get_state() == WIFI_MANAGER_POWERED_OFF) {
        STRRES_PRINTF(STR_WIFI_RADIO_ALREADY_OFF);
        return 0;
    }

    if (running_iperf >= 0) {
        iperf_stop_instance(running_iperf);
        running_iperf = -1;
    }

    esp_err_t err = wifi_manager_stop();
    if (err != ESP_OK) {
        STRRES_ERROR(STR_WIFI_RADIO_STOP_FAILED, esp_err_to_name(err));
        return -1;
    }

    /*
     * This is for measurements, not for power. An associated radio transmits on
     * its own schedule, and on a board sharing a supply with an analog front end
     * that shows up in the readings. Being able to take the radio away and put
     * it back is what turns "the noise might be WiFi" into an answer.
     */
    STRRES_PRINTF(STR_WIFI_RADIO_OFF_NOTE);
    return 0;
}

int cmd_wifi_on(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (wifi_manager_get_state() != WIFI_MANAGER_POWERED_OFF) {
        STRRES_PRINTF(STR_WIFI_RADIO_ALREADY_ON, state_name(wifi_manager_get_state()));
        return 0;
    }

    esp_err_t err = wifi_manager_start_station_mode();
    if (err != ESP_OK) {
        STRRES_ERROR(STR_WIFI_RADIO_START_FAILED, esp_err_to_name(err));
        return -1;
    }

    STRRES_PRINTF(STR_WIFI_RADIO_ON);
    wifi_manager_scan_and_connect();
    return 0;
}

/* ------------------------------------------------------------------ */
/* status                                                              */
/* ------------------------------------------------------------------ */

static int report_ap_status(void)
{
    wifi_config_t config = {0};
    esp_err_t err = esp_wifi_get_config(WIFI_IF_AP, &config);
    if (err != ESP_OK) {
        STRRES_ERROR(STR_WIFI_AP_CONFIG_READ_FAILED, esp_err_to_name(err));
        return -1;
    }

    STRRES_PRINTF(STR_WIFI_INFO_MODE_AP);
    STRRES_PRINTF(STR_WIFI_INFO_SSID_AP, config.ap.ssid_len, (const char *)config.ap.ssid);
    STRRES_PRINTF(STR_WIFI_INFO_CHANNEL_AP, config.ap.channel);
    STRRES_PRINTF(STR_WIFI_INFO_SECURITY_AP, auth_mode_name(config.ap.authmode));

    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ip;
    if (ap_netif && esp_netif_get_ip_info(ap_netif, &ip) == ESP_OK) {
        diag_printf("IP:       " IPSTR "\n", IP2STR(&ip.ip));
        diag_printf("Netmask:  " IPSTR "\n", IP2STR(&ip.netmask));
    }

    wifi_sta_list_t stations;
    if (esp_wifi_ap_get_sta_list(&stations) != ESP_OK) {
        return 0;
    }

    STRRES_PRINTF(STR_WIFI_INFO_CLIENTS, stations.num, config.ap.max_connection);
    if (stations.num == 0) {
        return 0;
    }

    /* Pair the associations with the DHCP leases, so a client can be found by
     * address and not just by MAC. */
    wifi_sta_mac_ip_list_t leases = {0};
    bool have_ips = esp_wifi_ap_get_sta_list_with_ip(&stations, &leases) == ESP_OK;

    for (int i = 0; i < stations.num; i++) {
        if (have_ips) {
            diag_printf("  " MACSTR "  " IPSTR "  %d dBm\n",
                        MAC2STR(stations.sta[i].mac), IP2STR(&leases.sta[i].ip),
                        stations.sta[i].rssi);
        } else {
            diag_printf("  " MACSTR "  %d dBm\n", MAC2STR(stations.sta[i].mac),
                        stations.sta[i].rssi);
        }
    }

    return 0;
}

int cmd_wifi_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    wifi_manager_state_t state = wifi_manager_get_state();
    STRRES_PRINTF(STR_WIFI_INFO_MANAGER, state_name(state));

    if (state == WIFI_MANAGER_POWERED_OFF) {
        STRRES_PRINTF(STR_WIFI_INFO_SWITCHED_OFF);
        return 0;
    }

    if (state == WIFI_MANAGER_AP_MODE) {
        return report_ap_status();
    }

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        STRRES_PRINTF(STR_WIFI_INFO_NOT_ASSOCIATED);
        return 0;
    }

    STRRES_PRINTF(STR_WIFI_INFO_SSID, (const char *)ap.ssid);
    STRRES_PRINTF(STR_WIFI_INFO_BSSID,
                  ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5]);
    STRRES_PRINTF(STR_WIFI_INFO_RSSI, ap.rssi);
    STRRES_PRINTF(STR_WIFI_INFO_CHANNEL, ap.primary);
    STRRES_PRINTF(STR_WIFI_INFO_SECURITY, auth_mode_name(ap.authmode));
    STRRES_PRINTF(STR_WIFI_INFO_PHY, phy_mode_name(&ap));
    STRRES_PRINTF(STR_WIFI_INFO_BANDWIDTH, bandwidth_name(ap.bandwidth));

    int8_t tx_power = 0;
    if (esp_wifi_get_max_tx_power(&tx_power) == ESP_OK) {
        STRRES_PRINTF(STR_WIFI_INFO_TX_POWER, tx_power / 4);
    }

    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (sta_netif && esp_netif_get_ip_info(sta_netif, &ip) == ESP_OK) {
        diag_printf("IP:       " IPSTR "\n", IP2STR(&ip.ip));
        diag_printf("Gateway:  " IPSTR "\n", IP2STR(&ip.gw));
        diag_printf("Netmask:  " IPSTR "\n", IP2STR(&ip.netmask));
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* netstats                                                            */
/* ------------------------------------------------------------------ */

/* lwIP splits each layer's errors into six causes; summed into one count to
 * keep the netstats output as terse as the rest of the group. */
static uint32_t proto_errors(const struct stats_proto *s)
{
    return s->chkerr + s->lenerr + s->memerr + s->rterr + s->proterr + s->err;
}

int cmd_wifi_netstats(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    STRRES_PRINTF(STR_WIFI_STATS_LINK,
                  lwip_stats.link.recv, lwip_stats.link.drop,
                  (unsigned long)proto_errors(&lwip_stats.link));
    STRRES_PRINTF(STR_WIFI_STATS_IP,
                  lwip_stats.ip.recv, lwip_stats.ip.drop,
                  (unsigned long)proto_errors(&lwip_stats.ip));
    STRRES_PRINTF(STR_WIFI_STATS_TCP,
                  lwip_stats.tcp.recv, lwip_stats.tcp.drop,
                  (unsigned long)proto_errors(&lwip_stats.tcp));
    STRRES_PRINTF(STR_WIFI_STATS_UDP,
                  lwip_stats.udp.recv, lwip_stats.udp.drop,
                  (unsigned long)proto_errors(&lwip_stats.udp));

    return 0;
}

/* ------------------------------------------------------------------ */
/* iperf                                                               */
/* ------------------------------------------------------------------ */

/*
 * Override the iperf component's weak report hook so throughput lines go
 * through the diag fan-out and therefore reach the web console too. The
 * default implementation prints straight to stdout.
 */
void iperf_report_output(const iperf_report_t *report)
{
    if (report->report_type == IPERF_REPORT_CONNECT_INFO) {
        STRRES_PRINTF(STR_WIFI_IPERF_CONNECTED, report->connect_info.socket);
        STRRES_PRINTF(STR_WIFI_IPERF_HEADER, "Interval", "Transfer", "Bandwidth");
        return;
    }

    const iperf_traffic_report_t *traffic = &report->traffic;
    unsigned long start_sec = (report->report_type == IPERF_REPORT_SUMMARY) ? 0 : traffic->period_start_sec;
    uint32_t seconds = traffic->end_sec - start_sec;
    double bytes = (report->report_type == IPERF_REPORT_SUMMARY) ? traffic->total_transfer_bytes : traffic->period_bytes;
    double mbits_per_sec = seconds ? (bytes * 8.0) / ((double)seconds * 1000000.0) : 0.0;

    STRRES_PRINTF(STR_WIFI_IPERF_ROW,
                  start_sec, (unsigned long)traffic->end_sec, bytes / 1024.0, mbits_per_sec,
                  report->report_type == IPERF_REPORT_SUMMARY ? "  (total)" : "");
}

/* Parse "<host>:<port>", where the port is optional. */
static int parse_target(const char *text, esp_ip4_addr_t *addr, uint16_t *port)
{
    char host[64];
    const char *colon = strrchr(text, ':');

    if (colon) {
        size_t host_len = (size_t)(colon - text);
        if (host_len == 0 || host_len >= sizeof(host)) {
            return -1;
        }
        memcpy(host, text, host_len);
        host[host_len] = '\0';

        int value;
        if (cli_parse_int_arg(colon + 1, &value) < 0 || value < 1 || value > 65535) {
            return -1;
        }
        *port = (uint16_t)value;
    } else {
        if (strlen(text) >= sizeof(host)) {
            return -1;
        }
        strcpy(host, text);
        *port = IPERF_DEFAULT_PORT;
    }

    /* Accept a dotted quad directly, otherwise resolve the name. */
    struct in_addr parsed;
    if (inet_aton(host, &parsed)) {
        addr->addr = parsed.s_addr;
        return 0;
    }

    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *result = NULL;
    if (getaddrinfo(host, NULL, &hints, &result) != 0 || !result) {
        return -1;
    }

    addr->addr = ((struct sockaddr_in *)result->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(result);
    return 0;
}

int cmd_wifi_iperf(int argc, char **argv)
{
    if (argc < 2) {
        STRRES_PRINTF(STR_WIFI_USAGE_IPERF);
        STRRES_PRINTF(STR_WIFI_USAGE_IPERF_STOP);
        return -1;
    }

    if (strcasecmp(argv[1], "stop") == 0) {
        if (running_iperf < 0) {
            STRRES_ERROR(STR_WIFI_IPERF_NOT_RUNNING);
            return -1;
        }
        iperf_stop_instance(running_iperf);
        running_iperf = -1;
        STRRES_PRINTF(STR_WIFI_IPERF_STOPPED);
        return 0;
    }

    if (!require_radio_on()) {
        return -1;
    }

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        STRRES_ERROR(STR_WIFI_IPERF_NOT_ASSOCIATED);
        return -1;
    }

    STRRES_PRINTF(STR_WIFI_IPERF_LINK,
                  ap.rssi, phy_mode_name(&ap), bandwidth_name(ap.bandwidth));

    bool continuous = (argc > 2 && strcasecmp(argv[2], "continuous") == 0);

    esp_ip4_addr_t server = {0};
    uint16_t port = IPERF_DEFAULT_PORT;
    if (parse_target(argv[1], &server, &port) < 0) {
        STRRES_ERROR(STR_WIFI_COULD_NOT_RESOLVE, argv[1]);
        return -1;
    }

    if (running_iperf >= 0) {
        iperf_stop_instance(running_iperf);
        running_iperf = -1;
    }

    esp_ip_addr_t destination = {0};
    destination.u_addr.ip4 = server;
    destination.type = ESP_IPADDR_TYPE_V4;

    iperf_cfg_t config = IPERF_DEFAULT_CONFIG_CLIENT(IPERF_FLAG_TCP, destination);
    config.dport = port;
    config.interval = continuous ? IPERF_CONTINUOUS_INTERVAL_S : IPERF_DEFAULT_INTERVAL;
    config.time = continuous ? IPERF_CONTINUOUS_DURATION_S : IPERF_SINGLE_DURATION_S;

    diag_printf("iperf2 TCP client -> " IPSTR ":%u, reporting every %lu s\n",
                IP2STR(&server), port, (unsigned long)config.interval);

    running_iperf = iperf_start_instance(&config);
    if (running_iperf < 0) {
        STRRES_ERROR(STR_WIFI_IPERF_START_FAILED);
        return -1;
    }

    if (continuous) {
        STRRES_PRINTF(STR_WIFI_IPERF_CONTINUOUS);
        return 0;
    }

    /* iperf runs in its own task; wait for the fixed-duration run to finish so
     * the prompt does not come back before the summary. */
    vTaskDelay(pdMS_TO_TICKS((IPERF_SINGLE_DURATION_S + 2) * 1000));
    iperf_stop_instance(running_iperf);
    running_iperf = -1;
    return 0;
}
