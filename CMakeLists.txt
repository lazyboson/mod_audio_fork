cmake_minimum_required(VERSION 3.25)
project(mod_audio_fork LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(audiofork_warnings INTERFACE)
target_compile_options(audiofork_warnings INTERFACE -Wall -Wextra -Werror -Wshadow)

option(AUDIOFORK_FUZZ "Build libFuzzer targets (requires clang)" OFF)
option(AUDIOFORK_BUILD_NET "Build the libwebsockets shim" ON)

include(CTest)
add_subdirectory(core)
if(AUDIOFORK_BUILD_NET)
  add_subdirectory(net)
endif()
