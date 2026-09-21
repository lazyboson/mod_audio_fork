add_executable(audiofork_net_tests
  shard_pool_test.cpp
  test_ws_server.cpp
  ws_client_test.cpp
  ws_tls_test.cpp
)
target_compile_definitions(audiofork_net_tests
  PRIVATE AUDIOFORK_TEST_TLS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tls"
)
target_link_libraries(audiofork_net_tests
  PRIVATE audiofork_net websockets audiofork_warnings GTest::gtest_main
)
target_include_directories(audiofork_net_tests SYSTEM PRIVATE
  $<TARGET_PROPERTY:websockets,INTERFACE_INCLUDE_DIRECTORIES>
)

include(GoogleTest)
gtest_discover_tests(audiofork_net_tests)
