# DirectorySize (C++) — Copyright (c) 2026 META.vn
# Author: Nguyễn Đức Sơn
# SPDX-License-Identifier: GPL-2.0-only
cmake_minimum_required(VERSION 3.16)
project(dirsize LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(UNIX)
  add_compile_definitions(_FILE_OFFSET_BITS=64)
endif()

if(WIN32)
  enable_language(RC)
  add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra)
endif()
if(MSVC)
  add_compile_options(/utf-8 /W3 /permissive- /D_CRT_SECURE_NO_WARNINGS)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(Threads REQUIRED)

# Shared application icon (repo-level assets/icon/dirsize.ico), embedded by the .rc files.
set(DIRSIZE_ICON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../assets/icon)

# Link libstdc++/libgcc statically with MinGW so the .exe runs stand-alone.
option(DIRSIZE_STATIC "Statically link the C++ runtime (MinGW)" ON)
function(dirsize_static target)
  if(DIRSIZE_STATIC AND MINGW)
    target_link_options(${target} PRIVATE -static -static-libgcc -static-libstdc++)
  endif()
endfunction()

# ---- Tier 1: OS-independent core --------------------------------------------
add_library(dirsize_core STATIC
  core/size_format.cpp
  core/size_tree.cpp
  core/scanner.cpp)
target_include_directories(dirsize_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dirsize_core PUBLIC Threads::Threads)

# ---- Tier 2: platform directory walkers -------------------------------------
if(WIN32)
  set(DIRSIZE_WALKER_SRC platform/win_walker.cpp)
else()
  # Linux walker (built and tested in Docker, see scripts/test-linux.sh).
  set(DIRSIZE_WALKER_SRC platform/posix_walker.cpp)
endif()
add_library(dirsize_platform STATIC ${DIRSIZE_WALKER_SRC})
target_link_libraries(dirsize_platform PUBLIC dirsize_core)

# ---- Tier 3: applications ---------------------------------------------------
if(WIN32)
  set(DIRSIZE_CLI_RC apps/cli/dirsize-cli.rc)
endif()
add_executable(dirsize-cli apps/cli/main.cpp ${DIRSIZE_CLI_RC})
if(WIN32)
  target_include_directories(dirsize-cli PRIVATE ${DIRSIZE_ICON_DIR})
endif()
target_link_libraries(dirsize-cli PRIVATE dirsize_platform)
dirsize_static(dirsize-cli)

if(WIN32)
  add_executable(dirsize-gui WIN32 apps/gui/main.cpp apps/gui/dirsize-gui.rc)
  target_include_directories(dirsize-gui PRIVATE ${DIRSIZE_ICON_DIR})
  target_link_libraries(dirsize-gui PRIVATE dirsize_platform comctl32 ole32 uuid shell32)
  dirsize_static(dirsize-gui)
  if(MSVC)
    # The .rc already embeds our manifest; stop the linker adding a second one.
    target_link_options(dirsize-gui PRIVATE /MANIFEST:NO)
  endif()
endif()

# ---- Tests ------------------------------------------------------------------
enable_testing()
add_executable(test_core tests/test_core.cpp)
target_link_libraries(test_core PRIVATE dirsize_core)
dirsize_static(test_core)
add_test(NAME core COMMAND test_core)
set_tests_properties(core PROPERTIES TIMEOUT 300)

add_executable(test_walker tests/test_walker.cpp)
target_link_libraries(test_walker PRIVATE dirsize_platform)
dirsize_static(test_walker)
add_test(NAME walker COMMAND test_walker)
set_tests_properties(walker PROPERTIES TIMEOUT 300)

add_executable(test_walker_extra tests/test_walker_extra.cpp)
target_link_libraries(test_walker_extra PRIVATE dirsize_platform)
dirsize_static(test_walker_extra)
add_test(NAME walker_extra COMMAND test_walker_extra)
set_tests_properties(walker_extra PROPERTIES TIMEOUT 300)

add_executable(test_cli tests/test_cli.cpp)
dirsize_static(test_cli)
add_test(NAME cli_ctrl_break COMMAND test_cli $<TARGET_FILE:dirsize-cli>)
set_tests_properties(cli_ctrl_break PROPERTIES TIMEOUT 120)
