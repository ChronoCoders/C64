# Cross-platform build for the C64 emulator. Mirrors the Makefile: a no-SDL
# emulation core, an SDL2 GUI binary, the Lorenz runner, and the unit suites.
# The Makefile stays the canonical Linux dev build; this is the portable path
# for macOS and Windows (MSVC or MinGW), where sdl2-config and the POSIX-shell
# recipes do not apply.
#
#   cmake -B build && cmake --build build          # core + GUI (if SDL2) + tests
#   ctest --test-dir build                          # unit suites (fast group)
#   ctest --test-dir build -L slow                  # the slow integration group
#
cmake_minimum_required(VERSION 3.16)
project(c64 LANGUAGES C)

# Strict C11, no compiler extensions: matches -std=c11 (not gnu11). The core uses
# no GCC/Clang extensions, so this holds on MSVC too.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Default to an optimised build when the generator is single-config.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(C64_WERROR "Treat warnings as errors (project policy is zero warnings)" ON)
option(C64_BUILD_TESTS "Build the unit suites and the Lorenz runner" ON)
option(C64_DEBUG_TOOLS "Compile the bus-watchpoint / snapshot hooks in src/debug.c" OFF)

# Warnings per compiler. -Werror / /WX can trip on a compiler emitting a warning
# the code was never checked against (clang and MSVC differ from gcc); flip
# -DC64_WERROR=OFF while porting, then clean the warnings and turn it back on.
if(MSVC)
  add_compile_options(/W4)
  if(C64_WERROR)
    add_compile_options(/WX)
  endif()
else()
  add_compile_options(-Wall -Wextra)
  if(C64_WERROR)
    add_compile_options(-Werror)
  endif()
endif()

# The emulation core: no SDL, no OS calls. Shared by the GUI binary and every
# test. This is the portable heart; only the pieces below touch a platform.
set(CORE_SOURCES
  src/bus.c src/mem.c src/cpu.c src/cpu6502.c src/vic.c src/sid.c
  src/cia.c src/drive.c src/via.c src/iec.c src/disk.c src/debug.c src/snapshot.c)

add_library(c64core STATIC ${CORE_SOURCES})
target_include_directories(c64core PUBLIC src)
if(C64_DEBUG_TOOLS)
  target_compile_definitions(c64core PUBLIC DEBUG_TOOLS=1)
endif()

# SDL2 GUI binary. Optional: if SDL2 is absent the tests still build, matching the
# Makefile's "the headless test runner still builds" behaviour. find_package(SDL2)
# is the portable discovery path (Homebrew, vcpkg, MSYS2 and apt all ship the
# config package), replacing the Linux-only sdl2-config.
find_package(SDL2 QUIET)
if(SDL2_FOUND OR TARGET SDL2::SDL2)
  add_executable(c64 src/main.c src/host.c)
  target_link_libraries(c64 PRIVATE c64core)
  # This program supplies its own main(). SDL_MAIN_HANDLED stops SDL from
  # redefining main / expecting an SDL_main entry, so the same plain main() links
  # on Linux, macOS and Windows without linking SDL2main. Note: with this set, SDL
  # wants SDL_SetMainReady() before SDL_Init on some platforms; if a window fails
  # to open on macOS/Windows, that one call in main.c (or linking SDL2main and
  # including <SDL.h> in main.c instead) is the lever.
  target_compile_definitions(c64 PRIVATE SDL_MAIN_HANDLED)
  if(TARGET SDL2::SDL2)
    target_link_libraries(c64 PRIVATE SDL2::SDL2)
  else()
    target_include_directories(c64 PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(c64 PRIVATE ${SDL2_LIBRARIES})
  endif()
  # Windows: place SDL2.dll beside the .exe so it runs from the build tree.
  if(WIN32 AND TARGET SDL2::SDL2)
    add_custom_command(TARGET c64 POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:c64>)
  endif()
else()
  message(STATUS
    "SDL2 not found: skipping the 'c64' GUI binary; tests still build. "
    "Install SDL2 (brew install sdl2 / apt install libsdl2-dev / vcpkg install sdl2).")
endif()

# Wolfgang Lorenz CPU-conformance runner. Core only, no SDL. Not registered as a
# ctest by default: the suite files live under test/lorenz (gitignored, often
# absent) and the runner exits non-zero when they are missing. Run it directly:
#   ./build/lorenz-runner   (or build/Release/lorenz-runner on multi-config)
add_executable(lorenz-runner test/runner.c)
target_link_libraries(lorenz-runner PRIVATE c64core)
target_include_directories(lorenz-runner PRIVATE src test)

# Note: the Makefile remains the canonical Linux dev gate. The sanitizer, coverage,
# valgrind and fuzz targets (make test-asan / coverage / valgrind / fuzz) stay
# there: they depend on gcc/clang/valgrind tooling with no portable equivalent and
# are not reimplemented here. This file covers the portable build plus the three
# test tiers (make test / test-slow / test-cpu) via CTest.
if(C64_BUILD_TESTS)
  enable_testing()

  # The suites read rom/*.rom and write build/*.d64 as paths relative to the repo
  # root (the Makefile runs them from there). Run every test with that working
  # directory and ensure the build/ scratch dir the suites write into exists.
  file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/build)

  # Most suites link the plain core. The debug suite needs the core built with the
  # hooks on, so it links a DEBUG_TOOLS=1 variant (PUBLIC, so the test TU sees the
  # define too), matching the Makefile's dedicated test-debug recipe.
  add_library(c64core_dbg STATIC ${CORE_SOURCES})
  target_include_directories(c64core_dbg PUBLIC src)
  target_compile_definitions(c64core_dbg PUBLIC DEBUG_TOOLS=1)

  set(UNIT_TESTS mem cpu cia sid vic drive via iec gcr debug snapshot)
  foreach(t IN LISTS UNIT_TESTS)
    add_executable(test-${t} test/${t}_test.c)
    target_include_directories(test-${t} PRIVATE src test)
    if(t STREQUAL "debug")
      target_link_libraries(test-${t} PRIVATE c64core_dbg)
    else()
      target_link_libraries(test-${t} PRIVATE c64core)
    endif()
    # No argument runs the fast group; the drive and iec suites take "slow" for
    # their DOS/serial integration group.
    add_test(NAME ${t} COMMAND test-${t})
    set_tests_properties(${t} PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  endforeach()

  foreach(t IN ITEMS drive iec)
    add_test(NAME ${t}-slow COMMAND test-${t} slow)
    set_tests_properties(${t}-slow PROPERTIES
      LABELS slow WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  endforeach()
endif()
