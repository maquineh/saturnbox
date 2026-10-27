add_executable(saturn-sh2
    src/main.cpp
)

target_link_libraries(saturn-sh2 PRIVATE saturn::core saturn::sh2 saturn::platform)
target_compile_features(saturn-sh2 PRIVATE cxx_std_20)

add_executable(saturn-x4-sprite-probe EXCLUDE_FROM_ALL
    ../diagnostics/probe_x4_sprite_job.cpp)
target_link_libraries(saturn-x4-sprite-probe PRIVATE saturn::platform)
target_compile_features(saturn-x4-sprite-probe PRIVATE cxx_std_20)

set(SATURNBOX_STATIC_MODULE_SOURCE "" CACHE FILEPATH
    "Generated C++ source with statically compiled SH-2 blocks")
set(SATURNBOX_STATIC_MODULE_DIR "" CACHE PATH
    "Directory emitted by saturn-aot with incremental C++ address-bank shards")
if(SATURNBOX_STATIC_MODULE_SOURCE AND SATURNBOX_STATIC_MODULE_DIR)
    message(FATAL_ERROR
        "Set only one of SATURNBOX_STATIC_MODULE_SOURCE or SATURNBOX_STATIC_MODULE_DIR")
endif()
if(SATURNBOX_STATIC_MODULE_SOURCE)
    if(NOT EXISTS "${SATURNBOX_STATIC_MODULE_SOURCE}")
        message(FATAL_ERROR
            "SATURNBOX_STATIC_MODULE_SOURCE does not exist: ${SATURNBOX_STATIC_MODULE_SOURCE}")
    endif()
    target_sources(saturn-sh2 PRIVATE "${SATURNBOX_STATIC_MODULE_SOURCE}")
    target_compile_definitions(saturn-sh2 PRIVATE SATURNBOX_HAS_STATIC_MODULE=1)
endif()
if(SATURNBOX_STATIC_MODULE_DIR)
    get_filename_component(SATURNBOX_STATIC_MODULE_DIR_ABSOLUTE
        "${SATURNBOX_STATIC_MODULE_DIR}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    set(SATURNBOX_STATIC_MODULE_SOURCES_FILE
        "${SATURNBOX_STATIC_MODULE_DIR_ABSOLUTE}/saturnbox_sources.cmake")
    if(NOT EXISTS "${SATURNBOX_STATIC_MODULE_SOURCES_FILE}")
        message(FATAL_ERROR
            "Missing generated source list: ${SATURNBOX_STATIC_MODULE_SOURCES_FILE}. "
            "Run saturn-aot --output-dir before configuring.")
    endif()
    include("${SATURNBOX_STATIC_MODULE_SOURCES_FILE}")
    target_sources(saturn-sh2 PRIVATE ${SATURNBOX_GENERATED_STATIC_SOURCES})
    target_compile_definitions(saturn-sh2 PRIVATE SATURNBOX_HAS_STATIC_MODULE=1)
endif()

if(WIN32)
    target_link_libraries(saturn-sh2 PRIVATE winmm user32 gdi32 xinput9_1_0)
endif()

if(MSVC)
    target_compile_options(saturn-sh2 PRIVATE /W4 /permissive-)
else()
    target_compile_options(saturn-sh2 PRIVATE -Wall -Wextra -Wpedantic)
endif()
