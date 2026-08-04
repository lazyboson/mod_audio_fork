cmake_minimum_required(VERSION 3.25)
project(mod_audio_fork LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# core/ and net/ are static archives linked into a shared module, so every
# object in them must be position-independent (implicit on macOS, not on Linux).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(audiofork_warnings INTERFACE)
target_compile_options(audiofork_warnings INTERFACE -Wall -Wextra -Werror -Wshadow)

option(AUDIOFORK_FUZZ "Build libFuzzer targets (requires clang)" OFF)
option(AUDIOFORK_BUILD_NET "Build the libwebsockets shim" ON)

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
