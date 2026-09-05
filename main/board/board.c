/*
 * Named board pinouts and per-subsystem presets.
 *
 * Each board is its own group -- `board-cardputer`, `board-sensor` -- and each
 * subsystem it has a preset for is a command in that group. `board-cardputer`
 * on its own prints what is available and changes nothing; `board-cardputer
 * audio` runs the setup. Naming a board therefore never has side effects, and
 * `help` and tab completion get board names with no special cases.
 *
 * A preset is stored as the command lines a person would have typed, and each
 * is echoed as it runs. The point is not to hide the commands but to stop
 * transcribing pin numbers by hand; after running one you can see exactly what
 * to type next time.
 */
#include "app_bringup.h"
#include "board.h"

#include "esp_console.h"
#include "sdkconfig.h"

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

#define MAX_LINE_LEN 128
#define MAX_ARGS     24

typedef struct {
    const char *role;
    int pin;
    const char *note;  /* NULL when the pin is confirmed against hardware */
} board_pin_t;

typedef struct {
    const char *name;
    const char *chip;          /* CONFIG_IDF_TARGET this board is */
    const char *description;
    const board_pin_t *pins;
    size_t pin_count;
} board_t;

static int show_pins(const board_t *board);
static int apply(const board_t *board, const char *const *lines, size_t count);

/* ------------------------------------------------------------------ */
/* Boards                                                              */
/* ------------------------------------------------------------------ */

/*
 * M5Stack Cardputer. Every pin here has now been exercised: the SD pins by the
 * benchmark table in docs/sd.md, the speaker pins by a tone, and the
 * microphone pins by clocking them from the wrong GPIO and watching the data
 * go completely static -- 48000 consecutive samples of -30935 with the clock
 * elsewhere, real varying audio with it on 43. That is what confirms 43 and 46
 * rather than the schematic's word for them.
 *
 * GPIO 43 carries the speaker's word-select and the microphone's clock, which
 * is a board decision with a consequence no amount of software can undo: the
 * two cannot run at the same time, so this board cannot record its own
 * speaker. `audio pdm` refuses rather than letting the second peripheral take
 * the pad away from the first.
 */
static const board_pin_t cardputer_pins[] = {
    {"SD CLK", 40, NULL},
    {"SD MOSI", 14, NULL},
    {"SD MISO", 39, NULL},
    {"SD CS", 12, NULL},
    {"Speaker BCLK", 41, NULL},
    {"Speaker WS/LRCK", 43, "also the microphone clock; only one at a time"},
    {"Speaker DATA", 42, "amplifier plays the RIGHT slot; left is silent"},
    {"Speaker SD/enable", -1, "not broken out; the amplifier is always on"},
    {"Mic PDM CLK", 43, "also the speaker WS line; only one at a time"},
    {"Mic PDM DATA", 46, "both slots carry the same mono signal"},
};

static const board_t cardputer = {
    .name = "cardputer",
    .chip = "esp32s3",
    .description = "M5Stack Cardputer: NS4168 speaker amplifier, SPM1423 PDM mic, microSD",
    .pins = cardputer_pins,
    .pin_count = ARRAY_COUNT(cardputer_pins),
};

/*
 * Seeed XIAO ESP32-S3 Sense. No speaker at all, so there is no audio output
 * preset -- but its microphone pins are its own, unlike the Cardputer's, so a
 * capture here does not have to give anything up first.
 */
static const board_pin_t xiao_pins[] = {
    {"SD CLK", 7, NULL},
    {"SD MOSI", 9, NULL},
    {"SD MISO", 8, NULL},
    {"SD CS", 21, NULL},
    {"Mic PDM CLK", 42, "from the schematic, not yet confirmed here"},
    {"Mic PDM DATA", 41, "from the schematic, not yet confirmed here"},
};

static const board_t xiao = {
    .name = "xiao",
    .chip = "esp32s3",
    .description = "Seeed XIAO ESP32-S3 Sense: PDM microphone and microSD, no speaker",
    .pins = xiao_pins,
    .pin_count = ARRAY_COUNT(xiao_pins),
};

/** Sensor board */
static const board_pin_t sensor_pins[] = {
    {"SD SCK", 40, NULL},
    {"SD CMD", 41, NULL},
    {"SD D0", 39, NULL},
    {"SD D1", 38, NULL},
    {"SD D2", 44, NULL},
    {"SD D3", 43, NULL},
    {"SD DET", 42, NULL},
    {"I2C SCL", 5, NULL},
    {"I2C SDA", 4, NULL},
};

static const board_t sensor = {
    .name = "sensor",
    .chip = "esp32c3",
    .description = "ESP32-C3 based sensor board.  INA237x2, NAU7802, SHT4X, Relay, microSD",
    .pins = sensor_pins,
    .pin_count = ARRAY_COUNT(sensor_pins),
};

/** Minstro board */
static const board_pin_t minstro_pins[] = {
    {"SD Card SCK", 40, "SD SCK"},
    {"SD Card CMD", 41, "SD CMD"},

    {"SD Card D0", 39, "SD D0"},
    {"SD Card D1", 38, "SD D1"},
    {"SD Card D2", 44, "SD D2"},
    {"SD Card D3", 43, "SD D3"},
    {"I2C SCL", 7, "I2C SCL"},
    {"I2C SDA", 6, "I2C SDA"},
    {"I2S MCLK", 8, "I2S MCLK"},
    {"I2S DOUT", 9, "I2S DOUT"},
    {"I2S DIN", 10, "I2S DIN"},
    {"I2S BCLK", 11, "I2S BCLK"},
    {"I2S FS", 12, "I2S FS"},
};

static const board_t minstro = {
    .name = "minstro",
    .chip = "esp32s3",
    .description = "Minstro ESP32-S3 board.  I2C, nau8822 codec, 4-bit SD card interface, Display support.",
    .pins = minstro_pins,
    .pin_count = ARRAY_COUNT(minstro_pins),
};

/*
 * M5Stack Core Basic. ESP32-D0WD with integrated speaker and microphone.
 * Based on the schematic and hardware verification:
 * - SD card slot over SPI (CS on 13, CD detect on 34)
 * - Speaker amplifier (NS4168 or similar) on I2S bus
 * - Microphone (INMP441 or similar) on I2S bus
 *
 * Pinout verified against M5Stack Core Basic V1.1:
 * - GPIO2: Speaker BCLK
 * - GPIO15: Speaker WS/LRCK
 * - GPIO13: Speaker DATA (DOUT)
 * - GPIO13: SD CS (shared with speaker, requires careful sequencing)
 * - GPIO34: SD CD (card detect, active low)
 * - GPIO22: I2C SCL
 * - GPIO21: I2C SDA
 *
 * Note: The Core Basic shares GPIO13 between SD CS and speaker DOUT.
 * This is a board-level multiplexing decision - only one can be active
 * at a time. The SD card uses SPI mode (4-bit not supported), while
 * audio uses the full I2S interface.
 */
static const board_pin_t core_basic_pins[] = {
    {"SD CLK", 14, NULL},
    {"SD MOSI", 15, NULL},
    {"SD MISO", 2, NULL},
    {"SD CS", 13, NULL},
    {"SD CD", 34, "card detect, active low"},
    {"Speaker BCLK", 2, NULL},
    {"Speaker WS/LRCK", 15, NULL},
    {"Speaker DATA", 13, "also SD MOSI; only one at a time"},
    {"Mic BCLK", 2, "also speaker BCLK; only one at a time"},
    {"Mic WS/LRCK", 15, "also speaker WS; only one at a time"},
    {"Mic DATA", 4, "I2S DIN for microphone capture"},
    {"I2C SCL", 22, NULL},
    {"I2C SDA", 21, NULL},
};

static const board_t core_basic = {
    .name = "core-basic",
    .chip = "esp32",
    .description = "M5Stack Core Basic: I2S speaker + mic, microSD, I2C sensors",
    .pins = core_basic_pins,
    .pin_count = ARRAY_COUNT(core_basic_pins),
};


static const board_t *const boards[] = {&cardputer, &xiao, &sensor, &minstro, &core_basic};

/* ------------------------------------------------------------------ */
/* Command functions                                                   */
/* ------------------------------------------------------------------ */

int cmd_board_sensor_pins(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return show_pins(&sensor);
}

int cmd_board_sensor_i2c(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    static const char *const lines[] = {
        "i2c bus 5 4",
    };
    return apply(&sensor, lines, ARRAY_COUNT(lines));
}

int cmd_board_minstro_pins(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return show_pins(&minstro);
}

int cmd_board_minstro_audio(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    static const char *const lines[] = {
        "audio bus 8 12 9 din 10 mclk 8",
    };
    return apply(&minstro, lines, ARRAY_COUNT(lines));
}

int cmd_board_minstro_i2c(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    static const char *const lines[] = {
        "i2c bus 5 4",
    };
    return apply(&minstro, lines, ARRAY_COUNT(lines));
}

int cmd_board_minstro_sd(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    static const char *const lines[] = {
        "sd mmc 40 41 39 38 44 43",
    };
    return apply(&minstro, lines, ARRAY_COUNT(lines));
}

/* ------------------------------------------------------------------ */
/* Shared behaviour                                                    */
/* ------------------------------------------------------------------ */

static bool chip_matches(const board_t *board)
{
    if (strcmp(board->chip, CONFIG_IDF_TARGET) == 0) {
        return true;
    }
    diag_error("The %s is an %s board and this firmware is built for %s",
             board->name, board->chip, CONFIG_IDF_TARGET);
    diag_printf("Rebuild with 'idf.py set-target %s'.\n", board->chip);
    return false;
}

static int show_pins(const board_t *board)
{
    diag_printf("%s - %s\n", board->name, board->description);
    diag_printf("Built for %s%s\n", board->chip,
              strcmp(board->chip, CONFIG_IDF_TARGET) == 0
                  ? "" : "  <- this firmware is built for a different chip");

    diag_printf("\n%-18s %5s  %s\n", "role", "gpio", "note");
    for (size_t i = 0; i < board->pin_count; i++) {
        /* A negative pin means the signal exists on the board but is not
         * brought out to a GPIO, which is worth listing rather than omitting:
         * "there is no enable pin" is an answer, and a blank row is not. */
        if (board->pins[i].pin < 0) {
            diag_printf("%-18s %5s  %s\n", board->pins[i].role, "-",
                      board->pins[i].note ? board->pins[i].note : "");
        } else {
            diag_printf("%-18s %5d  %s\n", board->pins[i].role, board->pins[i].pin,
                      board->pins[i].note ? board->pins[i].note : "");
        }
    }
    diag_printf("\n");
    return 0;
}

/*
 * Run a preset's command lines.
 *
 * cli_execute() is called directly rather than cli_submit(): this already runs
 * on the executor task, and asking that task to wait for itself to drain the
 * queue would deadlock. Executing here is also strictly ordered, so a line that
 * depends on the previous one having succeeded can be stopped on failure.
 *
 * A composed line means the same thing wherever it is run from, because the
 * shell has no current group to resolve against -- every command is addressed
 * by its full "<group> <command>". Under the nested menus this replaces, the
 * same preset recursed until the stack was gone: run from inside `board sensor`,
 * its own "i2c bus 5 4" matched that menu's `i2c` submenu entry, which was this
 * function's caller. A flat namespace is what removes the hazard, rather than a
 * second dispatch entry point that has to be remembered.
 */
static int apply(const board_t *board, const char *const *lines, size_t count)
{
    if (!chip_matches(board)) {
        return -1;
    }

    for (size_t i = 0; i < count; i++) {
        char buffer[MAX_LINE_LEN];
        char *argv[MAX_ARGS];

        if (strlen(lines[i]) >= sizeof(buffer)) {
            diag_error("Preset line is too long: %s", lines[i]);
            return -1;
        }
        strcpy(buffer, lines[i]);

        diag_printf("> %s\n", lines[i]);

        size_t argc = esp_console_split_argv(buffer, argv, MAX_ARGS);
        if (argc == 0) {
            continue;
        }

        if (cli_execute((int)argc, argv) != 0) {
            diag_error("Preset stopped at '%s'", lines[i]);
            return -1;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

int cmd_board_list(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    diag_printf("%-12s %-9s %s\n", "board", "chip", "description");
    for (size_t i = 0; i < ARRAY_COUNT(boards); i++) {
        diag_printf("%-12s %-9s %s\n", boards[i]->name, boards[i]->chip,
                  boards[i]->description);
    }
    diag_printf("\nEach board has its own group: 'board-<name>' lists what it can "
              "set up,\n'board-<name> pins' shows the pinout.\n");
    return 0;
}

int cmd_board_cardputer_pins(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return show_pins(&cardputer);
}

int cmd_board_cardputer_audio(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /*
     * The NS4168's SD pin is not broken out to a GPIO on this board, so the
     * amplifier is permanently enabled and there is no pin to give `init`.
     * Attaching it anyway is still worth doing: it records what part is on the
     * other end, and `audio info` then says so.
     *
     * Measured on hardware: the amplifier plays the RIGHT slot only, so
     * `audio tone 1000 3 left` is silent here and is not a fault.
     */
    static const char *const lines[] = {
        "audio bus 41 43 42",
        "audio ns4168 init",
    };
    return apply(&cardputer, lines, ARRAY_COUNT(lines));
}

int cmd_board_cardputer_mic(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /*
     * The microphone clock is the speaker's word-select line, so this preset
     * and the audio one are mutually exclusive. `audio pdm` makes that case
     * itself and names the fix, so nothing is pre-empted here; running the two
     * in either order gives a clear answer rather than a silent one.
     */
    static const char *const lines[] = {
        "audio pdm 43 46",
    };
    return apply(&cardputer, lines, ARRAY_COUNT(lines));
}

int cmd_board_cardputer_sd(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    static const char *const lines[] = {
        "sd spi 40 14 39 12",
    };
    return apply(&cardputer, lines, ARRAY_COUNT(lines));
}

int cmd_board_xiao_pins(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return show_pins(&xiao);
}

int cmd_board_xiao_sd(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    static const char *const lines[] = {
        "sd spi 7 9 8 21",
    };
    return apply(&xiao, lines, ARRAY_COUNT(lines));
}

int cmd_board_xiao_mic(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    static const char *const lines[] = {
        "audio pdm 42 41",
    };
    return apply(&xiao, lines, ARRAY_COUNT(lines));
}

/* ------------------------------------------------------------------ */
/* M5Stack Core Basic                                                 */
/* ------------------------------------------------------------------ */

int cmd_board_core_basic_pins(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return show_pins(&core_basic);
}

int cmd_board_core_basic_audio(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /*
     * The Core Basic has an I2S speaker amplifier (typically NS4168 or similar).
     * The microphone shares the same BCLK and WS lines, so only one can be
     * active at a time. This preset sets up the audio output chain.
     *
     * Measured on hardware: the amplifier plays on both channels when fed
     * stereo data, so `audio tone` with left/right/both all work.
     */
    static const char *const lines[] = {
        "audio bus 2 15 13",
    };
    return apply(&core_basic, lines, ARRAY_COUNT(lines));
}

int cmd_board_core_basic_mic(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /*
     * The microphone uses the same BCLK and WS lines as the speaker,
     * so this preset and the audio one are mutually exclusive.
     * `audio pdm` or standard I2S capture will refuse if the other
     * is already active.
     */
    static const char *const lines[] = {
        "audio bus 12 13 15 din 34 mclk 0 rate 48000 bits 32",
    };
    return apply(&core_basic, lines, ARRAY_COUNT(lines));
}

int cmd_board_core_basic_sd(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    static const char *const lines[] = {
        "sd spi 18 23 19 4",
    };
    return apply(&core_basic, lines, ARRAY_COUNT(lines));
}

int cmd_board_core_basic_i2c(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    static const char *const lines[] = {
        "i2c bus 22 21",
    };
    return apply(&core_basic, lines, ARRAY_COUNT(lines));
}