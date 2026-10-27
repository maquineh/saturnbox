add_library(saturn_platform
    src/boot_services.cpp
    src/cd.cpp
    src/dma.cpp
    src/mmio.cpp
    src/platform.cpp
    src/scheduler.cpp
    src/slave.cpp
    src/scsp.cpp
    src/smpc.cpp
    src/video.cpp
    src/vulkan_vdp1.cpp
)

add_library(saturn::platform ALIAS saturn_platform)

target_include_directories(saturn_platform
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(saturn_platform PUBLIC saturn::core saturn::sir saturn::sh2)
target_compile_features(saturn_platform PUBLIC cxx_std_20)

option(SATURNBOX_VULKAN "Build the VDP1 Vulkan compute backend" ON)
if(SATURNBOX_VULKAN)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    include(FetchContent)
    find_path(SATURN_VULKAN_INCLUDE vulkan/vulkan.h HINTS "$ENV{VULKAN_SDK}/Include")
    if(NOT SATURN_VULKAN_INCLUDE)
        FetchContent_Declare(saturn_vulkan_headers
            URL https://codeload.github.com/KhronosGroup/Vulkan-Headers/zip/refs/tags/v1.3.290
            URL_HASH SHA256=918a8bb31b8312fd649fd1d1826443da870f33f3187c16c4528aa94927409bab)
        FetchContent_MakeAvailable(saturn_vulkan_headers)
        set(SATURN_VULKAN_INCLUDE "${saturn_vulkan_headers_SOURCE_DIR}/include")
    endif()
    find_program(SATURN_GLSLANG NAMES glslangValidator glslang HINTS "$ENV{VULKAN_SDK}/Bin")
    if(NOT SATURN_GLSLANG)
        set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
        set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
        set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
        set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
        set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
        set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(saturn_glslang
            URL https://codeload.github.com/KhronosGroup/glslang/zip/refs/tags/15.0.0
            URL_HASH SHA256=5e0a0df342ed61e4a5852541731fe37b7a38ac45f466e547e6ed9adfe308068d)
        FetchContent_MakeAvailable(saturn_glslang)
        set(SATURN_GLSLANG "$<TARGET_FILE:glslang-standalone>")
        set(shader_dependency glslang-standalone)
    endif()
    set(shader_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
    add_custom_command(OUTPUT "${shader_dir}/vdp1_compute_spv.hpp"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${shader_dir}"
        COMMAND "${SATURN_GLSLANG}" -V --target-env vulkan1.1
            -o "${shader_dir}/vdp1.spv" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vdp1.comp"
        COMMAND ${CMAKE_COMMAND} "-DINPUT=${shader_dir}/vdp1.spv"
            "-DOUTPUT=${shader_dir}/vdp1_compute_spv.hpp"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/shaders/embed_spirv.cmake"
        DEPENDS shaders/vdp1.comp shaders/embed_spirv.cmake ${shader_dependency}
        VERBATIM)
    target_sources(saturn_platform PRIVATE "${shader_dir}/vdp1_compute_spv.hpp")
    target_include_directories(saturn_platform PRIVATE "${shader_dir}" "${SATURN_VULKAN_INCLUDE}")
    target_compile_definitions(saturn_platform PRIVATE SATURN_HAS_VULKAN=1)
    target_link_libraries(saturn_platform PRIVATE ${CMAKE_DL_LIBS})
endif()

if(MSVC)
    target_compile_options(saturn_platform PRIVATE /W4 /permissive-)
else()
    target_compile_options(saturn_platform PRIVATE -Wall -Wextra -Wpedantic)
endif()
