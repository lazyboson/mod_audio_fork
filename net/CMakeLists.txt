include(FetchContent)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(LWS_WITH_SSL ON CACHE BOOL "" FORCE)
set(LWS_WITH_OPENSSL ON CACHE BOOL "" FORCE)
set(LWS_WITH_MBEDTLS OFF CACHE BOOL "" FORCE)
set(LWS_SSL_CLIENT_USE_OS_CA_CERTS ON CACHE BOOL "" FORCE)
set(LWS_WITH_SHARED OFF CACHE BOOL "" FORCE)
set(LWS_WITH_STATIC ON CACHE BOOL "" FORCE)
set(LWS_WITHOUT_TESTAPPS ON CACHE BOOL "" FORCE)
set(LWS_WITHOUT_TEST_SERVER ON CACHE BOOL "" FORCE)
set(LWS_WITHOUT_TEST_PING ON CACHE BOOL "" FORCE)
set(LWS_WITHOUT_TEST_CLIENT ON CACHE BOOL "" FORCE)
set(LWS_WITH_MINIMAL_EXAMPLES OFF CACHE BOOL "" FORCE)
# Linux-only kernel route monitoring; unused by us, and its POLLIN handler
# races on lws globals when several contexts run on their own threads
set(LWS_WITH_NETLINK OFF CACHE BOOL "" FORCE)
# REQUIRED for the sharded design: lws guards its process-global bookkeeping
# (log-context refcount, lifecycle tag list, touched by every wsi create and
# destroy) with a mutex only when built SMP-aware. At MAX_SMP=1 those counters
# are unguarded, and one context per shard thread corrupts them — asserting in
# debug builds and silently miscounting in release.
set(LWS_MAX_SMP 8 CACHE STRING "" FORCE)

FetchContent_Declare(libwebsockets
  URL https://github.com/warmcat/libwebsockets/archive/refs/tags/v4.3.3.tar.gz
  URL_HASH SHA256=6fd33527b410a37ebc91bb64ca51bdabab12b076bc99d153d7c5dd405e4bdf90
  PATCH_COMMAND ${CMAKE_COMMAND}
    -DLWS_SOURCE_DIR=<SOURCE_DIR>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/patch_lws_client_http.cmake
)
FetchContent_MakeAvailable(libwebsockets)

add_library(audiofork_net STATIC
  src/shard_pool.cpp
  src/ws_client.cpp
)
target_include_directories(audiofork_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(audiofork_net SYSTEM PRIVATE
  $<TARGET_PROPERTY:websockets,INTERFACE_INCLUDE_DIRECTORIES>
)
target_link_libraries(audiofork_net
  PUBLIC audiofork_core
  PRIVATE websockets OpenSSL::SSL OpenSSL::Crypto audiofork_warnings
)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
