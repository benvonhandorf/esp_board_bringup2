# The config walker is generated from config/config_schema.json. Every section
# there $refs the schema owned by the component that consumes it, resolved by
# component name -- so the reference does not depend on where those components
# live.
#
# The guard is mandatory: ESP-IDF re-runs this file in `cmake -P` script mode to
# collect requirements, and js2c_generate_sections() does not exist in that pass.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    js2c_generate_sections("${CMAKE_CURRENT_LIST_DIR}/../config/config_schema.json"
                           BASENAME app_config OUT_C GEN_C)
    get_filename_component(GEN_DIR "${GEN_C}" DIRECTORY)
endif()

# No REQUIRES, deliberately: project.cmake grants main an implicit dependency on
# every component in the build only while REQUIRES and PRIV_REQUIRES are both
# unset. Naming one component here would silently strip the rest.

#
# The subdirectories under main/ are plain source folders, NOT ESP-IDF
# components: IDF only auto-discovers components under components/ or
# EXTRA_COMPONENT_DIRS, so an idf_component_register() in main/gpio/ would be
# silently ignored and those files never compiled. A new file must be added to
# SRCS below or it does not get built, silently.
#
idf_component_register(
    SRCS         "main.c"
                 "config_reader.c"
                 "app_console.c"
                 "app_http.c"
                 "app_status.c"
                 "audio/audio.c"
                 "audio/i2s_bus.c"
                 "audio/tone.c"
                 "audio/capture.c"
                 "audio/codec_nau8822.c"
                 "audio/codec_ns4168.c"
                 "audio/codec_sph0645.c"
                 "board/board.c"
                 "gpio/gpio.c"
                 "gpio/pwm.c"
                 "i2c/i2c.c"
                 "i2c/aw9523b_cmd.c"
                 "i2c/ina219_cmd.c"
                 "i2c/ina226_cmd.c"
                 "i2c/ina237_cmd.c"
                 "i2c/lm75bdp_cmd.c"
                 "i2c/nau7802_cmd.c"
                 "i2c/pi4ioe5v6408_cmd.c"
                 "i2c/rx8130ce_cmd.c"
                 "i2c/sht4x_cmd.c"
                 "loadcell/hx711_cmd.c"
                 "sd/sd.c"
                 "spi/spi.c"
                 "sys/sys_hw.c"
                 "touch/touch.c"
                 "uart/uart.c"
                 "wifi/wifi.c"
                 "${GEN_C}"
    INCLUDE_DIRS "."
                 "audio"
                 "board"
                 "gpio"
                 "i2c"
                 "loadcell"
                 "sd"
                 "spi"
                 "sys"
                 "touch"
                 "uart"
                 "wifi"
                 "${GEN_DIR}")

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)

# LOG_ERROR must be in scope before js2c_builtins.h inside the generated unit
# only, so hand-written sources keep their own error handling.
set_source_files_properties("${GEN_C}" TARGET_DIRECTORY ${COMPONENT_LIB}
    PROPERTIES COMPILE_OPTIONS "-include;js2c_error_capture.h")
