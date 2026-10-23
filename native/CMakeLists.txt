# LXR as CLAP and VST3 plugins (and the core library other native targets use).
#
#   cmake -S native -B native/build -DCMAKE_BUILD_TYPE=Release
#   cmake --build native/build --target lxr_all
#
# The plugin is written against CLAP; clap-wrapper turns the same code into a VST3.
# clap-wrapper, the CLAP headers and the VST3 SDK are downloaded at configure time
# unless their checkouts are given with -DCLAP_WRAPPER_ROOT=, -DCLAP_SDK_ROOT= and
# -DVST3_SDK_ROOT=.
cmake_minimum_required(VERSION 3.21)

if (APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 10.15 CACHE STRING "Minimum macOS version")
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal binary")
endif()

project(lxr VERSION 0.1.0 LANGUAGES C CXX)
if (APPLE)
    # clap-wrapper's macOS parts, compiled into our targets, are Objective-C++
    enable_language(OBJC)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
# the static MSVC runtime, as clap-wrapper uses (all parts of a plugin must match)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(LXR_PLUGINS "Build the CLAP and VST3 plugins" ON)
option(LXR_TESTS "Build the test driver used by rnbo/test (--native)" ON)
set(LXR_PLUGIN_FORMATS CLAP VST3 CACHE STRING "Plugin formats (clap-wrapper names)")

# The engine emulates the firmware's float32 arithmetic with doubles: no contraction
# into fused multiply-adds and no fast math, or the sound drifts from the firmware's.
if (MSVC)
    set(LXR_FP_FLAGS /fp:precise /bigobj)
else()
    set(LXR_FP_FLAGS -ffp-contract=off -fno-fast-math)
endif()

# the engine and sequencer as C++ (generated from the RNBO codebox sources) and the
# instrument around them
add_library(lxr-core STATIC core/LxrInstrument.cpp)
target_include_directories(lxr-core PUBLIC core generated)
target_compile_options(lxr-core PUBLIC ${LXR_FP_FLAGS})

# regenerate native/generated after editing rnbo/src (the output is committed, so a
# plain build doesn't need Python)
find_package(Python3 COMPONENTS Interpreter)
if (Python3_FOUND)
    add_custom_target(lxr-generate
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../rnbo/tools/build.py
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/codebox2cpp.py
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..
            COMMENT "Regenerating the codebox and C++ sources")
endif()

if (LXR_TESTS)
    add_executable(lxr_native test/lxr_native.cpp)
    target_include_directories(lxr_native PRIVATE core generated)
    target_compile_options(lxr_native PRIVATE ${LXR_FP_FLAGS})
endif()

if (LXR_PLUGINS)
    set(CLAP_WRAPPER_ROOT "" CACHE PATH "clap-wrapper checkout (empty: download)")
    if (CLAP_WRAPPER_ROOT STREQUAL "")
        include(FetchContent)
        FetchContent_Declare(clap-wrapper
                GIT_REPOSITORY https://github.com/free-audio/clap-wrapper.git
                GIT_TAG v0.17.0
                GIT_SHALLOW TRUE)
        FetchContent_GetProperties(clap-wrapper)
        if (NOT clap-wrapper_POPULATED)
            FetchContent_Populate(clap-wrapper)
        endif()
        set(CLAP_WRAPPER_ROOT ${clap-wrapper_SOURCE_DIR})
    endif()
    if (NOT DEFINED CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES
            AND (NOT CLAP_SDK_ROOT OR NOT VST3_SDK_ROOT))
        set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES TRUE)
    endif()
    set(CLAP_WRAPPER_OUTPUT_NAME lxr-wrapper)
    add_subdirectory(${CLAP_WRAPPER_ROOT} clap-wrapper EXCLUDE_FROM_ALL)

    add_library(lxr-impl STATIC plugin/lxr_clap.cpp)
    target_link_libraries(lxr-impl PUBLIC lxr-core clap)
    target_compile_definitions(lxr-impl PRIVATE LXR_VERSION="${PROJECT_VERSION}")

    make_clapfirst_plugins(
            TARGET_NAME lxr
            IMPL_TARGET lxr-impl
            OUTPUT_NAME "LXR"
            ENTRY_SOURCE plugin/lxr_clap_entry.cpp
            BUNDLE_IDENTIFIER "io.github.peterswimm.lxr"
            BUNDLE_VERSION ${PROJECT_VERSION}
            COPY_AFTER_BUILD FALSE
            WINDOWS_FOLDER_VST3 TRUE
            PLUGIN_FORMATS ${LXR_PLUGIN_FORMATS}
            ASSET_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins
            AUV2_MANUFACTURER_NAME "LXR port"
            AUV2_MANUFACTURER_CODE "LxrP"
            AUV2_SUBTYPE_CODE "LxrD"
            AUV2_INSTRUMENT_TYPE "aumu"
    )

    if (LXR_TESTS AND NOT WIN32)
        enable_testing()
        add_executable(lxr_clap_test test/clap_host_test.cpp)
        target_link_libraries(lxr_clap_test PRIVATE clap ${CMAKE_DL_LIBS})
        add_dependencies(lxr_clap_test lxr_clap)
        add_test(NAME clap-host COMMAND lxr_clap_test $<TARGET_FILE:lxr_clap>)
    endif()
endif()
