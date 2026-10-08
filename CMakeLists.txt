cmake_minimum_required(VERSION 3.21)
project(DOOM VERSION 1.10 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(DOOM_BUILD_X11 "Build the historical Linux X11/OSS executable" OFF)
option(DOOM_BUILD_SDL "Build the native playable SDL3 app" OFF)
option(DOOM_RENDER3D "Build the accelerated 3D renderer (SDL_gpu: Metal or Vulkan)" ON)
option(DOOM_TEST_GPU "Test 3D renderer gameplay (requires a desktop with Metal or Vulkan)" OFF)
set(DOOM_GAME_DATA "" CACHE FILEPATH "Optional IWAD to bundle as the default game")
set(DOOM_RELEASE_VERSION "0.1.0" CACHE STRING "Release version shown in the Mac app")
option(DOOM_ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
include(CTest)
include(TestBigEndian)
test_big_endian(DOOM_BIG_ENDIAN)

add_library(doom_options INTERFACE)
target_compile_definitions(doom_options INTERFACE NORMALUNIX DOOM_PORTABLE)
if(DOOM_BIG_ENDIAN)
    target_compile_definitions(doom_options INTERFACE DOOM_BIG_ENDIAN)
endif()
if(WIN32)
    target_compile_definitions(doom_options INTERFACE _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
else()
    target_compile_definitions(doom_options INTERFACE _POSIX_C_SOURCE=200809L)
endif()
if(MSVC)
    target_compile_options(doom_options INTERFACE /W3)
else()
    target_compile_options(doom_options INTERFACE
        -Wall -Werror=implicit-function-declaration -Werror=incompatible-pointer-types
        -Werror=int-to-pointer-cast -Werror=pointer-to-int-cast)
endif()
if(DOOM_ENABLE_SANITIZERS)
    if(MSVC)
        message(FATAL_ERROR "Sanitizer preset requires GCC or Clang")
    endif()
    target_compile_options(doom_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(doom_options INTERFACE -fsanitize=address,undefined)
endif()

set(DOOM_SOURCES
    linuxdoom-1.10/am_map.c
    linuxdoom-1.10/d_deh.c
    linuxdoom-1.10/d_items.c
    linuxdoom-1.10/d_main.c
    linuxdoom-1.10/d_net.c
    linuxdoom-1.10/doomdef.c
    linuxdoom-1.10/doomstat.c
    linuxdoom-1.10/dstrings.c
    linuxdoom-1.10/f_finale.c
    linuxdoom-1.10/f_wipe.c
    linuxdoom-1.10/g_game.c
    linuxdoom-1.10/hu_lib.c
    linuxdoom-1.10/hu_stuff.c
    linuxdoom-1.10/info.c
    linuxdoom-1.10/m_argv.c
    linuxdoom-1.10/m_bbox.c
    linuxdoom-1.10/m_cheat.c
    linuxdoom-1.10/m_fixed.c
    linuxdoom-1.10/m_menu.c
    linuxdoom-1.10/m_misc.c
    linuxdoom-1.10/m_random.c
    linuxdoom-1.10/m_swap.c
    linuxdoom-1.10/p_ceilng.c
    linuxdoom-1.10/p_doors.c
    linuxdoom-1.10/p_enemy.c
    linuxdoom-1.10/p_floor.c
    linuxdoom-1.10/p_inter.c
    linuxdoom-1.10/p_lights.c
    linuxdoom-1.10/p_map.c
    linuxdoom-1.10/p_maputl.c
    linuxdoom-1.10/p_mobj.c
    linuxdoom-1.10/p_plats.c
    linuxdoom-1.10/p_pspr.c
    linuxdoom-1.10/p_saveg.c
    linuxdoom-1.10/p_setup.c
    linuxdoom-1.10/p_sight.c
    linuxdoom-1.10/p_spec.c
    linuxdoom-1.10/p_switch.c
    linuxdoom-1.10/p_telept.c
    linuxdoom-1.10/p_tick.c
    linuxdoom-1.10/p_user.c
    linuxdoom-1.10/r_bsp.c
    linuxdoom-1.10/r_data.c
    linuxdoom-1.10/r_draw.c
    linuxdoom-1.10/r_main.c
    linuxdoom-1.10/r_plane.c
    linuxdoom-1.10/r_segs.c
    linuxdoom-1.10/r_sky.c
    linuxdoom-1.10/r_things.c
    linuxdoom-1.10/s_sound.c
    linuxdoom-1.10/sounds.c
    linuxdoom-1.10/st_lib.c
    linuxdoom-1.10/st_stuff.c
    linuxdoom-1.10/tables.c
    linuxdoom-1.10/v_video.c
    linuxdoom-1.10/w_wad.c
    linuxdoom-1.10/wi_stuff.c
    linuxdoom-1.10/z_zone.c
)
add_library(doom_core STATIC ${DOOM_SOURCES})
target_include_directories(doom_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/linuxdoom-1.10")
target_link_libraries(doom_core PUBLIC doom_options)
if(NOT WIN32)
    target_link_libraries(doom_core PUBLIC m)
endif()

# A complete link of the engine without graphics/audio dependencies.
add_library(doom_headless_platform OBJECT platform/i_headless.c platform/i_portable_system.c)
target_link_libraries(doom_headless_platform PUBLIC doom_core)
add_executable(doom-headless platform/headless_main.c)
target_link_libraries(doom-headless PRIVATE doom_headless_platform)

if(DOOM_BUILD_SDL)
    # enable_language() must run at directory scope, not inside the functions
    # below, and before SDL is added as a subproject.
    if(DOOM_RENDER3D)
        enable_language(CXX)
    endif()
    if(APPLE)
        enable_language(OBJCXX)
    endif()
    find_package(SDL3 3.2 CONFIG QUIET)
    if(NOT SDL3_FOUND)
        include(FetchContent)
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_TESTS OFF CACHE BOOL "" FORCE)
        set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(SDL3
            URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
            URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3)
        FetchContent_MakeAvailable(SDL3)
    endif()
    # Nuked OPL3 (LGPL-2.1+) emulates the FM chip for OPL music.
    include(FetchContent)
    FetchContent_Declare(NukedOPL3
        URL https://github.com/nukeykt/Nuked-OPL3/archive/765ec962e473aeb767e4cba74ffdc8f588ffbfe8.tar.gz
        URL_HASH SHA256=2fad908c3904d3ef51b55e0d6a8980c7142942e2d0baa7e99bf038cf0a41199d)
    FetchContent_MakeAvailable(NukedOPL3)
    add_library(nuked_opl3 STATIC "${nukedopl3_SOURCE_DIR}/opl3.c")
    target_include_directories(nuked_opl3 PUBLIC "${nukedopl3_SOURCE_DIR}")
    # The accelerated 3D renderer: SDL_gpu drives Metal on macOS and Vulkan on
    # Linux and Windows (SDL_GPU_DRIVER=vulkan selects MoltenVK on a Mac).
    function(doom_add_renderer target enabled)
        if(NOT enabled)
            return()
        endif()
        target_sources(${target} PRIVATE platform/scene3d.cpp platform/i_gpu3d.cpp platform/settings_panel.cpp
            platform/settings_menu.cpp)
        set_target_properties(${target} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)
        target_compile_definitions(${target} PRIVATE DOOM_RENDER3D)
        if(APPLE)
            target_sources(${target} PRIVATE platform/i_mac_settings.mm)
            set_source_files_properties(platform/i_mac_settings.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-D_DARWIN_C_SOURCE")
            set_target_properties(${target} PROPERTIES OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED YES)
            target_link_libraries(${target} PRIVATE "-framework Cocoa")
        endif()
    endfunction()
    add_executable(doom MACOSX_BUNDLE platform/sdl_main.c platform/i_sdl_video.c
        platform/i_sdl_sound.c platform/i_opl_music.c platform/i_singleplayer.c
        platform/i_portable_system.c platform/wad_probe.c)
    target_link_libraries(doom PRIVATE doom_core SDL3::SDL3 nuked_opl3)
    target_compile_definitions(doom PRIVATE DOOM_NATIVE)
    set_target_properties(doom PROPERTIES WIN32_EXECUTABLE TRUE)
    doom_add_renderer(doom ${DOOM_RENDER3D})
    if(APPLE)
        target_sources(doom PRIVATE platform/i_mac_music.c platform/i_wad_launcher.mm)
        set_source_files_properties(platform/i_wad_launcher.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-D_DARWIN_C_SOURCE")
        target_link_libraries(doom PRIVATE "-framework AudioToolbox" "-framework CoreFoundation" "-framework Cocoa")
        set_target_properties(doom PROPERTIES
            OUTPUT_NAME "Play DOOM"
            MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.robongrui.duct-tape-doom"
            MACOSX_BUNDLE_BUNDLE_NAME "Play DOOM"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/platform/Info.plist.in")
        if(DOOM_GAME_DATA)
            add_custom_command(TARGET doom POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:doom>/Contents/Resources"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DOOM_GAME_DATA}"
                    "$<TARGET_BUNDLE_DIR:doom>/Contents/Resources/default.wad")
        endif()
    elseif(DOOM_GAME_DATA)
        add_custom_command(TARGET doom POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DOOM_GAME_DATA}" "$<TARGET_FILE_DIR:doom>/default.wad")
    endif()
    if(BUILD_TESTING AND DOOM_GAME_DATA)
        function(doom_add_smoke target renderer)
            add_executable(${target} tests/native_smoke.c platform/i_sdl_video.c
                platform/i_sdl_sound.c platform/i_opl_music.c platform/i_singleplayer.c
                platform/i_portable_system.c)
            target_compile_definitions(${target} PRIVATE DOOM_TEST_DRIVER)
            target_link_libraries(${target} PRIVATE doom_core SDL3::SDL3 nuked_opl3)
            doom_add_renderer(${target} ${renderer})
            if(APPLE)
                target_sources(${target} PRIVATE platform/i_mac_music.c)
                target_link_libraries(${target} PRIVATE "-framework AudioToolbox" "-framework CoreFoundation")
            endif()
        endfunction()
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/smoke-data")
        doom_add_smoke(doom-native-smoke OFF)
        add_test(NAME native-gameplay-save-load COMMAND doom-native-smoke "${DOOM_GAME_DATA}")
        set_tests_properties(native-gameplay-save-load PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/smoke-data"
            ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy"
            TIMEOUT 30)
        if(DOOM_TEST_GPU)
            doom_add_smoke(doom-gpu-smoke ON)
            add_test(NAME gpu-gameplay-save-load COMMAND doom-gpu-smoke "${DOOM_GAME_DATA}")
            set_tests_properties(gpu-gameplay-save-load PROPERTIES
                WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/smoke-data" TIMEOUT 60)
        endif()
    endif()
endif()

# platform/gpu_shaders.h is generated from platform/shaders and committed;
# `cmake --build <dir> --target doom-gpu-shaders` regenerates it.
find_program(DOOM_GLSLANG glslangValidator)
find_program(DOOM_SPIRV_CROSS spirv-cross)
if(DOOM_GLSLANG AND DOOM_SPIRV_CROSS)
    add_custom_target(doom-gpu-shaders
        COMMAND ${CMAKE_COMMAND} -DGLSLANG=${DOOM_GLSLANG} -DSPIRV_CROSS=${DOOM_SPIRV_CROSS} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/gpu-shaders
            -P ${CMAKE_CURRENT_SOURCE_DIR}/scripts/compile_gpu_shaders.cmake
        COMMENT "Regenerating platform/gpu_shaders.h")
endif()

if(DOOM_BUILD_X11)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "The historical X11/OSS backend is Linux-only; use the portable targets here")
    endif()
    find_package(X11 REQUIRED)
    if(NOT X11_Xext_FOUND)
        message(FATAL_ERROR "The historical backend requires Xext development files")
    endif()
    add_executable(doom-x11 linuxdoom-1.10/i_main.c linuxdoom-1.10/i_net.c
        linuxdoom-1.10/i_sound.c linuxdoom-1.10/i_video.c platform/i_portable_system.c)
    target_compile_definitions(doom-x11 PRIVATE LINUX)
    target_include_directories(doom-x11 PRIVATE ${X11_INCLUDE_DIR})
    target_link_libraries(doom-x11 PRIVATE doom_core ${X11_LIBRARIES} ${X11_Xext_LIB})
endif()

if(BUILD_TESTING)
    if(APPLE)
        enable_language(CXX)
        add_executable(doom-surface-lighting-tests tests/surface_lighting.cpp)
        target_compile_features(doom-surface-lighting-tests PRIVATE cxx_std_17)
        add_test(NAME surface-lighting-transitions COMMAND doom-surface-lighting-tests)
        add_executable(doom-baked-lighting-tests tests/baked_lighting.cpp)
        target_compile_features(doom-baked-lighting-tests PRIVATE cxx_std_17)
        add_test(NAME baked-lighting-rays COMMAND doom-baked-lighting-tests)
    endif()
    add_executable(doom-screen-glass-tests tests/screen_glass.c)
    if(UNIX AND NOT APPLE)
        target_link_libraries(doom-screen-glass-tests PRIVATE m)
    endif()
    add_test(NAME screen-glass-detection COMMAND doom-screen-glass-tests)
    add_executable(doom-detail-texture-tests tests/detail_texture.c)
    if(UNIX AND NOT APPLE)
        target_link_libraries(doom-detail-texture-tests PRIVATE m)
    endif()
    add_test(NAME detail-texture-maps COMMAND doom-detail-texture-tests)
    add_executable(doom-wad-probe-tests tests/wad_probe.c platform/wad_probe.c)
    add_test(NAME wad-loader-probe COMMAND doom-wad-probe-tests)
    add_executable(doom-portability-tests tests/portability.c)
    target_link_libraries(doom-portability-tests PRIVATE doom_headless_platform)
    add_test(NAME portability COMMAND doom-portability-tests)
    add_test(NAME headless-help COMMAND doom-headless --help)
    add_test(NAME headless-requires-demo COMMAND doom-headless)
    if(DOOM_GLSLANG AND DOOM_SPIRV_CROSS)
        # Checks that platform/shaders compile; the committed header is not touched.
        add_test(NAME gpu-shaders-compile COMMAND ${CMAKE_COMMAND} -DGLSLANG=${DOOM_GLSLANG} -DSPIRV_CROSS=${DOOM_SPIRV_CROSS}
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/gpu_shaders_check.h -DWORK=${CMAKE_CURRENT_BINARY_DIR}/gpu-shaders
            -P ${CMAKE_CURRENT_SOURCE_DIR}/scripts/compile_gpu_shaders.cmake)
    endif()
    set_tests_properties(headless-requires-demo PROPERTIES WILL_FAIL TRUE)
endif()
