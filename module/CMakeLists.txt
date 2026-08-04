find_package(PkgConfig REQUIRED)
pkg_check_modules(FREESWITCH REQUIRED freeswitch)

add_library(mod_audio_fork MODULE
  mod_audio_fork.cpp
)
set_target_properties(mod_audio_fork PROPERTIES PREFIX "" OUTPUT_NAME "mod_audio_fork")
target_include_directories(mod_audio_fork SYSTEM PRIVATE ${FREESWITCH_INCLUDE_DIRS})
target_compile_options(mod_audio_fork PRIVATE ${FREESWITCH_CFLAGS_OTHER})
target_link_libraries(mod_audio_fork PRIVATE audiofork_net audiofork_core audiofork_warnings)

# FreeSWITCH resolves switch_* symbols from the running binary at dlopen time,
# so the module must not link libfreeswitch itself.
if(APPLE)
  target_link_options(mod_audio_fork PRIVATE -undefined dynamic_lookup)
endif()
