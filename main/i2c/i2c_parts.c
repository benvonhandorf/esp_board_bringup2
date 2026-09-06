#include "app_bringup.h"
#include "i2c_parts.h"

/* Address constants come from the components that own the parts, so a range
 * corrected in a driver cannot drift from what the scan suggests. Where a
 * component names only its default address, that default is the bottom of the
 * strap range and the span is written out here. */
#include "aw9523b.h"
#include "ina219.h"
#include "ina226.h"
#include "ina237.h"
#include "lm75bdp.h"
#include "nau7802.h"
#include "pi4ioe5v6408.h"
#include "rx8130ce.h"
#include "sht4x.h"

/* The NAU8822's addresses are private to main/audio/codec_nau8822.c, which has
 * no header of its own -- it is a codec vtable, not a component. */
#define NAU8822_ADDR_LOW  0x1a
#define NAU8822_ADDR_HIGH 0x1b

typedef struct {
    uint8_t     first;         /* inclusive; first == last for a fixed address */
    uint8_t     last;
    const char *part;          /* the marking on the package, not prose */
    strres_id_t what;          /* one line of prose, so it lives on `res` */
    const char *group;         /* the console group that drives it */
    const char *verb;          /* its first useful command */
    bool        takes_address; /* whether that command takes the address */
} i2c_part_t;

/*
 * In address order, which is the order that reads well in `i2c identify` with
 * no argument. Matches for one address are re-ordered by range width when they
 * are printed -- see parts_for().
 */
static const i2c_part_t parts[] = {
    {NAU8822_ADDR_LOW, NAU8822_ADDR_HIGH,
     "NAU8822", STR_I2C_PART_NAU8822,
     "audio-nau8822", "init", true},

    {NAU7802_I2C_ADDRESS, NAU7802_I2C_ADDRESS,
     "NAU7802", STR_I2C_PART_NAU7802,
     "i2c-nau7802", "init", false},

    {RX8130CE_I2C_ADDR_DEFAULT, RX8130CE_I2C_ADDR_DEFAULT,
     "RX8130CE", STR_I2C_PART_RX8130CE,
     "i2c-rx8130ce", "time", false},

    {INA219_I2C_ADDR_DEFAULT, INA219_I2C_ADDR_DEFAULT + 0x0f,
     "INA219", STR_I2C_PART_INA219,
     "i2c-ina219", "read", true},

    {INA226_I2C_ADDR_DEFAULT, INA226_I2C_ADDR_DEFAULT + 0x0f,
     "INA226", STR_I2C_PART_INA226,
     "i2c-ina226", "read", true},

    {INA237_ADDR_FIRST, INA237_ADDR_LAST,
     "INA237", STR_I2C_PART_INA237,
     "i2c-ina237", "read", true},

    {PI4IOE5V6408_I2C_ADDR_DEFAULT, PI4IOE5V6408_I2C_ADDR_DEFAULT + 1,
     "PI4IOE5V6408", STR_I2C_PART_PI4IOE5V6408,
     "i2c-pi4ioe", "init", true},

    {SHT4X_ADDR_FIRST, SHT4X_ADDR_LAST,
     "SHT4x", STR_I2C_PART_SHT4X,
     "i2c-sht4x", "read", true},

    {LM75BDP_I2C_ADDR_DEFAULT, LM75BDP_I2C_ADDR_DEFAULT + 0x07,
     "LM75BDP", STR_I2C_PART_LM75BDP,
     "i2c-lm75bdp", "read", true},

    {AW9523B_I2C_ADDR_DEFAULT, AW9523B_I2C_ADDR_DEFAULT + 0x03,
     "AW9523B", STR_I2C_PART_AW9523B,
     "i2c-aw9523b", "init", true},
};

#define PART_COUNT (sizeof(parts) / sizeof(parts[0]))

/* Widths are the longest value each column can hold, so no row wraps: the
 * part names peak at PI4IOE5V6408, the descriptions at the SHT4x's variant
 * list, and the address column has to fit a range in `i2c identify`. */
#define ROW_FORMAT "%-9s %-13s %-41s %s\n"

void i2c_parts_print_header(void)
{
    diag_printf(ROW_FORMAT, "ADDRESS", "PART", "WHAT", "TRY");
}

/*
 * The parts covering `address`, tightest range first.
 *
 * Range width is the only ranking available without touching the bus, and it
 * is a real one: at 0x44 an SHT4x has three addresses to choose from and a
 * PI4IOE5V6408 two, while an INA has sixteen, so the narrower parts are the
 * stronger guesses and belong at the top of the list.
 */
static size_t parts_for(uint8_t address, const i2c_part_t **out)
{
    size_t count = 0;

    for (size_t i = 0; i < PART_COUNT; i++) {
        if (address < parts[i].first || address > parts[i].last) {
            continue;
        }

        /* Insertion sort; the table is ten rows and a match is at most a few. */
        int width = parts[i].last - parts[i].first;
        size_t at = count;
        while (at > 0 && (out[at - 1]->last - out[at - 1]->first) > width) {
            out[at] = out[at - 1];
            at--;
        }
        out[at] = &parts[i];
        count++;
    }

    return count;
}

static void print_row(const char *address, const i2c_part_t *part, const char *argument)
{
    char next[48];

    /* The command is always named in full, group and all: a suggestion the
     * operator cannot paste is worse than none. */
    if (part->takes_address) {
        snprintf(next, sizeof(next), "%s %s %s", part->group, part->verb, argument);
    } else {
        snprintf(next, sizeof(next), "%s %s", part->group, part->verb);
    }

    /* The description is a string resource, and the column is width-limited,
     * so it has to be a value here rather than something merely printed. */
    char what[APP_STR_LEN];
    diag_printf(ROW_FORMAT, address, part->part,
                app_str(part->what, what, sizeof(what)), next);
}

void i2c_parts_print_address(uint8_t address)
{
    const i2c_part_t *matches[PART_COUNT];
    size_t count = parts_for(address, matches);

    char label[8];
    snprintf(label, sizeof(label), "0x%02x", address);

    if (count == 0) {
        /* Still a row: the table accounts for every address the scan found,
         * and a raw read is the only thing left to try here. */
        char next[24];
        snprintf(next, sizeof(next), "i2c read %s", label);
        diag_printf(ROW_FORMAT, label, "-", "no driver in this firmware claims it", next);
        return;
    }

    for (size_t i = 0; i < count; i++) {
        print_row(label, matches[i], label);
    }
}

void i2c_parts_print_all(void)
{
    for (size_t i = 0; i < PART_COUNT; i++) {
        char label[12];

        if (parts[i].first == parts[i].last) {
            snprintf(label, sizeof(label), "0x%02x", parts[i].first);
        } else {
            snprintf(label, sizeof(label), "0x%02x-%02x", parts[i].first, parts[i].last);
        }

        print_row(label, &parts[i], "<address>");
    }
}
