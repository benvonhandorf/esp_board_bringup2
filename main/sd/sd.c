/*
 * SD/MMC card bring-up and throughput measurement.
 *
 * A card can be reached three ways, and which one a board wired up is exactly
 * the sort of thing bring-up needs to establish: over SPI (the only option on
 * chips without an SD host peripheral, such as the ESP32-C3), or over the
 * dedicated SD host in 1-bit or 4-bit mode.
 *
 * Bring-up happens in two stages rather than through esp_vfs_fat_sdmmc_mount().
 * Those one-shot helpers do host init, card init and the FAT mount together and
 * unwind all of it if the mount fails -- which is the wrong behaviour here,
 * because a blank or non-FAT card should still report its identity and still be
 * measurable. So `sd spi` / `sd mmc` bring the interface up and initialize the
 * card, and FAT is only mounted when `sd bench` actually needs a filesystem.
 */
#include "app_bringup.h"
#include "display.h"
#include "sd.h"
#include "spi.h"
#include "sys_hw.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <unistd.h>

#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sd_protocol_defs.h"
#include "sdmmc_cmd.h"
#include "soc/soc_caps.h"

#if SOC_SDMMC_HOST_SUPPORTED
#include "driver/sdmmc_default_configs.h"
#include "driver/sdmmc_host.h"
#endif

#define SD_MOUNT_POINT "/sd"
#define SD_BENCH_FILE SD_MOUNT_POINT "/bench.tmp"
/* 8.3, so it works with the default CONFIG_FATFS_LFN_NONE. */
#define SD_RESULTS_FILE SD_MOUNT_POINT "/sdbench.txt"
#define SD_MAX_OPEN_FILES 4

#define DEFAULT_SIZE_KB 512
#define DEFAULT_BLOCK_KB 16
#define MAX_SIZE_KB 16384
#define MAX_BLOCK_KB 128

/* Below SDMMC_FREQ_PROBING the host cannot divide down far enough. */
#define MIN_FREQ_KHZ 400
#define MAX_FREQ_KHZ 80000

typedef enum {
    IFACE_NONE,
    IFACE_SPI,
    IFACE_MMC,
} sd_iface_t;

/*
 * What was asked for. This deliberately survives teardown(), so the card can be
 * torn down and reopened at a different clock -- which is what `sd sweep` does
 * on every step -- without the caller re-entering the pins.
 */
static struct {
    sd_iface_t iface;
    int pins[6];
    int pin_count;
    int width;                          /* 1 or 4, for IFACE_MMC */
    int freq_khz;
    int cd_pin;                         /* GPIO_NUM_NC when card detect is unused */
} cfg;

/*
 * What the hardware currently has open, which is not the same thing: it is
 * IFACE_NONE between a teardown and the next open, and it is what decides which
 * host teardown() has to release.
 */
static sd_iface_t open_iface;
static sdmmc_card_t *card;
static sdmmc_host_t host;
static bool host_inited;
static bool bus_owned;                  /* we called spi_bus_initialize() */
static sdspi_dev_handle_t spi_dev = -1;
static bool mounted;
static BYTE fat_pdrv = 0xFF;

bool sd_owns_spi_host(void)
{
    return bus_owned;
}

static bool require_card(void)
{
    if (!card) {
        STRRES_ERROR(STR_SD_NOT_INITIALIZED);
        return false;
    }
    return true;
}

static const char *iface_name(void)
{
    switch (cfg.iface) {
    case IFACE_SPI:
        return "SPI";
    case IFACE_MMC:
        return cfg.width == 4 ? "SD 4-bit" : "SD 1-bit";
    default:
        return "none";
    }
}

/* ------------------------------------------------------------------ */
/* Teardown                                                            */
/* ------------------------------------------------------------------ */

static void unmount_fat(void)
{
    if (!mounted) {
        return;
    }
    const char drive[3] = {(char)('0' + fat_pdrv), ':', '\0'};
    f_mount(NULL, drive, 0);
    esp_vfs_fat_unregister_path(SD_MOUNT_POINT);
    ff_diskio_unregister(fat_pdrv);
    fat_pdrv = 0xFF;
    mounted = false;
}

/*
 * Release the hardware, in the reverse of the order it was acquired, leaving
 * `cfg` alone. Called by `sd close`, at the top of `sd spi` / `sd mmc` so those
 * can be re-run with different pins the way `spi bus` and `i2c bus` can, on
 * every step of `sd sweep`, and on every init failure so a failed attempt never
 * leaves half-configured hardware behind.
 */
static void teardown(void)
{
    unmount_fat();

    free(card);
    card = NULL;

    if (open_iface == IFACE_SPI) {
        if (spi_dev >= 0) {
            sdspi_host_remove_device(spi_dev);
            spi_dev = -1;
        }
        if (host_inited) {
            sdspi_host_deinit();
        }
    }
#if SOC_SDMMC_HOST_SUPPORTED
    else if (open_iface == IFACE_MMC) {
        if (host_inited) {
            sdmmc_host_deinit();
        }
    }
#endif
    host_inited = false;

    if (bus_owned) {
        spi_bus_free(BP_SPI_HOST_ID);
        bus_owned = false;
    }

    open_iface = IFACE_NONE;
}

/* ------------------------------------------------------------------ */
/* Argument helpers                                                    */
/* ------------------------------------------------------------------ */

/*
 * Consume an optional trailing "khz <freq>" starting at `index`, which must be
 * the end of the argument list.
 *
 * The frequency is a keyword pair rather than a bare trailing number because
 * `sd mmc` takes a variable number of pins (three for 1-bit, six for 4-bit): a
 * bare number could not be told apart from another pin. This mirrors the
 * `nau7802 init ldo 3.0` idiom.
 */
static int take_options(int argc, char **argv, int index, int *freq_khz, int *cd_pin)
{
    while (index < argc) {
        if (index + 1 >= argc) {
            STRRES_ERROR(STR_SD_OPTION_NEEDS_VALUE, argv[index]);
            return -1;
        }
        if (strcasecmp(argv[index], "khz") == 0) {
            if (cli_parse_int_arg(argv[index + 1], freq_khz) < 0 ||
                *freq_khz < MIN_FREQ_KHZ || *freq_khz > MAX_FREQ_KHZ) {
                STRRES_ERROR(STR_SD_FREQUENCY_RANGE, MIN_FREQ_KHZ, MAX_FREQ_KHZ);
                return -1;
            }
        } else if (strcasecmp(argv[index], "cd") == 0) {
            if (cli_parse_int_arg(argv[index + 1], cd_pin) < 0 ||
                !GPIO_IS_VALID_GPIO(*cd_pin)) {
                STRRES_ERROR(STR_SD_CD_INVALID, argv[index + 1]);
                return -1;
            }
        } else {
            STRRES_ERROR(STR_SD_UNEXPECTED_ARGUMENT, argv[index]);
            return -1;
        }
        index += 2;
    }
    return 0;
}

/* Every SD line is bidirectional, so all of them must be output-capable. */
static bool check_pins(const int *pins, int count, const char *const *names)
{
    for (int i = 0; i < count; i++) {
        if (!GPIO_IS_VALID_OUTPUT_GPIO(pins[i])) {
            STRRES_ERROR(STR_SD_PIN_NOT_OUTPUT_CAPABLE, names[i], pins[i]);
            return false;
        }
    }
    return true;
}

/*
 * Parse the optional [size_kb] [block_kb] pair shared by `bench` and `raw`.
 * Returns 0 on success, -1 on a bad value.
 */
static int take_sizes(int argc, char **argv, int first, size_t *total_bytes, size_t *block_bytes)
{
    int size_kb = DEFAULT_SIZE_KB;
    int block_kb = DEFAULT_BLOCK_KB;

    if (argc > first && cli_parse_int_arg(argv[first], &size_kb) < 0) {
        STRRES_ERROR(STR_SD_TRANSFER_SIZE_WHOLE_KB);
        return -1;
    }
    if (argc > first + 1 && cli_parse_int_arg(argv[first + 1], &block_kb) < 0) {
        STRRES_ERROR(STR_SD_BLOCK_SIZE_WHOLE_KB);
        return -1;
    }
    if (size_kb < 1 || size_kb > MAX_SIZE_KB) {
        STRRES_ERROR(STR_SD_TRANSFER_SIZE_RANGE, MAX_SIZE_KB);
        return -1;
    }
    if (block_kb < 1 || block_kb > MAX_BLOCK_KB) {
        STRRES_ERROR(STR_SD_BLOCK_SIZE_RANGE, MAX_BLOCK_KB);
        return -1;
    }
    if (block_kb > size_kb) {
        block_kb = size_kb;
    }

    *block_bytes = (size_t)block_kb * 1024;
    /* Round the total down to a whole number of blocks so the maths is exact. */
    *total_bytes = ((size_t)size_kb * 1024 / *block_bytes) * *block_bytes;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Card information                                                    */
/* ------------------------------------------------------------------ */

static const char *card_type(void)
{
    if (card->is_sdio) {
        return "SDIO";
    }
    if (card->is_mmc) {
        return "MMC";
    }
    if ((card->ocr & SD_OCR_SDHC_CAP) == 0) {
        return "SDSC";
    }
    return (card->ocr & SD_OCR_S18_RA) ? "SDHC/SDXC (UHS-I)" : "SDHC/SDXC";
}

int cmd_sd_info(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!require_card()) {
        return -1;
    }

    STRRES_PRINTF(STR_SD_INFO_INTERFACE, iface_name());
    for (int i = 0; i < cfg.pin_count; i++) {
        diag_printf("%s%d", i ? "," : "", cfg.pins[i]);
    }
    diag_printf("\n");

    if (cfg.cd_pin != GPIO_NUM_NC) {
        /* Active low: the switch closes to ground when a card is seated. */
        if (gpio_get_level(cfg.cd_pin)) {
            STRRES_PRINTF(STR_SD_INFO_DETECT_EMPTY, cfg.cd_pin);
        } else {
            STRRES_PRINTF(STR_SD_INFO_DETECT_PRESENT, cfg.cd_pin);
        }
    }
    STRRES_PRINTF(STR_SD_INFO_TYPE, card_type());
    STRRES_PRINTF(STR_SD_INFO_NAME, card->cid.name);

    /*
     * The SD CID packs the manufacture date into 12 bits as
     * (year - 2000) << 4 | month. MMC uses a different epoch, so only decode
     * the date for SD cards rather than print something plausible but wrong.
     */
    if (!card->is_mmc) {
        STRRES_PRINTF(STR_SD_INFO_CID_SD,
                      (unsigned)card->cid.mfg_id, (unsigned)card->cid.oem_id,
                      (card->cid.revision >> 4) & 0xF, card->cid.revision & 0xF,
                      (unsigned)card->cid.serial, 2000 + (card->cid.date >> 4),
                      card->cid.date & 0xF);
    } else {
        STRRES_PRINTF(STR_SD_INFO_CID_MMC,
                      (unsigned)card->cid.mfg_id, (unsigned)card->cid.oem_id,
                      card->cid.revision, (unsigned)card->cid.serial);
    }

    uint64_t bytes = (uint64_t)card->csd.capacity * (uint64_t)card->csd.sector_size;
    STRRES_PRINTF(STR_SD_INFO_CAPACITY,
                  (double)bytes / (1024.0 * 1024.0 * 1024.0), (unsigned long long)bytes,
                  card->csd.capacity, card->csd.sector_size);
    STRRES_PRINTF(STR_SD_INFO_CSD,
                  card->is_mmc ? card->csd.csd_ver : card->csd.csd_ver + 1,
                  card->csd.read_block_len, (unsigned)card->csd.card_command_class);

    /*
     * real_freq_khz is what the host divider actually produced, which is not
     * what was asked for: the divider quantizes. max_freq_khz is the ceiling
     * the card itself reports, so the pair together says whether the interface
     * or the card is the limit.
     */
    if (card->real_freq_khz == 0) {
        STRRES_PRINTF(STR_SD_INFO_CLOCK_UNKNOWN);
    } else {
        STRRES_PRINTF(STR_SD_INFO_CLOCK,
                      card->real_freq_khz / 1000.0, cfg.freq_khz, card->max_freq_khz / 1000.0,
                      card->is_ddr ? ", DDR" : "", card->is_uhs1 ? ", UHS-I" : "");
        /*
         * The CSD speed is what the card advertises after any high-speed switch,
         * and is therefore the line between running it in spec and overclocking
         * it. `sd sweep` finds out what it will actually tolerate.
         */
        if (card->real_freq_khz * 1000 > card->csd.tr_speed) {
            STRRES_PRINTF(STR_SD_INFO_CARD_RATED_OVER, card->csd.tr_speed / 1e6);
        } else {
            STRRES_PRINTF(STR_SD_INFO_CARD_RATED, card->csd.tr_speed / 1e6);
        }
    }

    STRRES_PRINTF(STR_SD_INFO_BUS_WIDTH, 1 << card->log_bus_width);
    if (!card->is_mmc && !card->is_sdio) {
        STRRES_PRINTF(STR_SD_INFO_SCR,
                      card->scr.sd_spec, (card->scr.bus_width & BIT(0)) ? " 1-bit" : "",
                      (card->scr.bus_width & BIT(2)) ? " 4-bit" : "");
        STRRES_PRINTF(STR_SD_INFO_SSR, card->ssr.cur_bus_width ? 4 : 1,
                      (unsigned long)card->ssr.alloc_unit_kb);
    }

    if (mounted) {
        uint64_t total = 0, freespace = 0;
        if (esp_vfs_fat_info(SD_MOUNT_POINT, &total, &freespace) == ESP_OK) {
            STRRES_PRINTF(STR_SD_INFO_FAT_MOUNTED,
                          SD_MOUNT_POINT, (unsigned long long)(total / (1024 * 1024)),
                          (unsigned long long)(freespace / (1024 * 1024)));
        }
    } else {
        STRRES_PRINTF(STR_SD_INFO_FAT_ABSENT);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Bring-up                                                            */
/* ------------------------------------------------------------------ */

static esp_err_t open_spi(void)
{
    const spi_bus_config_t bus_config = {
        .sclk_io_num = cfg.pins[0],
        .mosi_io_num = cfg.pins[1],
        .miso_io_num = cfg.pins[2],
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = MAX_BLOCK_KB * 1024,
    };

    esp_err_t err = spi_bus_initialize(BP_SPI_HOST_ID, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        return err;
    }
    bus_owned = true;

    err = sdspi_host_init();
    if (err != ESP_OK) {
        return err;
    }
    host_inited = true;

    sdspi_device_config_t dev_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev_config.host_id = BP_SPI_HOST_ID;
    dev_config.gpio_cs = cfg.pins[3];
    dev_config.gpio_cd = cfg.cd_pin;

    err = sdspi_host_init_device(&dev_config, &spi_dev);
    if (err != ESP_OK) {
        return err;
    }

    host = (sdmmc_host_t)SDSPI_HOST_DEFAULT();
    host.slot = spi_dev;
    host.max_freq_khz = cfg.freq_khz;
    return ESP_OK;
}

#if SOC_SDMMC_HOST_SUPPORTED
static esp_err_t open_mmc(void)
{
    host = (sdmmc_host_t)SDMMC_HOST_DEFAULT();
    host.max_freq_khz = cfg.freq_khz;
    host.slot = SDMMC_HOST_SLOT_1;

    /*
     * The bus width is driven by the slot, not by host.flags: sdmmc_fix_host_flags()
     * reads the slot width back and narrows the host flags to match, so setting
     * .width alone is what actually keeps a 1-bit slot from being switched to
     * 4-bit during initialization.
     */
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = cfg.pins[0];
    slot_config.cmd = cfg.pins[1];
    slot_config.d0 = cfg.pins[2];
    if (cfg.width == 4) {
        slot_config.d1 = cfg.pins[3];
        slot_config.d2 = cfg.pins[4];
        slot_config.d3 = cfg.pins[5];
    }
    slot_config.cd = cfg.cd_pin;    /* SDMMC_SLOT_NO_CD unless 'cd <pin>' was given */
    slot_config.wp = SDMMC_SLOT_NO_WP;
    slot_config.width = (uint8_t)cfg.width;
    /* Boards being brought up frequently lack the external pull-ups the bus
     * needs; the internal ones are weak but usually enough to get a card to
     * answer, which is the point of the exercise. */
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = sdmmc_host_init();
    if (err != ESP_OK) {
        return err;
    }
    host_inited = true;

    return sdmmc_host_init_slot(host.slot, &slot_config);
}
#endif

/*
 * Bring the hardware up from `cfg` and probe the card. Quiet on success: the
 * sweep calls this once per clock step and does its own reporting.
 *
 * Always leaves the hardware released on failure, so the caller can go straight
 * on to the next attempt.
 */
static esp_err_t open_card(void)
{
    teardown();

    esp_err_t err;
    if (cfg.iface == IFACE_SPI) {
        open_iface = IFACE_SPI;
        err = open_spi();
    }
#if SOC_SDMMC_HOST_SUPPORTED
    else if (cfg.iface == IFACE_MMC) {
        open_iface = IFACE_MMC;
        err = open_mmc();
    }
#endif
    else {
        return ESP_ERR_INVALID_STATE;
    }

    if (err != ESP_OK) {
        teardown();
        return err;
    }

    card = calloc(1, sizeof(sdmmc_card_t));
    if (!card) {
        teardown();
        return ESP_ERR_NO_MEM;
    }

    err = sdmmc_card_init(&host, card);
    if (err != ESP_OK) {
        teardown();
        return err;
    }
    return ESP_OK;
}

/*
 * A timeout out of card init can come from two very different places, and the
 * error code alone does not say which -- an earlier version of this hint
 * asserted the card had failed ACMD41 and sent a real diagnosis down the wrong
 * path when the host had actually stalled before sending anything.
 */
static void explain_init_timeout(esp_err_t err)
{
    if (err != ESP_ERR_TIMEOUT) {
        return;
    }
    STRRES_PRINTF(STR_SD_HINT_TIMEOUT);
    STRRES_PRINTF(STR_SD_HINT_CLOCK_UPDATE);
    STRRES_PRINTF(STR_SD_HINT_CLOCK_UPDATE_2);
    STRRES_PRINTF(STR_SD_HINT_SEND_OP_COND);
    STRRES_PRINTF(STR_SD_HINT_SEND_OP_COND_2);
    STRRES_PRINTF(STR_SD_HINT_SEND_OP_COND_3);
    STRRES_PRINTF(STR_SD_HINT_SEE_DOCS);
}

/*
 * Warn about lines that are already held low before the SD host is handed the
 * pins.
 *
 * CMD and the data lines idle high through pull-ups on any working SD wiring.
 * One stuck low makes the host's clock-update command wait forever for an idle
 * bus, which surfaces as an unhelpful timeout deep in the driver. Checking first
 * turns that into a specific, actionable line. Only a warning: the levels are a
 * strong hint, not proof, and the real attempt may still be informative.
 */
static void warn_if_bus_held_low(const int *pins, int count, const char *const *names)
{
    /* Index 0 is CLK, which the host drives; the rest should idle high. */
    for (int i = 1; i < count; i++) {
        const gpio_config_t probe = {
            .pin_bit_mask = BIT64(pins[i]),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        if (gpio_config(&probe) != ESP_OK) {
            continue;
        }
        if (gpio_get_level(pins[i]) == 0) {
            STRRES_PRINTF(STR_SD_WARN_LINE_LOW, names[i], pins[i]);
        }
    }

    /*
     * Then look for a short between CLK and a data line.
     *
     * That one does not show up above: at rest every line floats high through
     * its pull-up and looks fine. It only bites once the host starts clocking,
     * when every CLK low pulls the shorted data line down with it, the bus never
     * reads idle, and the clock-update command hangs. Driving CLK low for a
     * moment and seeing which lines follow reproduces it in microseconds. CLK is
     * a host output, so driving it here costs nothing.
     */
    const gpio_config_t clk_out = {
        .pin_bit_mask = BIT64(pins[0]),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&clk_out) != ESP_OK) {
        return;
    }
    gpio_set_level(pins[0], 0);

    for (int i = 1; i < count; i++) {
        if (gpio_get_level(pins[i]) == 0) {
            STRRES_PRINTF(STR_SD_WARN_LINE_SHORTED, names[i], pins[i], names[0], pins[0]);
        }
    }
    gpio_set_level(pins[0], 1);
}

/*
 * ESP-IDF maps these three exact frequencies onto the UHS-I modes and rejects
 * them outright on a host without UHS support. Asking for 50000 kHz on an
 * ESP32-S3 therefore fails with a bare ESP_ERR_NOT_SUPPORTED that says nothing
 * about why, so say it here instead.
 */
static bool frequency_is_reserved(int freq_khz)
{
#if SOC_SDMMC_UHS_I_SUPPORTED
    (void)freq_khz;
    return false;
#else
    if (freq_khz == SDMMC_FREQ_DDR50 || freq_khz == SDMMC_FREQ_SDR50 ||
        freq_khz == SDMMC_FREQ_SDR104) {
        STRRES_ERROR(STR_SD_UHS_UNSUPPORTED, freq_khz, CONFIG_IDF_TARGET, freq_khz - 1000);
        return true;
    }
    return false;
#endif
}

int cmd_sd_spi(int argc, char **argv)
{
    if (argc < 5) {
        STRRES_PRINTF(STR_SD_USAGE_SPI);
        return -1;
    }

    int pins[4];
    static const char *const names[4] = {"CLK", "MOSI", "MISO", "CS"};
    for (int i = 0; i < 4; i++) {
        if (cli_parse_int_arg(argv[i + 1], &pins[i]) < 0) {
            STRRES_ERROR(STR_SD_PIN_NOT_A_NUMBER, names[i]);
            return -1;
        }
    }

    int freq_khz = SDMMC_FREQ_DEFAULT;
    int cd_pin = GPIO_NUM_NC;
    if (take_options(argc, argv, 5, &freq_khz, &cd_pin) < 0) {
        return -1;
    }
    if (!check_pins(pins, 4, names)) {
        return -1;
    }
    warn_if_bus_held_low(pins, 3, names);   /* CLK, MOSI, MISO -- CS is ours to drive */
    if (frequency_is_reserved(freq_khz)) {
        return -1;
    }

    if (spi_group_owns_host()) {
        STRRES_ERROR(STR_SD_SPI_HOST_BUSY);
        return -1;
    }
    if (display_owns_spi_host(BP_SPI_HOST_ID)) {
        STRRES_ERROR(STR_SD_SPI_HOST_DISPLAY);
        return -1;
    }

    teardown();
    cfg.iface = IFACE_SPI;
    memcpy(cfg.pins, pins, sizeof(pins));
    cfg.pin_count = 4;
    cfg.width = 1;
    cfg.freq_khz = freq_khz;
    cfg.cd_pin = cd_pin;

    esp_err_t err = open_card();
    if (err != ESP_OK) {
        STRRES_ERROR(STR_SD_SPI_INIT_FAILED,
                     freq_khz, esp_err_to_name(err));
        explain_init_timeout(err);
        return -1;
    }

    STRRES_PRINTF(STR_SD_SPI_UP, card->real_freq_khz / 1000.0);
    return cmd_sd_info(0, NULL);
}

#if SOC_SDMMC_HOST_SUPPORTED
int cmd_sd_mmc(int argc, char **argv)
{
    static const char *const names[6] = {"CLK", "CMD", "D0", "D1", "D2", "D3"};

    int pins[6];
    int pin_count = 0;
    int index = 1;
    while (index < argc && pin_count < 6 &&
           strcasecmp(argv[index], "khz") != 0 && strcasecmp(argv[index], "cd") != 0) {
        if (cli_parse_int_arg(argv[index], &pins[pin_count]) < 0) {
            STRRES_ERROR(STR_SD_PIN_NOT_A_NUMBER_NAMED, names[pin_count], argv[index]);
            return -1;
        }
        pin_count++;
        index++;
    }

    if (pin_count != 3 && pin_count != 6) {
        STRRES_PRINTF(STR_SD_USAGE_MMC);
        STRRES_ERROR(STR_SD_MMC_PIN_COUNT);
        return -1;
    }

    int freq_khz = SDMMC_FREQ_DEFAULT;
    int cd_pin = GPIO_NUM_NC;
    if (take_options(argc, argv, index, &freq_khz, &cd_pin) < 0) {
        return -1;
    }
    if (!check_pins(pins, pin_count, names)) {
        return -1;
    }
    if (frequency_is_reserved(freq_khz)) {
        return -1;
    }
    warn_if_bus_held_low(pins, pin_count, names);

    teardown();
    cfg.iface = IFACE_MMC;
    memcpy(cfg.pins, pins, sizeof(int) * (size_t)pin_count);
    cfg.pin_count = pin_count;
    cfg.width = (pin_count == 6) ? 4 : 1;
    cfg.freq_khz = freq_khz;
    cfg.cd_pin = cd_pin;

    esp_err_t err = open_card();
    if (err != ESP_OK) {
        STRRES_ERROR(STR_SD_MMC_INIT_FAILED,
                     iface_name(), freq_khz, esp_err_to_name(err));
        explain_init_timeout(err);
        return -1;
    }

    STRRES_PRINTF(STR_SD_MMC_UP, iface_name(), card->real_freq_khz / 1000.0);
    return cmd_sd_info(0, NULL);
}
#else  /* !SOC_SDMMC_HOST_SUPPORTED */
int cmd_sd_mmc(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    STRRES_ERROR(STR_SD_NO_SD_HOST, CONFIG_IDF_TARGET);
    return -1;
}
#endif

int cmd_sd_close(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (cfg.iface == IFACE_NONE) {
        STRRES_PRINTF(STR_SD_NOTHING_INITIALIZED);
        return 0;
    }
    teardown();
    cfg.iface = IFACE_NONE;
    cfg.pin_count = 0;
    cfg.cd_pin = GPIO_NUM_NC;
    STRRES_PRINTF(STR_SD_RELEASED);
    return 0;
}

/* ------------------------------------------------------------------ */
/* FAT mount                                                           */
/* ------------------------------------------------------------------ */

/*
 * `quiet` suppresses the diag_error() reporting for callers where a missing
 * filesystem is not a command failure -- saving the results file is best effort,
 * and an ERR: line there would tell a host script the benchmark failed when it
 * did not.
 */
static bool mount_fat(bool quiet)
{
    if (mounted) {
        return true;
    }

    esp_err_t err = ff_diskio_get_drive(&fat_pdrv);
    if (err != ESP_OK || fat_pdrv == 0xFF) {
        if (!quiet) {
            STRRES_ERROR(STR_SD_NO_DRIVE_SLOT, esp_err_to_name(err));
        }
        fat_pdrv = 0xFF;
        return false;
    }
    ff_diskio_register_sdmmc(fat_pdrv, card);

    const char drive[3] = {(char)('0' + fat_pdrv), ':', '\0'};
    const esp_vfs_fat_conf_t conf = {
        .base_path = SD_MOUNT_POINT,
        .fat_drive = drive,
        .max_files = SD_MAX_OPEN_FILES,
    };

    FATFS *fs = NULL;
    err = esp_vfs_fat_register(&conf, &fs);
    if (err != ESP_OK) {
        if (!quiet) {
            STRRES_ERROR(STR_SD_VFS_REGISTER_FAILED, SD_MOUNT_POINT, esp_err_to_name(err));
        }
        ff_diskio_unregister(fat_pdrv);
        fat_pdrv = 0xFF;
        return false;
    }

    FRESULT res = f_mount(fs, drive, 1);
    if (res != FR_OK) {
        esp_vfs_fat_unregister_path(SD_MOUNT_POINT);
        ff_diskio_unregister(fat_pdrv);
        fat_pdrv = 0xFF;
        if (quiet) {
            /* nothing: the caller reports it in its own terms */
        } else if (res == FR_NO_FILESYSTEM) {
            STRRES_ERROR(STR_SD_NO_FILESYSTEM);
        } else {
            STRRES_ERROR(STR_SD_MOUNT_FAILED, res);
        }
        return false;
    }

    mounted = true;
    return true;
}

/* ------------------------------------------------------------------ */
/* Results file                                                        */
/* ------------------------------------------------------------------ */

/*
 * A growable text buffer holding the report as it is produced.
 *
 * Results cannot be streamed to the card as they are measured: `sd sweep` tears
 * the card down and reopens it on every step, which would invalidate any open
 * file, and `sd raw` runs with no filesystem mounted at all. So the report is
 * accumulated in RAM and written once, at the end, when the card is settled.
 */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} strbuf_t;

static void sb_init(strbuf_t *sb)
{
    sb->cap = 1024;
    sb->len = 0;
    sb->data = malloc(sb->cap);
    if (sb->data) {
        sb->data[0] = '\0';
    }
}

static void sb_free(strbuf_t *sb)
{
    free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

static void sb_vprintf(strbuf_t *sb, const char *fmt, va_list args)
{
    /* Capture is best effort. If it could not allocate, the console output has
     * still happened and the measurement is still valid. */
    if (!sb->data) {
        return;
    }

    va_list measure;
    va_copy(measure, args);
    int needed = vsnprintf(NULL, 0, fmt, measure);
    va_end(measure);
    if (needed < 0) {
        return;
    }

    if (sb->len + (size_t)needed + 1 > sb->cap) {
        size_t cap = sb->cap;
        while (sb->len + (size_t)needed + 1 > cap) {
            cap *= 2;
        }
        char *grown = realloc(sb->data, cap);
        if (!grown) {
            return;
        }
        sb->data = grown;
        sb->cap = cap;
    }

    vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, args);
    sb->len += (size_t)needed;
}

/*
 * Print to the console and capture the identical text for the results file, so
 * the saved report cannot drift from what was shown on screen.
 */
static void tee(strbuf_t *sb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void tee(strbuf_t *sb, const char *fmt, ...)
{
    va_list console;
    va_start(console, fmt);
    va_list captured;
    va_copy(captured, console);

    diag_vprintf(fmt, console);
    sb_vprintf(sb, fmt, captured);

    va_end(captured);
    va_end(console);
}

/*
 * The same, for text that lives on the res partition.
 *
 * strres_printf() cannot serve here: this output has two sinks, so the format
 * has to exist as a value rather than only reaching a printf. strres_hold()
 * lends it for the length of the call and pins nothing longer -- one hold at a
 * time, released before the next line. A miss prints the id, the way
 * strres_printf() would, so a line is never silently dropped from either sink.
 *
 * Call it through TEE(), which checks the arguments against the English text
 * the way STRRES_PRINTF() does.
 */
static void tee_id(strbuf_t *sb, strres_id_t id, ...)
{
    strres_hold_t hold = {0};
    const char *fmt = strres_hold(id, &hold);

    if (fmt) {
        va_list console;
        va_start(console, id);
        va_list captured;
        va_copy(captured, console);

        diag_vprintf(fmt, console);
        sb_vprintf(sb, fmt, captured);

        va_end(captured);
        va_end(console);
    } else {
        tee(sb, "[str:%04X]", (unsigned)id);
    }
    strres_release(&hold);
}

#define TEE(sb, id, ...)                                                \
    do {                                                                \
        if (0) strres_check_format(STRRES_FMT_##id, ##__VA_ARGS__);     \
        tee_id((sb), (id), ##__VA_ARGS__);                              \
    } while (0)

/* The run header goes only to the file, and needs the format as a value for the
 * same reason. */
static void file_id(FILE *f, strres_id_t id, ...)
{
    strres_hold_t hold = {0};
    const char *fmt = strres_hold(id, &hold);

    if (fmt) {
        va_list args;
        va_start(args, id);
        vfprintf(f, fmt, args);
        va_end(args);
    } else {
        fprintf(f, "[str:%04X]", (unsigned)id);
    }
    strres_release(&hold);
}

#define FILE_PRINTF(f, id, ...)                                         \
    do {                                                                \
        if (0) strres_check_format(STRRES_FMT_##id, ##__VA_ARGS__);     \
        file_id((f), (id), ##__VA_ARGS__);                              \
    } while (0)

/*
 * Stamp enough identity into the file that a result found on a card months later
 * can be tied back to the board, the wiring and the card it came from. The
 * station MAC is the part that is actually unique per board; the chip model and
 * pin roles say which design and which harness.
 */
static void write_run_header(FILE *f, strres_id_t test_name)
{
    static const char *const mmc_pin_names[6] = {"CLK", "CMD", "D0", "D1", "D2", "D3"};
    static const char *const spi_pin_names[4] = {"CLK", "MOSI", "MISO", "CS"};

    esp_chip_info_t chip;
    esp_chip_info(&chip);

    FILE_PRINTF(f, STR_SD_FILE_RULE_DOUBLE);
    /* A variable id, so unchecked -- and it is a whole line that takes no
     * arguments, which is the only shape this is ever given. */
    file_id(f, test_name);
    FILE_PRINTF(f, STR_SD_FILE_UPTIME, (unsigned long long)(esp_timer_get_time() / 1000000));

    FILE_PRINTF(f, STR_SD_FILE_CHIP,
                app_chip_model_name(chip.model), chip.revision / 100, chip.revision % 100,
                chip.cores, chip.cores == 1 ? "" : "s");

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        FILE_PRINTF(f, STR_SD_FILE_FLASH, (unsigned long)(flash_size / 1024));
    }

    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        FILE_PRINTF(f, STR_SD_FILE_MAC, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    const esp_app_desc_t *app = esp_app_get_description();
    if (app) {
        FILE_PRINTF(f, STR_SD_FILE_FIRMWARE,
                    app->project_name, app->version, app->date, app->time);
    }
    FILE_PRINTF(f, STR_SD_FILE_IDF, esp_get_idf_version());

    FILE_PRINTF(f, STR_SD_FILE_INTERFACE, iface_name());
    const char *const *names = (cfg.iface == IFACE_SPI) ? spi_pin_names : mmc_pin_names;
    for (int i = 0; i < cfg.pin_count; i++) {
        fprintf(f, "%s%s=GPIO%d", i ? ", " : "", names[i], cfg.pins[i]);
    }
    fprintf(f, "\n");

    if (card) {
        if (card->real_freq_khz * 1000 > card->csd.tr_speed) {
            FILE_PRINTF(f, STR_SD_FILE_CLOCK_OVER, card->real_freq_khz / 1000.0,
                        cfg.freq_khz, card->csd.tr_speed / 1e6);
        } else {
            FILE_PRINTF(f, STR_SD_FILE_CLOCK, card->real_freq_khz / 1000.0,
                        cfg.freq_khz, card->csd.tr_speed / 1e6);
        }
        FILE_PRINTF(f, STR_SD_FILE_CARD,
                    card->cid.name, card_type(),
                    (double)((uint64_t)card->csd.capacity * (uint64_t)card->csd.sector_size) / (1024.0 * 1024.0 * 1024.0),
                    (unsigned)card->cid.mfg_id, (unsigned)card->cid.serial,
                    2000 + (card->cid.date >> 4), card->cid.date & 0xF);
    }
    FILE_PRINTF(f, STR_SD_FILE_RULE_SINGLE);
}

/*
 * Append the accumulated report to the card.
 *
 * Best effort on purpose: the measurement has already succeeded by the time this
 * runs, so a card with no filesystem, or a full one, gets a note rather than
 * turning a good result into a failed command.
 */
static void results_save(strres_id_t test_name, const strbuf_t *sb)
{
    if (!sb->data || sb->len == 0) {
        return;
    }
    if (!mount_fat(/*quiet=*/true)) {
        STRRES_PRINTF(STR_SD_RESULTS_NO_FS);
        return;
    }

    FILE *f = fopen(SD_RESULTS_FILE, "a");
    if (!f) {
        STRRES_PRINTF(STR_SD_RESULTS_OPEN_FAILED, SD_RESULTS_FILE, strerror(errno));
        return;
    }

    write_run_header(f, test_name);
    bool ok = fwrite(sb->data, 1, sb->len, f) == sb->len;
    ok = (fflush(f) == 0) && ok;
    ok = (fsync(fileno(f)) == 0) && ok;
    fclose(f);

    if (ok) {
        STRRES_PRINTF(STR_SD_RESULTS_APPENDED, SD_RESULTS_FILE);
    } else {
        STRRES_PRINTF(STR_SD_RESULTS_WRITE_FAILED, SD_RESULTS_FILE, strerror(errno));
    }
}

/*
 * Print the results file back, or delete it.
 *
 * Worth having because the board is often the only thing holding the card: on a
 * bringup bench you cannot conveniently pull it and read it on a host, and
 * without this the saved file could not be checked at all.
 */
int cmd_sd_results(int argc, char **argv)
{
    if (!require_card()) {
        return -1;
    }
    if (!mount_fat(/*quiet=*/false)) {
        return -1;
    }

    if (argc > 1) {
        if (strcasecmp(argv[1], "clear") != 0) {
            STRRES_PRINTF(STR_SD_USAGE_RESULTS);
            return -1;
        }
        if (unlink(SD_RESULTS_FILE) != 0) {
            if (errno == ENOENT) {
                STRRES_PRINTF(STR_SD_RESULTS_NONE_TO_CLEAR);
                return 0;
            }
            STRRES_ERROR(STR_SD_REMOVE_FAILED, SD_RESULTS_FILE, strerror(errno));
            return -1;
        }
        STRRES_PRINTF(STR_SD_RESULTS_CLEARED, SD_RESULTS_FILE);
        return 0;
    }

    FILE *f = fopen(SD_RESULTS_FILE, "r");
    if (!f) {
        if (errno == ENOENT) {
            STRRES_PRINTF(STR_SD_RESULTS_NONE);
            return 0;
        }
        STRRES_ERROR(STR_SD_OPEN_FAILED, SD_RESULTS_FILE, strerror(errno));
        return -1;
    }

    char chunk[256];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk) - 1, f)) > 0) {
        chunk[got] = '\0';
        diag_write(chunk, got);
    }
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Benchmarks                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t bytes;
    int64_t elapsed_us;      /* time in the transfer calls only */
    int64_t worst_block_us;  /* slowest single block */
} bench_result_t;

/*
 * Report one direction.
 *
 * The worst single block is reported alongside the average because SD cards
 * stall for tens of milliseconds while they do internal housekeeping; on a
 * board that has to keep up with a sensor or a camera, that worst case is the
 * number that decides whether the design works, and an average hides it.
 */
static void report(strbuf_t *sb, const char *label, const bench_result_t *r, size_t block_bytes)
{
    double seconds = (double)r->elapsed_us / 1e6;
    double kib = (double)r->bytes / 1024.0;

    if (seconds <= 0.0) {
        TEE(sb, STR_SD_BENCH_TOO_FAST, label, kib);
        return;
    }

    TEE(sb, STR_SD_BENCH_RESULT,
        label, kib, seconds, kib / seconds, kib / seconds / 1024.0,
        (unsigned)(block_bytes / 1024), (double)r->worst_block_us / 1000.0);
}

/* Fill a block with a position-dependent pattern, stamped with the block index
 * so a filesystem that hands back the wrong block is caught rather than scored. */
static void fill_pattern(uint8_t *buf, size_t len, uint32_t block_index)
{
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(i * 31u + 7u);
    }
    if (len >= sizeof(uint32_t)) {
        memcpy(buf, &block_index, sizeof(uint32_t));
    }
}

int cmd_sd_bench(int argc, char **argv)
{
    if (!require_card()) {
        return -1;
    }

    size_t total_bytes = 0, block_bytes = 0;
    if (take_sizes(argc, argv, 1, &total_bytes, &block_bytes) < 0) {
        return -1;
    }
    if (!mount_fat(/*quiet=*/false)) {
        return -1;
    }

    const uint32_t blocks = (uint32_t)(total_bytes / block_bytes);

    uint8_t *pattern = heap_caps_malloc(block_bytes, MALLOC_CAP_DMA);
    uint8_t *readback = heap_caps_malloc(block_bytes, MALLOC_CAP_DMA);
    if (!pattern || !readback) {
        STRRES_ERROR(STR_SD_OUT_OF_DMA_MEMORY, (unsigned)(block_bytes / 1024));
        free(pattern);
        free(readback);
        return -1;
    }

    strbuf_t sb;
    sb_init(&sb);

    TEE(&sb, STR_SD_BENCH_HEADER,
        SD_BENCH_FILE, (unsigned)(total_bytes / 1024), (unsigned)(block_bytes / 1024),
        iface_name(), card->real_freq_khz / 1000.0);

    int result = -1;
    bench_result_t write_result = {0};
    bench_result_t read_result = {0};

    FILE *f = fopen(SD_BENCH_FILE, "wb");
    if (!f) {
        STRRES_ERROR(STR_SD_CREATE_FAILED, SD_BENCH_FILE, strerror(errno));
        goto done;
    }

    for (uint32_t i = 0; i < blocks; i++) {
        fill_pattern(pattern, block_bytes, i);
        int64_t start = esp_timer_get_time();
        size_t written = fwrite(pattern, 1, block_bytes, f);
        int64_t elapsed = esp_timer_get_time() - start;

        if (written != block_bytes) {
            STRRES_ERROR(STR_SD_BLOCK_WRITE_FAILED, (unsigned)i, strerror(errno));
            fclose(f);
            goto cleanup_file;
        }
        write_result.bytes += written;
        write_result.elapsed_us += elapsed;
        if (elapsed > write_result.worst_block_us) {
            write_result.worst_block_us = elapsed;
        }
    }

    /*
     * The flush is inside the measurement on purpose. Without it the card is
     * still absorbing the tail of the data when the clock stops, and the
     * reported write speed is the speed of filling a RAM buffer.
     */
    int64_t flush_start = esp_timer_get_time();
    bool flushed = (fflush(f) == 0) && (fsync(fileno(f)) == 0);
    int64_t flush_us = esp_timer_get_time() - flush_start;
    write_result.elapsed_us += flush_us;
    fclose(f);

    if (!flushed) {
        STRRES_ERROR(STR_SD_FLUSH_FAILED, SD_BENCH_FILE, strerror(errno));
        goto cleanup_file;
    }

    report(&sb, "Write", &write_result, block_bytes);
    TEE(&sb, STR_SD_BENCH_FINAL_FLUSH, (double)flush_us / 1000.0);

    f = fopen(SD_BENCH_FILE, "rb");
    if (!f) {
        STRRES_ERROR(STR_SD_REOPEN_FAILED, SD_BENCH_FILE, strerror(errno));
        goto cleanup_file;
    }

    for (uint32_t i = 0; i < blocks; i++) {
        int64_t start = esp_timer_get_time();
        size_t got = fread(readback, 1, block_bytes, f);
        int64_t elapsed = esp_timer_get_time() - start;

        if (got != block_bytes) {
            STRRES_ERROR(STR_SD_BLOCK_SHORT_READ, (unsigned)i, (unsigned)got);
            fclose(f);
            goto cleanup_file;
        }
        read_result.bytes += got;
        read_result.elapsed_us += elapsed;
        if (elapsed > read_result.worst_block_us) {
            read_result.worst_block_us = elapsed;
        }

        /* Verified outside the timed region above -- the compare is charged to
         * the CPU, not to the card. */
        fill_pattern(pattern, block_bytes, i);
        if (memcmp(pattern, readback, block_bytes) != 0) {
            STRRES_ERROR(STR_SD_BLOCK_MISMATCH, (unsigned)i);
            fclose(f);
            goto cleanup_file;
        }
    }
    fclose(f);

    report(&sb, "Read", &read_result, block_bytes);
    TEE(&sb, STR_SD_BENCH_VERIFIED, (unsigned)(read_result.bytes / 1024));
    result = 0;

cleanup_file:
    /* The card is left as it was found; this is a bring-up tool, not a
     * destructive one. */
    if (unlink(SD_BENCH_FILE) != 0 && errno != ENOENT) {
        STRRES_ERROR(STR_SD_REMOVE_FAILED, SD_BENCH_FILE, strerror(errno));
        result = -1;
    }

done:
    if (result == 0) {
        results_save(STR_SD_FILE_TEST_BENCH, &sb);
    }
    sb_free(&sb);
    free(pattern);
    free(readback);
    return result;
}

int cmd_sd_raw(int argc, char **argv)
{
    if (!require_card()) {
        return -1;
    }

    size_t total_bytes = 0, block_bytes = 0;
    if (take_sizes(argc, argv, 1, &total_bytes, &block_bytes) < 0) {
        return -1;
    }

    int start_sector = 0;
    if (argc > 3 && (cli_parse_int_arg(argv[3], &start_sector) < 0 || start_sector < 0)) {
        STRRES_ERROR(STR_SD_START_SECTOR_INVALID);
        return -1;
    }

    const size_t sector_size = (size_t)card->csd.sector_size;
    if (sector_size == 0 || block_bytes < sector_size) {
        STRRES_ERROR(STR_SD_BLOCK_BELOW_SECTOR, (unsigned)sector_size);
        return -1;
    }

    /* Whole sectors only: sdmmc_read_sectors() counts in sectors. */
    const size_t sectors_per_block = block_bytes / sector_size;
    block_bytes = sectors_per_block * sector_size;
    const uint32_t blocks = (uint32_t)(total_bytes / block_bytes);
    if (blocks == 0) {
        STRRES_ERROR(STR_SD_TRANSFER_BELOW_BLOCK);
        return -1;
    }

    const uint64_t last_sector = (uint64_t)start_sector + (uint64_t)blocks * sectors_per_block;
    if (last_sector > (uint64_t)card->csd.capacity) {
        STRRES_ERROR(STR_SD_SECTORS_PAST_END,
                     start_sector, (unsigned long long)last_sector - 1, card->csd.capacity);
        return -1;
    }

    uint8_t *buffer = heap_caps_malloc(block_bytes, MALLOC_CAP_DMA);
    if (!buffer) {
        STRRES_ERROR(STR_SD_OUT_OF_DMA_MEMORY, (unsigned)(block_bytes / 1024));
        return -1;
    }

    /*
     * Read-only, so it is safe to run anywhere on the card, and it measures the
     * interface without the filesystem's allocation and metadata overhead in
     * the way -- the difference between this and `sd bench` is what FAT costs.
     */
    strbuf_t sb;
    sb_init(&sb);

    TEE(&sb, STR_SD_RAW_HEADER,
        (unsigned)(blocks * block_bytes / 1024), start_sector, (unsigned)(block_bytes / 1024),
        iface_name(), card->real_freq_khz / 1000.0);

    bench_result_t result = {0};
    for (uint32_t i = 0; i < blocks; i++) {
        size_t sector = (size_t)start_sector + (size_t)i * sectors_per_block;

        int64_t start = esp_timer_get_time();
        esp_err_t err = sdmmc_read_sectors(card, buffer, sector, sectors_per_block);
        int64_t elapsed = esp_timer_get_time() - start;

        if (err != ESP_OK) {
            STRRES_ERROR(STR_SD_SECTOR_READ_FAILED,
                         (unsigned)sectors_per_block, (unsigned)sector, esp_err_to_name(err));
            sb_free(&sb);
            free(buffer);
            return -1;
        }
        result.bytes += block_bytes;
        result.elapsed_us += elapsed;
        if (elapsed > result.worst_block_us) {
            result.worst_block_us = elapsed;
        }
    }

    report(&sb, "Read", &result, block_bytes);
    free(buffer);
    results_save(STR_SD_FILE_TEST_RAW, &sb);
    sb_free(&sb);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Clock sweep                                                         */
/* ------------------------------------------------------------------ */

/*
 * Rates to try, in kHz. The host divides a fixed source clock, so most of these
 * land on the same actual frequency as a neighbour; the sweep reports what was
 * really produced and skips a step that repeats the previous one.
 *
 * 50000, 100000 and 200000 are deliberately absent: ESP-IDF treats those exact
 * values as requests for UHS-I modes and rejects them on hosts without UHS
 * support, which would look like a card failure rather than a host limitation.
 */
static const int freq_ladder[] = {
    20000, 24000, 26667, 30000, 33333, 36000, 40000,
    44000, 48000, 52000, 56000, 60000, 66667, 72000, 80000,
};

#define SWEEP_BASELINE_KHZ SDMMC_FREQ_DEFAULT
#define SWEEP_MAX_CONSECUTIVE_FAILURES 3

/*
 * How many consecutive steps may produce the same clock as the one before
 * before the sweep concludes the host, not the card, is the limit.
 *
 * It cannot be 1: the divider legitimately skips a step here and there (asking
 * for 30 MHz when 26.67 and 32 are the neighbouring divisors lands back on
 * 26.67, and the next step up still moves). Three in a row does not happen for
 * that reason -- it means every larger request is being clamped.
 */
#define SWEEP_MAX_CONSECUTIVE_DUPLICATES 3

/*
 * Read `blocks` blocks from sector 0 and return a CRC32 over the lot.
 *
 * A CRC rather than a kept copy of the reference: it costs 4 bytes instead of a
 * second buffer, which is what lets the verified region be large enough to be
 * worth something on a chip with a few hundred KB of RAM.
 */
static esp_err_t read_crc(uint8_t *buffer, size_t block_bytes, uint32_t blocks,
                          size_t sectors_per_block, uint32_t *out_crc,
                          bench_result_t *out_timing)
{
    uint32_t crc = 0;
    bench_result_t timing = {0};

    for (uint32_t i = 0; i < blocks; i++) {
        size_t sector = (size_t)i * sectors_per_block;

        int64_t start = esp_timer_get_time();
        esp_err_t err = sdmmc_read_sectors(card, buffer, sector, sectors_per_block);
        int64_t elapsed = esp_timer_get_time() - start;
        if (err != ESP_OK) {
            return err;
        }

        timing.bytes += block_bytes;
        timing.elapsed_us += elapsed;
        if (elapsed > timing.worst_block_us) {
            timing.worst_block_us = elapsed;
        }
        crc = esp_rom_crc32_le(crc, buffer, (uint32_t)block_bytes);
    }

    *out_crc = crc;
    if (out_timing) {
        *out_timing = timing;
    }
    return ESP_OK;
}

static double throughput_kib_s(const bench_result_t *r)
{
    if (r->elapsed_us <= 0) {
        return 0.0;
    }
    return ((double)r->bytes / 1024.0) / ((double)r->elapsed_us / 1e6);
}

int cmd_sd_sweep(int argc, char **argv)
{
    if (!require_card()) {
        return -1;
    }

    int max_khz = MAX_FREQ_KHZ;
    if (argc > 1 && (cli_parse_int_arg(argv[1], &max_khz) < 0 ||
                     max_khz < SWEEP_BASELINE_KHZ || max_khz > MAX_FREQ_KHZ)) {
        STRRES_ERROR(STR_SD_CEILING_RANGE, SWEEP_BASELINE_KHZ, MAX_FREQ_KHZ);
        return -1;
    }

    size_t total_bytes = 0, block_bytes = 0;
    if (take_sizes(argc, argv, 2, &total_bytes, &block_bytes) < 0) {
        return -1;
    }

    const size_t sector_size = (size_t)card->csd.sector_size;
    if (sector_size == 0 || block_bytes < sector_size) {
        STRRES_ERROR(STR_SD_BLOCK_BELOW_SECTOR, (unsigned)sector_size);
        return -1;
    }
    const size_t sectors_per_block = block_bytes / sector_size;
    block_bytes = sectors_per_block * sector_size;
    const uint32_t blocks = (uint32_t)(total_bytes / block_bytes);
    if (blocks == 0 || (uint64_t)blocks * sectors_per_block > (uint64_t)card->csd.capacity) {
        STRRES_ERROR(STR_SD_REGION_TOO_LARGE);
        return -1;
    }

    /* Remember what to fall back to if every overclocked rate fails. */
    const int entry_freq_khz = cfg.freq_khz;

    uint8_t *buffer = heap_caps_malloc(block_bytes, MALLOC_CAP_DMA);
    if (!buffer) {
        STRRES_ERROR(STR_SD_OUT_OF_DMA_MEMORY, (unsigned)(block_bytes / 1024));
        return -1;
    }

    strbuf_t sb;
    sb_init(&sb);

    TEE(&sb, STR_SD_SWEEP_HEADER, iface_name(), (unsigned)(blocks * block_bytes / 1024));
    TEE(&sb, STR_SD_SWEEP_READONLY_NOTE);

    /*
     * The drivers log the same lines on every single init, and the sweep does
     * one init per step; left alone that is a dozen copies of each interleaved
     * with the results table. Only the informational chatter is silenced --
     * warnings and errors still come through, because the reason a step failed
     * is exactly what the table cannot say on its own. Restored before
     * returning.
     */
    esp_log_level_set("SD_HOST", ESP_LOG_ERROR);
    esp_log_level_set("sdspi_transaction", ESP_LOG_WARN);

    int result = -1;
    uint32_t reference_crc = 0;

    /*
     * Establish the reference at a rate the card is rated for. Read it twice: if
     * the card cannot reproduce its own data in spec then every comparison after
     * this is meaningless, and it is better to say so than to report a
     * confident-looking overclocking limit derived from noise.
     */
    cfg.freq_khz = SWEEP_BASELINE_KHZ;
    esp_err_t err = open_card();
    if (err != ESP_OK) {
        STRRES_ERROR(STR_SD_REFERENCE_REINIT_FAILED, SWEEP_BASELINE_KHZ, esp_err_to_name(err));
        goto done;
    }

    bench_result_t reference_timing = {0};
    err = read_crc(buffer, block_bytes, blocks, sectors_per_block, &reference_crc,
                   &reference_timing);
    if (err == ESP_OK) {
        uint32_t again = 0;
        err = read_crc(buffer, block_bytes, blocks, sectors_per_block, &again, NULL);
        if (err == ESP_OK && again != reference_crc) {
            STRRES_ERROR(STR_SD_REFERENCE_MISMATCH, SWEEP_BASELINE_KHZ);
            goto done;
        }
    }
    if (err != ESP_OK) {
        STRRES_ERROR(STR_SD_REFERENCE_READ_FAILED,
                     SWEEP_BASELINE_KHZ, esp_err_to_name(err));
        goto done;
    }

    /*
     * The card's rated speed is not a constant, so it has to be re-read at every
     * step rather than captured once here.
     *
     * ESP-IDF only attempts the CMD6 high-speed switch when the host asks for
     * more than 20 MHz (sdmmc_enable_hs_mode_and_check). Below that the card is
     * left in Default Speed and its CSD reports 25 MHz; once switched, the CSD
     * is re-read and reports 50 MHz. Comparing every step against the rating
     * seen at the 20 MHz reference would therefore label perfectly in-spec
     * High Speed operation as overclocking.
     */
    const int baseline_rated_khz = card->csd.tr_speed / 1000;
    TEE(&sb, STR_SD_SWEEP_REFERENCE,
        (unsigned)reference_crc, baseline_rated_khz / 1000.0, SWEEP_BASELINE_KHZ);
    TEE(&sb, STR_SD_SWEEP_TABLE_HEADER);

    int best_requested_khz = 0;
    int best_actual_khz = 0;
    int best_rated_khz = baseline_rated_khz;
    double best_kib_s = 0.0;
    int previous_actual_khz = -1;
    int consecutive_failures = 0;
    int consecutive_duplicates = 0;
    int first_failure_khz = 0;
    bool host_clamped = false;

    for (size_t i = 0; i < sizeof(freq_ladder) / sizeof(freq_ladder[0]); i++) {
        const int requested = freq_ladder[i];
        if (requested > max_khz) {
            break;
        }

        cfg.freq_khz = requested;
        err = open_card();
        if (err != ESP_OK) {
            TEE(&sb, STR_SD_SWEEP_INIT_FAILED, requested, esp_err_to_name(err));
            if (!first_failure_khz) {
                first_failure_khz = requested;
            }
            if (++consecutive_failures >= SWEEP_MAX_CONSECUTIVE_FAILURES) {
                TEE(&sb, STR_SD_SWEEP_STOPPING, consecutive_failures);
                break;
            }
            continue;
        }

        const int actual = card->real_freq_khz;
        /* Re-read: a step that entered high-speed mode is rated higher than the
         * same card was at the reference rate. */
        const int step_rated_khz = card->csd.tr_speed / 1000;
        if (actual == previous_actual_khz) {
            TEE(&sb, STR_SD_SWEEP_SAME_CLOCK, requested, actual / 1000.0);
            if (++consecutive_duplicates >= SWEEP_MAX_CONSECUTIVE_DUPLICATES) {
                host_clamped = true;
                break;
            }
            continue;
        }
        previous_actual_khz = actual;
        consecutive_duplicates = 0;

        uint32_t crc = 0;
        bench_result_t timing = {0};
        err = read_crc(buffer, block_bytes, blocks, sectors_per_block, &crc, &timing);

        if (err != ESP_OK) {
            TEE(&sb, STR_SD_SWEEP_READ_FAILED,
                requested, actual / 1000.0, esp_err_to_name(err));
        } else if (crc != reference_crc) {
            /* This is the failure mode that matters: the transfer "succeeded"
             * and returned corrupt data. Without the CRC it would have been
             * recorded as a pass with a very good throughput figure. */
            TEE(&sb, STR_SD_SWEEP_MISMATCH,
                requested, actual / 1000.0, throughput_kib_s(&timing), (unsigned)crc);
        } else {
            const double kib_s = throughput_kib_s(&timing);
            if (actual > step_rated_khz) {
                TEE(&sb, STR_SD_SWEEP_OK_OVER, requested, actual / 1000.0, kib_s);
            } else {
                TEE(&sb, STR_SD_SWEEP_OK, requested, actual / 1000.0, kib_s);
            }
            if (actual > best_actual_khz) {
                best_requested_khz = requested;
                best_actual_khz = actual;
                best_rated_khz = step_rated_khz;
                best_kib_s = kib_s;
            }
            consecutive_failures = 0;
            continue;
        }

        if (!first_failure_khz) {
            first_failure_khz = requested;
        }
        if (++consecutive_failures >= SWEEP_MAX_CONSECUTIVE_FAILURES) {
            TEE(&sb, STR_SD_SWEEP_STOPPING, consecutive_failures);
            break;
        }
    }

    tee(&sb, "\n");
    if (best_actual_khz == 0) {
        STRRES_ERROR(STR_SD_NO_RATE_PASSED, SWEEP_BASELINE_KHZ);
        cfg.freq_khz = entry_freq_khz;
    } else {
        TEE(&sb, STR_SD_SWEEP_FASTEST,
            best_actual_khz / 1000.0, best_kib_s, best_kib_s / 1024.0,
            throughput_kib_s(&reference_timing) > 0.0 ? best_kib_s / throughput_kib_s(&reference_timing) : 0.0);
        if (first_failure_khz) {
            TEE(&sb, STR_SD_SWEEP_FIRST_FAILURE, first_failure_khz);
            /*
             * Above SDMMC_FREQ_DEFAULT the driver tries the CMD6 high-speed
             * switch, and it does so while the bus is still at the 400kHz
             * initialization clock -- sdmmc_init_card_hs_mode runs before
             * sdmmc_init_host_frequency. So a failure that appears exactly one
             * step above the default rate is a protocol threshold being
             * crossed, not the wiring running out of margin, and reporting it
             * as a speed limit sends people to look at their board for nothing.
             * A card that answers "not supported" is handled gracefully; one
             * that accepts the switch and then fails the follow-up SEND_CSD
             * takes the whole init down with it.
             */
            if (best_actual_khz <= SDMMC_FREQ_DEFAULT &&
                first_failure_khz > SDMMC_FREQ_DEFAULT) {
                TEE(&sb, STR_SD_SWEEP_HIGH_SPEED_NOTE,
                    SDMMC_FREQ_DEFAULT, SDMMC_FREQ_DEFAULT + 1, SDMMC_FREQ_DEFAULT);
            }
        } else if (host_clamped) {
            /*
             * Not the same thing as "the card is happy up here". Every larger
             * request produced the identical clock, so the card was never
             * actually driven any faster and its own limit is still unknown.
             */
            TEE(&sb, STR_SD_SWEEP_HOST_LIMITED,
                best_requested_khz, best_actual_khz / 1000.0, CONFIG_IDF_TARGET);
        } else {
            TEE(&sb, STR_SD_SWEEP_NO_FAILURES, max_khz);
        }
        if (best_actual_khz > best_rated_khz) {
            TEE(&sb, STR_SD_SWEEP_OVERCLOCKED_NOTE,
                (best_actual_khz - best_rated_khz) / 1000.0, best_rated_khz / 1000.0);
        } else {
            TEE(&sb, STR_SD_SWEEP_WITHIN_SPEC, best_rated_khz / 1000.0);
        }
        cfg.freq_khz = best_requested_khz;
        result = 0;
    }

    /* Leave the card usable at the best rate found, rather than wherever the
     * sweep happened to stop. */
    err = open_card();
    if (err != ESP_OK) {
        STRRES_ERROR(STR_SD_SWEEP_REINIT_FAILED,
                     cfg.freq_khz, esp_err_to_name(err));
        result = -1;
    } else {
        TEE(&sb, STR_SD_SWEEP_REINITIALIZED, card->real_freq_khz / 1000.0);
        /* Only now, back at a rate that passed verification, is it safe to put
         * anything on the card. */
        results_save(STR_SD_FILE_TEST_SWEEP, &sb);
    }

done:
    esp_log_level_set("SD_HOST", ESP_LOG_WARN);
    esp_log_level_set("sdspi_transaction", ESP_LOG_INFO);
    sb_free(&sb);
    free(buffer);
    return result;
}
