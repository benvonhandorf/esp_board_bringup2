#include "app_bringup.h"
#include "sys_hw.h"

#include "esp_chip_info.h"
#include "esp_clk_tree.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "soc/rtc.h"
#include "esp_heap_caps.h"

/* How long the 32kHz oscillator is given to start up before we call it dead.
 * A watch crystal needs a few hundred milliseconds; be generous. */
#define LFXTAL_SETTLE_MS 1000

/* A 32.768kHz crystal that is running should measure within a few percent. */
#define LFXTAL_NOMINAL_HZ 32768
#define LFXTAL_TOLERANCE_HZ 3000

/* Slow-clock cycles to average over. More cycles is more accurate but slower;
 * 100 cycles of 32kHz is about 3ms. */
#define LFXTAL_CAL_CYCLES 100

const char *app_chip_model_name(esp_chip_model_t model)
{
    switch (model) {
    case CHIP_ESP32:    return "ESP32";
    case CHIP_ESP32S2:  return "ESP32-S2";
    case CHIP_ESP32S3:  return "ESP32-S3";
    case CHIP_ESP32C3:  return "ESP32-C3";
    case CHIP_ESP32C2:  return "ESP32-C2";
    case CHIP_ESP32C6:  return "ESP32-C6";
    case CHIP_ESP32H2:  return "ESP32-H2";
    case CHIP_ESP32P4:  return "ESP32-P4";
    case CHIP_ESP32C5:  return "ESP32-C5";
    default:            return CONFIG_IDF_TARGET;
    }
}

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_EXT:      return "external pin";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT:      return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO:     return "SDIO";
    case ESP_RST_USB:      return "USB peripheral";
    case ESP_RST_JTAG:     return "JTAG";
    default:               return "unknown";
    }
}

static void print_memory(const char *label, uint32_t caps)
{
    size_t total = heap_caps_get_total_size(caps);
    if (total == 0) {
        return;
    }
    size_t free_now = heap_caps_get_free_size(caps);
    size_t low_water = heap_caps_get_minimum_free_size(caps);

    STRRES_PRINTF(STR_SYS_MEMORY_ROW,
                  label, (unsigned)(total / 1024), (unsigned)(free_now / 1024),
                  (unsigned)(low_water / 1024));
}

void sys_hw_print_chip_info(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);

    STRRES_PRINTF(STR_SYS_CHIP,
                  app_chip_model_name(info.model), info.revision / 100, info.revision % 100,
                  info.cores, info.cores == 1 ? "" : "s");

    STRRES_PRINTF(STR_SYS_FEATURES,
                  info.features & CHIP_FEATURE_WIFI_BGN ? " WiFi-b/g/n" : "",
                  info.features & CHIP_FEATURE_BT ? " BT" : "",
                  info.features & CHIP_FEATURE_BLE ? " BLE" : "",
                  info.features & CHIP_FEATURE_IEEE802154 ? " 802.15.4" : "",
                  info.features & CHIP_FEATURE_EMB_FLASH ? " embedded-flash" : "");

    uint32_t flash_size = 0;
    esp_err_t err = esp_flash_get_size(NULL, &flash_size);
    if (err == ESP_OK) {
        STRRES_PRINTF(STR_SYS_FLASH, (unsigned long)(flash_size / 1024));
    } else {
        STRRES_PRINTF(STR_SYS_FLASH_UNAVAILABLE, esp_err_to_name(err));
    }

    STRRES_PRINTF(STR_SYS_MEMORY_HEADER);
    print_memory("internal", MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT);
    print_memory("DMA", MALLOC_CAP_DMA);
    print_memory("PSRAM", MALLOC_CAP_SPIRAM);

    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        STRRES_PRINTF(STR_SYS_MAC, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    STRRES_PRINTF(STR_SYS_RESET, reset_reason_name(esp_reset_reason()));
    STRRES_PRINTF(STR_SYS_UPTIME, (unsigned long long)(esp_timer_get_time() / 1000000));
}

/*
 * Measure a clock by calibrating it against the main crystal.
 *
 * rtc_clk_cal() returns the average period in microseconds as a Q13.19 fixed
 * point value, or 0 if calibration timed out -- which is exactly what happens
 * when no 32kHz crystal is fitted. Note that rtc_clk_slow_freq_get_hz() cannot
 * be used for this: it returns a nominal constant for whichever source is
 * selected, so it reports 32768 Hz even for a dead oscillator.
 */
static uint32_t measure_clock_hz(soc_clk_freq_calculation_src_t source, uint32_t cycles)
{
    uint32_t period_q13_19 = rtc_clk_cal(source, cycles);
    if (period_q13_19 == 0) {
        return 0;
    }
    return (uint32_t)((1000000ULL << RTC_CLK_CAL_FRACT) / period_q13_19);
}

int cmd_sys_lfxtal(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    soc_rtc_slow_clk_src_t original = rtc_clk_slow_src_get();

    STRRES_PRINTF(STR_SYS_XTAL, (int)rtc_clk_xtal_freq_get());
    STRRES_PRINTF(STR_SYS_RTC_SLOW_SOURCE,
                  original == SOC_RTC_SLOW_CLK_SRC_XTAL32K ? "XTAL32K" : "internal RC oscillator");

    STRRES_PRINTF(STR_SYS_LFXTAL_ENABLING);
    rtc_clk_32k_enable(true);
    vTaskDelay(pdMS_TO_TICKS(LFXTAL_SETTLE_MS));

    uint32_t measured = measure_clock_hz(CLK_CAL_32K_XTAL, LFXTAL_CAL_CYCLES);
    if (measured == 0) {
        rtc_clk_32k_enable(false);
        STRRES_ERROR(STR_SYS_LFXTAL_ABSENT);
        return -1;
    }

    STRRES_PRINTF(STR_SYS_LFXTAL_MEASURED, (unsigned long)measured);

    if (measured < LFXTAL_NOMINAL_HZ - LFXTAL_TOLERANCE_HZ ||
        measured > LFXTAL_NOMINAL_HZ + LFXTAL_TOLERANCE_HZ) {
        rtc_clk_32k_enable(false);
        STRRES_ERROR(STR_SYS_LFXTAL_OFF_FREQUENCY,
                     (unsigned long)measured,
                     original == SOC_RTC_SLOW_CLK_SRC_XTAL32K ? "XTAL32K" : "the RC oscillator");
        return -1;
    }

    /* Selecting it as the RTC slow clock is what actually puts it to work. */
    rtc_clk_slow_src_set(SOC_RTC_SLOW_CLK_SRC_XTAL32K);
    vTaskDelay(pdMS_TO_TICKS(100));

    if (rtc_clk_slow_src_get() != SOC_RTC_SLOW_CLK_SRC_XTAL32K) {
        rtc_clk_slow_src_set(original);
        STRRES_ERROR(STR_SYS_LFXTAL_SWITCH_FAILED);
        return -1;
    }

    STRRES_PRINTF(STR_SYS_LFXTAL_SWITCHED);
    STRRES_PRINTF(STR_SYS_LFXTAL_OK);
    return 0;
}
