# Optional Musashi 68000 integration for the SCSP sound CPU.
# Including this module does not fetch anything unless SATURNBOX_MUSASHI_68K
# is explicitly enabled.
option(SATURNBOX_MUSASHI_68K "Fetch and build the Musashi 68000 core" OFF)

if(NOT SATURNBOX_MUSASHI_68K)
    return()
endif()

if(NOT CMAKE_C_COMPILER_LOADED)
    enable_language(C)
endif()

include(FetchContent)
FetchContent_Declare(musashi
    GIT_REPOSITORY https://github.com/kstenerud/Musashi.git
    GIT_TAG        313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd
    GIT_SHALLOW    FALSE
)
FetchContent_GetProperties(musashi)
if(NOT musashi_POPULATED)
    if(POLICY CMP0169)
        cmake_policy(PUSH)
        cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Populate(musashi)
    if(POLICY CMP0169)
        cmake_policy(POP)
    endif()
endif()

set(musashi_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/musashi-generated")
file(MAKE_DIRECTORY "${musashi_generated_dir}")
file(GENERATE OUTPUT "${musashi_generated_dir}/saturn_m68kconf.h" CONTENT
"#pragma once\n#define M68K_OPT_OFF 0\n#define M68K_OPT_ON 1\n#define M68K_OPT_SPECIFY_HANDLER 2\n#define M68K_COMPILE_FOR_MAME M68K_OPT_OFF\n#define M68K_EMULATE_010 M68K_OPT_OFF\n#define M68K_EMULATE_EC020 M68K_OPT_OFF\n#define M68K_EMULATE_020 M68K_OPT_OFF\n#define M68K_EMULATE_030 M68K_OPT_OFF\n#define M68K_EMULATE_040 M68K_OPT_OFF\n#define M68K_EMULATE_PMMU M68K_OPT_OFF\n#define M68K_EMULATE_PREFETCH M68K_OPT_OFF\n#define M68K_EMULATE_ADDRESS_ERROR M68K_OPT_ON\n#define M68K_USE_64_BIT M68K_OPT_ON\n")

add_executable(saturn_musashi_generator "${musashi_SOURCE_DIR}/m68kmake.c")
set_target_properties(saturn_musashi_generator PROPERTIES C_STANDARD 90)
if(MSVC)
    target_compile_options(saturn_musashi_generator PRIVATE /Od)
endif()

add_custom_command(
    OUTPUT "${musashi_generated_dir}/m68kops.c" "${musashi_generated_dir}/m68kops.h"
    COMMAND $<TARGET_FILE:saturn_musashi_generator>
        "${musashi_generated_dir}" "${musashi_SOURCE_DIR}/m68k_in.c"
    DEPENDS saturn_musashi_generator "${musashi_SOURCE_DIR}/m68k_in.c"
    VERBATIM
)

add_library(saturn_musashi_core STATIC
    "${musashi_SOURCE_DIR}/m68kcpu.c"
    # Musashi's m68kcpu.c includes m68kfpu.c and uses the bundled SoftFloat
    # implementation; retain its separate notice in third_party/.
    "${musashi_SOURCE_DIR}/softfloat/softfloat.c"
    "${musashi_generated_dir}/m68kops.c"
)
target_include_directories(saturn_musashi_core
    PUBLIC "${musashi_SOURCE_DIR}" "${musashi_generated_dir}"
)
target_compile_definitions(saturn_musashi_core PUBLIC
    "MUSASHI_CNF=\"saturn_m68kconf.h\""
)
set_target_properties(saturn_musashi_core PROPERTIES C_STANDARD 99)

add_library(saturn_m68k STATIC "${CMAKE_CURRENT_LIST_DIR}/../libs/saturn/src/m68k.cpp")
target_include_directories(saturn_m68k PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/../libs/saturn/include"
)
target_compile_features(saturn_m68k PUBLIC cxx_std_20)
target_link_libraries(saturn_m68k PUBLIC saturn_musashi_core)
add_library(saturn::m68k ALIAS saturn_m68k)
