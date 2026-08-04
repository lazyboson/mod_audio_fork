include(FetchContent)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
  URL_HASH SHA256=8ad598c73ad796e0d8280b082cebd82a630d73e73cd3c70057938a6501bba5d7
)
FetchContent_MakeAvailable(googletest)

add_executable(audiofork_core_tests
  spsc_ring_test.cpp
  slab_pool_test.cpp
  session_state_test.cpp
  session_interleave_test.cpp
)
target_link_libraries(audiofork_core_tests
  PRIVATE audiofork_core audiofork_warnings GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(audiofork_core_tests)
