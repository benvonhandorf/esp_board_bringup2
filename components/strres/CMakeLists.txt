# strres -- runtime reader for the .sr string catalogues.
#
# The CMake code-generation API lives in project_include.cmake, which IDF pulls
# into project scope before any component is added. This file builds only the
# reader.

idf_component_register(
    SRCS             "src/strres.c"
    INCLUDE_DIRS     "include"
    PRIV_INCLUDE_DIRS "src"
    # esp_err_t appears in the public header.
    REQUIRES         esp_common
    # diag is the output fan-out every printed string goes through, and freertos
    # provides the mutex guarding the section cache. Both are private: a caller
    # needs neither to include strres.h.
    PRIV_REQUIRES    diag freertos)

# Strict warnings for this component only. PRIVATE, so a consuming project is
# not forced to build the rest of its tree this way.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
