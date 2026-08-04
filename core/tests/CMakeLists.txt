include(FetchContent)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
  URL_HASH SHA256=8ad598c73ad796e0d8280b082cebd82a630d73e73cd3c70057938a6501bba5d7
)
FetchContent_MakeAvailable(googletest)

add_executable(audiofork_core_tests
  backoff_test.cpp
  config_test.cpp
  fork_session_test.cpp
  protocol_test.cpp
  send_buffer_test.cpp
  session_interleave_test.cpp
  session_state_test.cpp
  slab_pool_test.cpp
  spsc_ring_test.cpp
)
target_link_libraries(audiofork_core_tests
  PRIVATE audiofork_core audiofork_warnings GTest::gtest_main
)
target_include_directories(audiofork_core_tests SYSTEM PRIVATE
  $<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>
)

include(GoogleTest)
gtest_discover_tests(audiofork_core_tests)

if(AUDIOFORK_FUZZ)
  add_executable(protocol_fuzz fuzz/protocol_fuzz.cpp)
  target_link_libraries(protocol_fuzz PRIVATE audiofork_core audiofork_warnings)
  target_compile_options(protocol_fuzz PRIVATE -fsanitize=fuzzer)
  target_link_options(protocol_fuzz PRIVATE -fsanitize=fuzzer)
endif()
