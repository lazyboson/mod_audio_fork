add_library(audiofork_core STATIC
  src/slab_pool.cpp
)
target_include_directories(audiofork_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(audiofork_core PRIVATE audiofork_warnings)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
