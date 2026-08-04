include(FetchContent)

FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
)
FetchContent_MakeAvailable(nlohmann_json)

add_library(audiofork_core STATIC
  src/protocol.cpp
  src/slab_pool.cpp
)
target_include_directories(audiofork_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(audiofork_core SYSTEM PRIVATE
  $<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>
)
target_link_libraries(audiofork_core PRIVATE audiofork_warnings)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
