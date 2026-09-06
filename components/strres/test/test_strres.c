/*
 * Host test for the .sr reader and, through the fixtures it compiles first, for
 * strres_compile.py as well. The cache is two slots wide here (see
 * stubs/sdkconfig.h) so eviction and the all-slots-held refusal are reachable.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag.h"
#include "strres.h"
#include "strres_ids.h"
#include "strres_priv.h"

static int failures;

static void expect(bool cond, const char *what)
{
    printf("%s: %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) {
        failures++;
    }
}

static void expect_text(const char *want, const char *what)
{
    const char *got = diag_capture_text();
    bool ok = strcmp(want, got) == 0;
    printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        printf("      want %s\n      got  %s\n", want, got);
        failures++;
    }
}

/* Build a valid section image by hand so a field can be corrupted. */
static uint8_t *make_section(uint32_t hash, uint16_t count, const char *const *texts,
                             size_t *out_size)
{
    size_t blob = 0;
    for (uint16_t i = 0; i < count; i++) {
        blob += strlen(texts[i]) + 1;
    }
    size_t table = (size_t)(count + 1) * 2;
    size_t size = STRRES_HEADER_BYTES + table + blob;
    uint8_t *f = calloc(1, size);

    memcpy(f, STRRES_MAGIC, 4);
    f[4] = hash & 0xFF; f[5] = (hash >> 8) & 0xFF;
    f[6] = (hash >> 16) & 0xFF; f[7] = (hash >> 24) & 0xFF;
    f[8] = count & 0xFF; f[9] = (count >> 8) & 0xFF;

    size_t at = 0;
    uint8_t *offsets = f + STRRES_HEADER_BYTES;
    char *out = (char *)(offsets + table);
    for (uint16_t i = 0; i < count; i++) {
        offsets[2 * i] = at & 0xFF;
        offsets[2 * i + 1] = (at >> 8) & 0xFF;
        size_t n = strlen(texts[i]) + 1;
        memcpy(out + at, texts[i], n);
        at += n;
    }
    offsets[2 * count] = at & 0xFF;
    offsets[2 * count + 1] = (at >> 8) & 0xFF;

    *out_size = size;
    return f;
}

static void test_format(void)
{
    const char *const texts[] = { "first", "second" };
    size_t size;
    strres_section_t s;

    uint8_t *f = make_section(0xABCD1234u, 2, texts, &size);
    expect(strres_section_open(&s, f, size, 0xABCD1234u) == ESP_OK, "a valid section opens");
    expect(strcmp(strres_section_at(&s, 0), "first") == 0, "index 0 is the first string");
    expect(strcmp(strres_section_at(&s, 1), "second") == 0, "index 1 is the second");
    expect(strres_section_at(&s, 2) == NULL, "an index past the end is a miss");
    free(f);

    f = make_section(0xABCD1234u, 2, texts, &size);
    expect(strres_section_open(&s, f, size, 0x11111111u) == ESP_ERR_STRRES_CATALOG_SKEW,
           "a catalogue hash that disagrees is skew, not corruption");
    free(f);

    f = make_section(0xABCD1234u, 2, texts, &size);
    f[1] = 'X';
    expect(strres_section_open(&s, f, size, 0xABCD1234u) == ESP_ERR_STRRES_BAD_SECTION,
           "a wrong magic is rejected");
    free(f);

    f = make_section(0xABCD1234u, 2, texts, &size);
    expect(strres_section_open(&s, f, size - 4, 0xABCD1234u) == ESP_ERR_STRRES_BAD_SECTION,
           "a truncated file is rejected");
    free(f);

    f = make_section(0xABCD1234u, 2, texts, &size);
    expect(strres_section_open(&s, f, STRRES_HEADER_BYTES - 1, 0xABCD1234u)
               == ESP_ERR_STRRES_BAD_SECTION,
           "a file shorter than the header is rejected");
    free(f);

    /*
     * The invariant that matters for a corrupt table is that no pointer ever
     * leaves the blob -- an offset pair that merely aliases another string
     * yields the wrong text, which is a resource bug, not a memory one.
     */
    f = make_section(0xABCD1234u, 2, texts, &size);
    expect(strres_section_open(&s, f, size, 0xABCD1234u) == ESP_OK, "reopened for corruption");
    uint8_t *offsets = f + STRRES_HEADER_BYTES;
    offsets[2] = 0xFF; offsets[3] = 0xFF;    /* string 1 starts past the end */
    expect(strres_section_at(&s, 1) == NULL, "an offset past the blob is a miss");
    free(f);

    f = make_section(0xABCD1234u, 2, texts, &size);
    expect(strres_section_open(&s, f, size, 0xABCD1234u) == ESP_OK, "reopened again");
    offsets = f + STRRES_HEADER_BYTES;
    offsets[4] = 0; offsets[5] = 0;          /* sentinel now precedes string 1 */
    expect(strres_section_at(&s, 1) == NULL, "an offset pair running backwards is a miss");
    free(f);
}

static void test_lookup(void)
{
    strres_config_t cfg = { .base_dir = "fixtures-build", .locale = "en-US" };
    expect(strres_init(&cfg, &strres_generated_catalog) == ESP_OK, "init loads the pinned section");
    expect(strres_catalog_hash() == STRRES_CATALOG_HASH, "the firmware's catalogue hash is reported");

    char buf[64];
    expect(strres_copy(STR_DEMO_PLAIN, buf, sizeof(buf)) == 32, "copy returns the length");
    expect(strcmp(buf, "a plain string with no arguments") == 0, "copy returns the text");

    expect(strres_copy(STR_DEMO_PINNED, buf, sizeof(buf)) > 0, "a pinned string resolves");
    expect(strcmp(buf, "pinned and always resident") == 0, "the pinned text is right");

    /* Truncation reports what would have been needed, as snprintf does. */
    char small[10];
    expect(strres_copy(STR_DEMO_PLAIN, small, sizeof(small)) == 32,
           "a short buffer still reports the full length");
    expect(strlen(small) == 9 && small[9] == '\0', "and is left NUL-terminated");

    diag_capture_reset();
    STRRES_PRINTF(STR_DEMO_PLAIN);
    expect_text("a plain string with no arguments", "a plain string prints");

    diag_capture_reset();
    STRRES_PRINTF(STR_DEMO_ONE_ARG, 17);
    expect_text("GPIO 17 does not exist on this chip", "one argument is substituted");

    diag_capture_reset();
    STRRES_PRINTF(STR_DEMO_TWO_ARGS, 3, "s");
    expect_text("3 devices responding", "two arguments are substituted");

    diag_capture_reset();
    STRRES_ERROR(STR_DEMO_ONE_ARG, 4);
    expect_text("ERR: GPIO 4 does not exist on this chip\n", "an error is prefixed and terminated");
}

static void test_misses(void)
{
    char buf[64];

    expect(strres_copy(STRRES_ID_NONE, buf, sizeof(buf)) == -1, "id 0 is reserved and misses");

    /* Section 1 exists; index 900 does not. */
    expect(strres_copy((strres_id_t)((1u << 10) | 900), buf, sizeof(buf)) == -1,
           "an index past a section's end misses");

    /* Section 60 is not in the catalogue at all. */
    expect(strres_copy((strres_id_t)(60u << 10), buf, sizeof(buf)) == -1,
           "an unknown section misses");

    diag_capture_reset();
    strres_printf((strres_id_t)((1u << 10) | 900));
    expect_text("[str:0784]\n", "a miss prints its id so it can be looked up");

    diag_capture_reset();
    strres_error((strres_id_t)((1u << 10) | 900));
    expect_text("ERR: [str:0784]\n", "a missing error string still reports as an error");
}

static void test_cache(void)
{
    char buf[64];

    /* Three sections through a two-slot cache: the first is evicted and must
     * still read correctly when it is loaded again. */
    expect(strres_copy(STR_DEMO_PLAIN, buf, sizeof(buf)) > 0, "demo loads");
    expect(strres_copy(STR_SECOND_ALPHA, buf, sizeof(buf)) > 0, "second loads");
    expect(strres_copy(STR_THIRD_ONLY, buf, sizeof(buf)) > 0, "third loads, evicting demo");
    expect(strres_copy(STR_DEMO_PLAIN, buf, sizeof(buf)) == 32, "demo reloads intact");
    expect(strcmp(buf, "a plain string with no arguments") == 0, "and reads the same");

    /* A held section must survive pressure that would otherwise evict it. */
    strres_hold_t h;
    const char *held = strres_hold(STR_DEMO_PLAIN, &h);
    expect(held != NULL, "a hold returns the text");
    expect(strres_copy(STR_SECOND_ALPHA, buf, sizeof(buf)) > 0, "another section still loads");
    expect(strres_copy(STR_THIRD_ONLY, buf, sizeof(buf)) > 0, "and a third takes the free slot");
    expect(strcmp(held, "a plain string with no arguments") == 0,
           "the held pointer is still valid after eviction pressure");
    strres_release(&h);
    expect(h.section == NULL, "release clears the hold");

    /* With every slot held there is nowhere to put a fourth section, and the
     * documented behaviour is to miss rather than to allocate past the budget. */
    strres_hold_t a, b;
    expect(strres_hold(STR_DEMO_PLAIN, &a) != NULL, "hold one");
    expect(strres_hold(STR_SECOND_ALPHA, &b) != NULL, "hold two, filling the cache");
    strres_hold_t c;
    expect(strres_hold(STR_THIRD_ONLY, &c) == NULL,
           "a third section with every slot held misses rather than overrunning the budget");
    strres_release(&c);
    strres_release(&a);
    strres_release(&b);
    expect(strres_copy(STR_THIRD_ONLY, buf, sizeof(buf)) > 0, "and works again once released");
}

static void test_locale(void)
{
    expect(strcmp(strres_locale(), "en-US") == 0, "the configured locale is reported");
    expect(strres_set_locale("xx-XX") == ESP_OK, "an absent locale falls back rather than failing");
    expect(strcmp(strres_locale(), "en-US") == 0, "and reports the locale actually in use");

    char buf[64];
    expect(strres_copy(STR_DEMO_PLAIN, buf, sizeof(buf)) == 32, "strings still resolve after fallback");
}

int main(void)
{
    test_format();
    test_lookup();
    test_misses();
    test_cache();
    test_locale();

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "passed", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
