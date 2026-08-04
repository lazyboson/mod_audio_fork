add_executable(audiofork_net_tests
  test_ws_server.cpp
  ws_client_test.cpp
)
target_link_libraries(audiofork_net_tests
  PRIVATE audiofork_net websockets audiofork_warnings GTest::gtest_main
)
target_include_directories(audiofork_net_tests SYSTEM PRIVATE
  $<TARGET_PROPERTY:websockets,INTERFACE_INCLUDE_DIRECTORIES>
)

include(GoogleTest)
gtest_discover_tests(audiofork_net_tests)
