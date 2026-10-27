add_executable(saturnbox-tests
    test_main.cpp
    analysis_tests.cpp
    decoder_tests.cpp
    cfg_tests.cpp
    lifter_tests.cpp
    sir_tests.cpp
    win64_jit_tests.cpp
    memory_map_tests.cpp
    slave_tests.cpp
)

target_link_libraries(saturnbox-tests PRIVATE saturn::core saturn::sh2 saturn::platform)
target_compile_features(saturnbox-tests PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(saturnbox-tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(saturnbox-tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME saturnbox-tests COMMAND saturnbox-tests)

# A portable MinGW installation keeps its C++ runtime DLLs beside its compiler.
# Make CTest deterministic even when CMake is launched before Windows reloads
# the user PATH modified by the installer.
if(WIN32 AND MINGW)
    get_filename_component(SATURNBOX_MINGW_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(saturnbox-tests PROPERTIES
        ENVIRONMENT "PATH=${SATURNBOX_MINGW_BIN}")
elseif(ENABLE_SANITIZERS AND MSVC)
    get_filename_component(SATURNBOX_MSVC_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(saturnbox-tests PROPERTIES
        ENVIRONMENT "PATH=${SATURNBOX_MSVC_BIN}")
endif()
