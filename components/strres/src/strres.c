/*
 * The reader for the .sr catalogues strres_compile.py produces.
 *
 * A section is read whole into one allocation and kept, because the access
 * pattern is bursty rather than uniform: printing one group's help touches a
 * dozen strings that all live in the same file. Caching whole sections makes
 * that one filesystem read instead of a dozen, which is the entire reason this
 * is affordable on a CLI.
 *
 * Only stdio is used, never the VFS API directly, so the application chooses
 * and mounts the filesystem and the reader can be tested on the host against
 * real files -- the same reasoning config_store gives for taking paths.
 */

#include "strres.h"
#include "strres_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag.h"
#include "sdkconfig.h"

#ifndef CONFIG_STRRES_CACHE_SECTIONS
#define CONFIG_STRRES_CACHE_SECTIONS 3
#endif
#ifndef CONFIG_STRRES_FORMAT_BUF_SIZE
#define CONFIG_STRRES_FORMAT_BUF_SIZE 256
#endif
#ifndef CONFIG_STRRES_DEFAULT_LOCALE
#define CONFIG_STRRES_DEFAULT_LOCALE "en-US"
#endif
#ifndef CONFIG_STRRES_BASE_DIR
#define CONFIG_STRRES_BASE_DIR "/res/str"
#endif

#define PATH_MAX_LEN 96
#define LOCALE_MAX_LEN 16

/*
 * The lock. The command executor is single-threaded, but diag sinks and log
 * redirection can reach here from another task, so the cache needs one.
 *
 * It is never held across a call into diag: strres_printf() takes a hold,
 * releases the lock, formats, and only then drops the hold. Nesting this inside
 * diag's own lock would be a deadlock waiting for a second interface to attach.
 */
#if defined(STRRES_HOST_TEST)
#define LOCK_INIT() do { } while (0)
#define LOCK()      do { } while (0)
#define UNLOCK()    do { } while (0)
#else
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
#define LOCK_INIT() do { if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf); } while (0)
#define LOCK()      do { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); } while (0)
#define UNLOCK()    do { if (s_lock) xSemaphoreGive(s_lock); } while (0)
#endif

static const strres_catalog_t *s_catalog;
static char s_base_dir[PATH_MAX_LEN];
static char s_locale[LOCALE_MAX_LEN];
static bool s_ready;
static bool s_skewed;
static uint32_t s_clock;

/* Slot 0 is the pinned section, held for the life of the process and outside
 * the LRU budget. The rest are the cache proper. */
static strres_section_t s_pin;
static strres_section_t s_cache[CONFIG_STRRES_CACHE_SECTIONS];

/* ------------------------------------------------------------------ format */

esp_err_t strres_section_open(strres_section_t *out, uint8_t *file, size_t size,
                              uint32_t expect_hash)
{
    memset(out, 0, sizeof(*out));

    if (size < STRRES_HEADER_BYTES || memcmp(file, STRRES_MAGIC, 4) != 0) {
        return ESP_ERR_STRRES_BAD_SECTION;
    }

    uint32_t hash = (uint32_t)file[4] | ((uint32_t)file[5] << 8) |
                    ((uint32_t)file[6] << 16) | ((uint32_t)file[7] << 24);
    if (hash != expect_hash) {
        return ESP_ERR_STRRES_CATALOG_SKEW;
    }

    uint16_t count = (uint16_t)(file[8] | ((uint16_t)file[9] << 8));

    /* count + 1 offsets, the last a sentinel giving the final string's length. */
    size_t table = (size_t)(count + 1) * 2u;
    if (size < STRRES_HEADER_BYTES + table) {
        return ESP_ERR_STRRES_BAD_SECTION;
    }

    const uint8_t *offsets = file + STRRES_HEADER_BYTES;
    size_t blob_len = size - STRRES_HEADER_BYTES - table;

    /* The sentinel must span exactly the blob, and the blob must end in a
     * terminator -- together those are what let strres_section_at() hand out a
     * bare char* without a length. */
    if (strres_off(offsets, count) != blob_len || blob_len == 0 ||
        file[size - 1] != '\0') {
        return ESP_ERR_STRRES_BAD_SECTION;
    }

    out->count = count;
    out->file = file;
    out->size = size;
    out->offsets = offsets;
    out->blob = (const char *)(offsets + table);
    out->blob_len = blob_len;
    return ESP_OK;
}

const char *strres_section_at(const strres_section_t *s, unsigned index)
{
    if (!s->file || index >= s->count) {
        return NULL;
    }
    uint16_t start = strres_off(s->offsets, index);
    uint16_t end = strres_off(s->offsets, index + 1);
    /* A corrupt table could run backwards or point past the blob; the blob's
     * closing terminator alone would not save us from either. */
    if (end <= start || (size_t)end > s->blob_len) {
        return NULL;
    }
    return s->blob + start;
}

/* -------------------------------------------------------------------- load */

static void section_free(strres_section_t *s)
{
    free(s->file);
    memset(s, 0, sizeof(*s));
}

/* Read one .sr whole. Returns the allocation and its length. */
static uint8_t *read_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    uint8_t *buf = NULL;
    long size = 0;
    if (fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) > 0 &&
        fseek(f, 0, SEEK_SET) == 0) {
        buf = malloc((size_t)size);
        if (buf && fread(buf, 1, (size_t)size, f) != (size_t)size) {
            free(buf);
            buf = NULL;
        }
    }
    fclose(f);
    if (buf) {
        *out_size = (size_t)size;
    }
    return buf;
}

static esp_err_t load_section(strres_section_t *out, uint16_t section)
{
    if (!s_catalog || section >= s_catalog->section_count) {
        return ESP_ERR_INVALID_ARG;
    }

    char path[PATH_MAX_LEN];
    int n = snprintf(path, sizeof(path), "%s/%s/%s.sr", s_base_dir, s_locale,
                     s_catalog->section_names[section]);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t size = 0;
    uint8_t *file = read_file(path, &size);
    if (!file) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = strres_section_open(out, file, size, s_catalog->hash);
    if (err != ESP_OK) {
        free(file);
        return err;
    }
    out->section = section;
    out->holds = 0;
    out->used = ++s_clock;
    return ESP_OK;
}

/*
 * The cached section for `section`, loading and evicting as needed.
 *
 * Eviction is least-recently-used among slots nothing holds. If every slot is
 * held the lookup fails rather than growing the cache -- a caller holding
 * CONFIG_STRRES_CACHE_SECTIONS sections at once is a bug in the caller, and
 * silently allocating past the configured budget would hide it.
 *
 * Caller must hold the lock.
 */
static strres_section_t *acquire(uint16_t section)
{
    if (s_pin.file && s_pin.section == section) {
        return &s_pin;
    }

    for (size_t i = 0; i < CONFIG_STRRES_CACHE_SECTIONS; i++) {
        if (s_cache[i].file && s_cache[i].section == section) {
            s_cache[i].used = ++s_clock;
            return &s_cache[i];
        }
    }

    strres_section_t *victim = NULL;
    for (size_t i = 0; i < CONFIG_STRRES_CACHE_SECTIONS; i++) {
        if (!s_cache[i].file) {
            victim = &s_cache[i];
            break;
        }
        if (s_cache[i].holds == 0 && (!victim || s_cache[i].used < victim->used)) {
            victim = &s_cache[i];
        }
    }
    if (!victim || victim->holds != 0) {
        return NULL;
    }
    if (victim->file) {
        section_free(victim);
    }
    if (load_section(victim, section) != ESP_OK) {
        memset(victim, 0, sizeof(*victim));
        return NULL;
    }
    return victim;
}

/* ------------------------------------------------------------------ public */

esp_err_t strres_init(const strres_config_t *cfg, const strres_catalog_t *catalog)
{
    if (!catalog || !catalog->section_names || catalog->section_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    LOCK_INIT();
    LOCK();

    for (size_t i = 0; i < CONFIG_STRRES_CACHE_SECTIONS; i++) {
        if (s_cache[i].file) {
            section_free(&s_cache[i]);
        }
    }
    if (s_pin.file) {
        section_free(&s_pin);
    }

    s_catalog = catalog;
    s_skewed = false;
    snprintf(s_base_dir, sizeof(s_base_dir), "%s",
             (cfg && cfg->base_dir) ? cfg->base_dir : CONFIG_STRRES_BASE_DIR);
    snprintf(s_locale, sizeof(s_locale), "%s",
             (cfg && cfg->locale) ? cfg->locale : CONFIG_STRRES_DEFAULT_LOCALE);

    /* Section 0 is the pinned set. Loading it now both makes those strings free
     * to reach later and answers, once, whether the resources on disk match
     * this firmware. */
    esp_err_t err = load_section(&s_pin, 0);
    if (err == ESP_ERR_NOT_FOUND && strcmp(s_locale, CONFIG_STRRES_DEFAULT_LOCALE) != 0) {
        snprintf(s_locale, sizeof(s_locale), "%s", CONFIG_STRRES_DEFAULT_LOCALE);
        err = load_section(&s_pin, 0);
    }
    if (err == ESP_OK) {
        s_pin.pinned = true;
    } else {
        memset(&s_pin, 0, sizeof(s_pin));
        if (err == ESP_ERR_STRRES_CATALOG_SKEW) {
            s_skewed = true;
        }
    }

    s_ready = true;
    UNLOCK();
    return err;
}

esp_err_t strres_set_locale(const char *locale)
{
    if (!locale || !*locale) {
        return ESP_ERR_INVALID_ARG;
    }
    /* strres_init() writes the base directory into s_base_dir, so it must not
     * be handed s_base_dir itself to read from. Copy first. */
    char base[PATH_MAX_LEN];
    snprintf(base, sizeof(base), "%s", s_base_dir);

    strres_config_t cfg = { .base_dir = base, .locale = locale };
    return strres_init(&cfg, s_catalog);
}

const char *strres_locale(void)
{
    return s_locale;
}

uint32_t strres_catalog_hash(void)
{
    return s_catalog ? s_catalog->hash : 0;
}

const char *strres_hold(strres_id_t id, strres_hold_t *hold)
{
    if (hold) {
        hold->section = NULL;
    }
    if (!hold || !s_ready || id == STRRES_ID_NONE) {
        return NULL;
    }

    LOCK();
    strres_section_t *s = acquire((uint16_t)(id >> STRRES_INDEX_BITS));
    const char *text = NULL;
    if (s) {
        text = strres_section_at(s, id & STRRES_INDEX_MASK);
        if (text) {
            s->holds++;
            hold->section = s;
        }
    }
    UNLOCK();
    return text;
}

void strres_release(strres_hold_t *hold)
{
    if (!hold || !hold->section) {
        return;
    }
    LOCK();
    strres_section_t *s = hold->section;
    if (s->holds > 0) {
        s->holds--;
    }
    hold->section = NULL;
    UNLOCK();
}

int strres_copy(strres_id_t id, char *buf, size_t buflen)
{
    if (!buf || buflen == 0) {
        return -1;
    }
    strres_hold_t hold;
    const char *text = strres_hold(id, &hold);
    if (!text) {
        buf[0] = '\0';
        strres_release(&hold);
        return -1;
    }
    int len = snprintf(buf, buflen, "%s", text);
    strres_release(&hold);
    return len;
}

/*
 * A string that is not there.
 *
 * The id is printed rather than a name: names would be a table of strings in
 * the image, which is what this component exists to avoid. Decode it with the
 * strres_ids.txt the build emits, or turn on CONFIG_STRRES_DEBUG_NAMES.
 */
static void print_miss(strres_id_t id, bool as_error)
{
    if (as_error) {
        diag_error("[str:%04X]", (unsigned)id);
    } else {
        diag_printf("[str:%04X]\n", (unsigned)id);
    }
}

void strres_vprintf(strres_id_t id, va_list args)
{
    strres_hold_t hold;
    const char *fmt = strres_hold(id, &hold);
    if (fmt) {
        diag_vprintf(fmt, args);
    } else {
        print_miss(id, false);
    }
    strres_release(&hold);
}

void strres_printf(strres_id_t id, ...)
{
    va_list args;
    va_start(args, id);
    strres_vprintf(id, args);
    va_end(args);
}

void strres_error(strres_id_t id, ...)
{
    strres_hold_t hold;
    const char *fmt = strres_hold(id, &hold);
    if (!fmt) {
        strres_release(&hold);
        print_miss(id, true);
        return;
    }

    /*
     * diag has no verror(), and splitting the prefix from the text across two
     * diag calls would let another task's line land in the middle of this one.
     * Format first, emit once.
     */
    char buf[CONFIG_STRRES_FORMAT_BUF_SIZE];
    va_list args;
    va_start(args, id);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    strres_release(&hold);

    diag_error("%s", buf);
}
