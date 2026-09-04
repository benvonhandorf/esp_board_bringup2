cmake_minimum_required(VERSION 3.22)

#
# Where the shared components come from.
#
# While esp_components is a local checkout, it is reached through
# EXTRA_COMPONENT_DIRS. Override the location with -DESP_COMPONENTS_DIR=... or the
# ESP_COMPONENTS_DIR environment variable.
#
# Once that repository is published and tagged, delete this block and depend on
# the components by version in main/idf_component.yml instead:
#
#     dependencies:
#       cli:
#         git: https://github.com/benvonhandorf/esp_components.git
#         path: cli
#         version: cli-v0.1.0
#
# Nothing else in this project changes when you do -- the components are found by
# name either way, which is the property the whole arrangement exists to have.
#
if(NOT DEFINED ESP_COMPONENTS_DIR)
    if(DEFINED ENV{ESP_COMPONENTS_DIR})
        set(ESP_COMPONENTS_DIR "$ENV{ESP_COMPONENTS_DIR}")
    else()
        set(ESP_COMPONENTS_DIR "${CMAKE_CURRENT_LIST_DIR}/../esp_components")
    endif()
endif()

if(EXISTS "${ESP_COMPONENTS_DIR}")
    list(APPEND EXTRA_COMPONENT_DIRS "${ESP_COMPONENTS_DIR}")
    message(STATUS "Shared components: ${ESP_COMPONENTS_DIR}")
else()
    message(FATAL_ERROR
        "Shared components not found at ${ESP_COMPONENTS_DIR}.\n"
        "Clone benvonhandorf/esp_components beside this project, or pass "
        "-DESP_COMPONENTS_DIR=/path/to/esp_components.")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(idf_template)

#
# Build the LittleFS image from fs/ and flash it with the firmware.
#
# fs/config.json is gitignored and holds the device's real credentials;
# config/config.example.json is the committed template. A project that ships
# without a config.json still boots -- config_store reports the file is missing
# and the schema's defaults apply -- so a fresh clone is buildable.
#
littlefs_create_partition_image(res fs FLASH_IN_PROJECT)
