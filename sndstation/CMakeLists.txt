# LXR as a sndstation app: the handheld build of the LXR port in native/.
#
#   cmake -S sndstation -B sndstation/build -DSNDSTATION_DIR=/path/to/sndstation
#   cmake --build sndstation/build -j4
#   ./sndstation/build/lxr-sndstation
#
# The sndstation platform is built from its source tree alongside, as every sndstation
# app is (its examples/hello). On a handheld, or for one, see README.md.
cmake_minimum_required(VERSION 3.16)
project(lxr-sndstation VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(SNDSTATION_DIR "" CACHE PATH "the sndstation source tree")
if (NOT TARGET sndstation::sndstation)
    if (NOT SNDSTATION_DIR OR NOT EXISTS "${SNDSTATION_DIR}/include/sndstation/sndstation.h")
        message(FATAL_ERROR "Point -DSNDSTATION_DIR= at a sndstation checkout (with its submodules)")
    endif()
    add_subdirectory(${SNDSTATION_DIR} ${CMAKE_CURRENT_BINARY_DIR}/sndstation EXCLUDE_FROM_ALL)
endif()

# The LXR engine, sequencer and instrument, from native/: the same code as the plugins.
# It emulates the firmware's float32 arithmetic with doubles, so no fused multiply-adds
# and no fast math (aarch64 GCC contracts into FMAs by default).
set(LXR_NATIVE ${CMAKE_CURRENT_SOURCE_DIR}/../native)
add_library(lxr-core STATIC ${LXR_NATIVE}/core/LxrInstrument.cpp)
target_include_directories(lxr-core PUBLIC ${LXR_NATIVE}/core ${LXR_NATIVE}/generated)
target_compile_options(lxr-core PUBLIC -ffp-contract=off -fno-fast-math)

# The typeface: Open Sans, Regular and Bold, unmodified static TrueType files with their
# license (SIL OFL 1.1), from googlefonts/opensans at bd7e37632246368c60fdcbd374dbf9bad11969b6
# (the files the synthstation video demo ships). The app draws in nothing else; see
# src/Fonts.hpp. Checked here, so a changed file is a failed configure, not a new look.
set(LXR_FONT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/fonts)
set(LXR_FONT_FILES OpenSans-Regular.ttf OpenSans-Bold.ttf OpenSans-OFL.txt)
set(LXR_FONT_SHA256_OpenSans-Regular.ttf c53aceea2dcf5b4098099c0c4d0a061d17e178a049317b42a422b1a9f7f8eb59)
set(LXR_FONT_SHA256_OpenSans-Bold.ttf 27da758f4dcac9a65abe914c13b463b42982b9909bc65713424099f4810bd1e6)
set(LXR_FONT_SHA256_OpenSans-OFL.txt 01cd5ffb3a528c219a86e49a814c6c53bd8a69b9a3c305dd237074ba1c811af7)
foreach (f ${LXR_FONT_FILES})
    file(SHA256 ${LXR_FONT_DIR}/${f} sum)
    if (NOT sum STREQUAL LXR_FONT_SHA256_${f})
        message(FATAL_ERROR "fonts/${f} is not the Open Sans file it should be (SHA-256 ${sum})")
    endif()
endforeach()

add_library(lxr-app STATIC src/Engine.cpp src/Pages.cpp src/Session.cpp src/App.cpp src/Fonts.cpp)
target_include_directories(lxr-app PUBLIC src)
target_link_libraries(lxr-app PUBLIC lxr-core sndstation::sndstation)
target_compile_options(lxr-app PRIVATE -Wall -Wextra)
target_compile_definitions(lxr-app PRIVATE LXR_SOURCE_FONT_DIR="${LXR_FONT_DIR}")

add_executable(lxr-sndstation src/main.cpp)
target_link_libraries(lxr-sndstation PRIVATE lxr-app)
target_compile_options(lxr-sndstation PRIVATE -Wall -Wextra)

option(LXR_SNDSTATION_TESTS "Build the tests" ON)
if (LXR_SNDSTATION_TESTS)
    enable_testing()
    add_executable(test_lxr_sndstation tests/test_lxr_sndstation.cpp)
    target_link_libraries(test_lxr_sndstation PRIVATE lxr-app)
    add_test(NAME lxr-sndstation-engine COMMAND test_lxr_sndstation)
    add_executable(test_lxr_controls tests/test_lxr_controls.cpp)
    target_link_libraries(test_lxr_controls PRIVATE lxr-app)
    add_test(NAME lxr-sndstation-controls COMMAND test_lxr_controls)
    add_test(NAME lxr-sndstation-shots
            COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:lxr-sndstation> -DDIR=${CMAKE_CURRENT_BINARY_DIR}/shots
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/shots.cmake)
    # without Open Sans the app refuses to start, rather than draw in another face
    add_test(NAME lxr-sndstation-no-fonts
            COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:lxr-sndstation> -DDIR=${CMAKE_CURRENT_BINARY_DIR}/no-fonts
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/no-fonts.cmake)
endif()

include(GNUInstallDirs)
install(TARGETS lxr-sndstation RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES lxr.json DESTINATION share/sndstation/apps)
# beside bin/, where src/Fonts.cpp looks: <bin>/../share/lxr-sndstation/fonts
list(TRANSFORM LXR_FONT_FILES PREPEND ${LXR_FONT_DIR}/ OUTPUT_VARIABLE LXR_FONT_PATHS)
install(FILES ${LXR_FONT_PATHS} DESTINATION share/lxr-sndstation/fonts)
