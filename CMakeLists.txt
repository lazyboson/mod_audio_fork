cmake_minimum_required(VERSION 3.25)
project(mod_audio_fork LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# core/ and net/ are static archives linked into a shared module, so every
# object in them must be position-independent (implicit on macOS, not on Linux).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# Same reason for visibility: the archives' objects carry into the .so, so
# hiding only the module's own objects would still export core/ and net/.
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

add_library(audiofork_warnings INTERFACE)
target_compile_options(audiofork_warnings INTERFACE -Wall -Wextra -Werror -Wshadow)

option(AUDIOFORK_FUZZ "Build libFuzzer targets (requires clang)" OFF)
option(AUDIOFORK_BUILD_NET "Build the libwebsockets shim" ON)

if(AUDIOFORK_BUILD_NET)
  # Homebrew keeps openssl@3 off the default search path, and the vendored lws
  # build resolves OpenSSL through the same cache variables this sets.
  if(APPLE AND NOT DEFINED OPENSSL_ROOT_DIR)
    execute_process(COMMAND brew --prefix openssl@3
      OUTPUT_VARIABLE AUDIOFORK_BREW_OPENSSL OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
    if(AUDIOFORK_BREW_OPENSSL)
      set(OPENSSL_ROOT_DIR "${AUDIOFORK_BREW_OPENSSL}" CACHE PATH "")
    endif()
  endif()
  # Never static: a second OpenSSL inside the module would collide with the one
  # FreeSWITCH already has loaded (DESIGN.md §13).
  set(OPENSSL_USE_STATIC_LIBS OFF)
  find_package(OpenSSL REQUIRED)
endif()

# The module needs FreeSWITCH headers; default to building it only where they
# are present so core/ and net/ stay buildable anywhere.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(FREESWITCH_PROBE QUIET freeswitch)
endif()
option(AUDIOFORK_BUILD_MODULE "Build the FreeSWITCH module" ${FREESWITCH_PROBE_FOUND})

include(CTest)
add_subdirectory(core)
if(AUDIOFORK_BUILD_NET)
  add_subdirectory(net)
endif()
if(AUDIOFORK_BUILD_MODULE)
  if(NOT AUDIOFORK_BUILD_NET)
    message(FATAL_ERROR "AUDIOFORK_BUILD_MODULE requires AUDIOFORK_BUILD_NET")
  endif()
  add_subdirectory(module)
endif()
