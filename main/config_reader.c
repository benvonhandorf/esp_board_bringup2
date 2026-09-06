#include "config_reader.h"

#include <inttypes.h>
#include <string.h>

#include "config_store.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "js2c_error_capture.h"
#include "strres.h"
#include "strres_ids.h"

static const char *TAG = "config";

#define CONFIG_BUF_SIZE 4096

/* The LittleFS partition named in the partition table, and its mount point. */
#define FS_PARTITION "res"
#define FS_MOUNT     "/res"

static char s_source[8] = "none";

/*
 * One line per section, dispatched by the X-macro the schema generated.
 *
 * Adding a section to config_schema.json without wiring it here does not
 * compile: APP_CONFIG_SECTIONS expands to an X() naming a parser and a struct
 * member that do not exist yet. That is the whole reason the walker is generated
 * rather than hand-written -- the two cannot drift apart.
 */
#define SECTION_PARSER_wifi json_parse_wifi_manager_config_with_len
#define SECTION_PARSER_mqtt json_parse_mqtt_manager_config_with_len
#define SECTION_PARSER_ntp  json_parse_ntp_manager_config_with_len
#define SECTION_PARSER_http json_parse_http_server_config_with_len

#define PARSE_SECTION(name)                                                   \
    do {                                                                      \
        const app_config_json_ref_t *slice = &out->top.name;                  \
        js2c_error_capture_reset();                                           \
        /* In place: the slice indexes the buffer; no section is copied. */  \
        if (SECTION_PARSER_##name(json + slice->index, slice->length,         \
                                  &out->name)) {                              \
            snprintf(err, err_len, "%s: %s", #name, js2c_error_capture_get()); \
            return ESP_ERR_INVALID_ARG;                                       \
        }                                                                     \
    } while (0);

static esp_err_t parse(app_config_full_t *out, const char *json, size_t len,
                       char *err, size_t err_len)
{
    memset(out, 0, sizeof(*out));
    err[0] = '\0';

    js2c_error_capture_reset();
    if (json_parse_app_config_with_len(json, len, &out->top)) {
        snprintf(err, err_len, "%s", js2c_error_capture_get());
        return ESP_ERR_INVALID_ARG;
    }

    APP_CONFIG_SECTIONS(PARSE_SECTION)
    return ESP_OK;
}

/*
 * The configuration a unit runs when it has none of its own: every section
 * present and empty, so each is filled from the defaults in its owning
 * component's schema. That only works if every field either has a default or is
 * genuinely required -- a required field with no default aborts its section
 * before the rest of the defaults are applied, and the section comes back
 * zeroed. That was the bug behind a `scan_interval_ms` of 0 and a radio that
 * scanned without pause. The fix belongs in the schema; this document is what
 * proves it, at boot, on the path a freshly flashed unit takes.
 */
static const char DEFAULT_CONFIG[] =
    "{\"config_version\":1,"
    "\"wifi\":{},"
    /*
     * Spelled out rather than left empty, and mqtt_manager's schema is right to
     * require these two: a broker with no address is a misconfiguration, not a
     * default, and a config file that omits them should be rejected. This
     * document is not a config file, though -- it is the absence of one, and
     * empty strings are how the application already spells "no broker".
     * main.c skips mqtt_manager_start() on an empty uri and says so.
     */
    "\"mqtt\":{\"uri\":\"\",\"topic_prefix\":\"\"},"
    "\"ntp\":{},\"http\":{}}";

static esp_err_t parse_defaults(app_config_full_t *out)
{
    char err[128] = "";
    esp_err_t err_code = parse(out, DEFAULT_CONFIG, strlen(DEFAULT_CONFIG),
                               err, sizeof(err));
    if (err_code != ESP_OK) {
        /*
         * Reachable only if a schema gained a required field with no default,
         * which no configuration file can cause and no test over a valid file
         * will catch. `out` is zeroed rather than defaulted at this point, and
         * downstream a zeroed field is a live value, not an unset one -- so say
         * plainly that it is the defaults themselves that are broken.
         */
        ESP_LOGE(TAG, "built-in defaults do not parse: %s", err);
        ESP_LOGE(TAG, "every unset field is now zero, not its default");
    }
    return err_code;
}

static esp_err_t mount_filesystem(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = FS_MOUNT,
        .partition_label = FS_PARTITION,
        /* A first boot, or a partition resized by a new table, arrives here with
         * nothing readable. Formatting beats refusing to start: the defaults are
         * enough to bring the console up and be reconfigured. */
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mounting %s: %s", FS_PARTITION, esp_err_to_name(err));
    }
    return err;
}

/*
 * The user-visible strings live on the partition just mounted, so this is the
 * earliest point they can be read -- and it is before the shell starts, which
 * is the first thing that prints any.
 *
 * A failure here is reported and not fatal. Every lookup then misses and prints
 * its id, which is legible enough to diagnose from, and the alternative -- a
 * device that will not boot because its text is missing -- is worse. The
 * messages on this path stay C literals for the same reason.
 */
static void load_strings(void)
{
    esp_err_t err = strres_init(NULL, &strres_generated_catalog);
    if (err == ESP_ERR_STRRES_CATALOG_SKEW) {
        ESP_LOGE(TAG, "string resources are from a different build "
                      "(firmware catalog %08" PRIX32 "); reflash the res partition",
                 strres_catalog_hash());
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "loading strings from %s: %s", FS_MOUNT, esp_err_to_name(err));
    }
}

esp_err_t config_reader_load(app_config_full_t *out, char *err, size_t err_len)
{
    mount_filesystem();
    load_strings();

    /*
     * An SD card overrides the built-in copy, so a unit can be reconfigured
     * without reflashing. Writes still go to the internal filesystem: a card can
     * be pulled at any moment, and the device has to boot without it.
     */
    static const config_store_config_t store = {
        .path = FS_MOUNT "/config.json",
        .override_path = "/sdcard/config.json",
        .max_size = CONFIG_BUF_SIZE,
    };
    esp_err_t cfg_err = config_store_init(&store);
    if (cfg_err != ESP_OK) {
        snprintf(err, err_len, "config_store: %s", esp_err_to_name(cfg_err));
        return cfg_err;
    }

    /* Static rather than on the stack: 4 kB would overflow most task stacks, and
     * this runs once at boot on app_main's. */
    static char json[CONFIG_BUF_SIZE];
    size_t len = 0;
    config_store_source_t source = CONFIG_STORE_SOURCE_NONE;

    esp_err_t read_err = config_store_read(json, sizeof(json), &len, &source);
    if (read_err == ESP_ERR_NOT_FOUND) {
        /*
         * Not an error. Every optional field has a default in its owning
         * component's schema, so an empty document yields a usable
         * configuration -- the device comes up on its own access point with the
         * console running, which is exactly what a freshly flashed unit needs.
         */
        ESP_LOGW(TAG, "no config.json; using schema defaults");
        snprintf(s_source, sizeof(s_source), "default");
        return parse_defaults(out);
    }
    if (read_err != ESP_OK) {
        snprintf(err, err_len, "reading config: %s", esp_err_to_name(read_err));
        return read_err;
    }

    snprintf(s_source, sizeof(s_source), "%s",
             source == CONFIG_STORE_SOURCE_OVERRIDE ? "sdcard" : "flash");

    esp_err_t parse_err = parse(out, json, len, err, err_len);
    if (parse_err != ESP_OK) {
        ESP_LOGE(TAG, "config from %s is invalid: %s", s_source, err);
        /*
         * parse() zeroes `out` and returns at the first section that fails, so
         * without this the caller is handed a struct of zeros -- not defaults,
         * whatever the message it prints next says. Nothing downstream reads a
         * zero as "unset": wifi_manager took a scan_interval_ms of 0 and rearmed
         * its scan timer for "now", indefinitely. Reparsing the defaults costs a
         * few hundred microseconds once at boot, and is the difference between
         * "the configuration was rejected" and "the configuration was rejected
         * and the device now misbehaves in a way that does not name it".
         *
         * All of it is replaced, not only the section that failed: a device
         * running half the operator's configuration and half defaults is harder
         * to diagnose than one running defaults and saying so. `err` still holds
         * the original failure, so the caller reports the real reason.
         */
        parse_defaults(out);
        snprintf(s_source, sizeof(s_source), "default");
    } else {
        ESP_LOGI(TAG, "config loaded from %s", s_source);
    }
    return parse_err;
}

esp_err_t config_reader_store(const char *json, size_t len, char *err, size_t err_len)
{
    /* Parse before storing. A configuration that will not load must not be
     * allowed to replace one that does, because the way back is a cable. */
    static app_config_full_t scratch;
    esp_err_t parse_err = parse(&scratch, json, len, err, err_len);
    if (parse_err != ESP_OK) {
        return parse_err;
    }

    return config_store_write(json, len);
}

const char *config_reader_source(void)
{
    return s_source;
}
