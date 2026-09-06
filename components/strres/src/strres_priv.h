#ifndef STRRES_PRIV_H
#define STRRES_PRIV_H

/* Not on the include path -- this header is private to the component, and is
 * shared only with the host tests so they can inspect the cache. */

#include <stddef.h>
#include <stdint.h>

#include "strres.h"

#define STRRES_MAGIC "SR01"
#define STRRES_HEADER_BYTES 12
#define STRRES_INDEX_BITS 10
#define STRRES_INDEX_MASK ((strres_id_t)((1u << STRRES_INDEX_BITS) - 1))

/* One cached section: the whole file in one allocation, with the offset table
 * and blob pointed into it rather than copied out again. */
typedef struct {
    uint16_t section;       /* index into the catalogue */
    uint16_t count;
    uint32_t used;          /* LRU stamp; 0 when the slot is empty */
    int holds;              /* outstanding strres_hold()s; never evicted above 0 */
    bool pinned;            /* section 0: loaded once, outside the LRU budget */
    uint8_t *file;          /* the allocation */
    size_t size;
    const uint8_t *offsets; /* into file; read with strres_off() for alignment */
    const char *blob;
    size_t blob_len;
} strres_section_t;

/* The offset table is uint16 little-endian but sits at a 12-byte boundary, so
 * it is not naturally aligned on every target. Read it a byte at a time. */
static inline uint16_t strres_off(const uint8_t *offsets, unsigned i)
{
    return (uint16_t)(offsets[2u * i] | ((uint16_t)offsets[2u * i + 1] << 8));
}

/* Parse and validate a whole .sr image already in memory. Takes ownership of
 * `file` on success. Exposed for the host tests. */
esp_err_t strres_section_open(strres_section_t *out, uint8_t *file, size_t size,
                              uint32_t expect_hash);

/* The string at `index`, or NULL if the section does not carry one. */
const char *strres_section_at(const strres_section_t *s, unsigned index);

#endif /* STRRES_PRIV_H */
