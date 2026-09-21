find_package(PkgConfig REQUIRED)
pkg_check_modules(FREESWITCH REQUIRED freeswitch)

add_library(mod_audio_fork MODULE
  mod_audio_fork.cpp
)
set_target_properties(mod_audio_fork PROPERTIES PREFIX "" OUTPUT_NAME "mod_audio_fork")
target_include_directories(mod_audio_fork SYSTEM PRIVATE
  ${FREESWITCH_INCLUDE_DIRS}
  $<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>
)
target_compile_options(mod_audio_fork PRIVATE ${FREESWITCH_CFLAGS_OTHER})
# Without this, SWITCH_MODULE_DEFINITION emits the interface struct with no
# visibility attribute, the hidden default hides it, and no version script can
# export it again.
target_compile_definitions(mod_audio_fork PRIVATE SWITCH_API_VISIBILITY=1)
target_link_libraries(mod_audio_fork PRIVATE audiofork_net audiofork_core audiofork_warnings)

# FreeSWITCH resolves switch_* symbols from the running binary at dlopen time,
# so the module must not link libfreeswitch itself. For the same reason there is
# no -Wl,-z,defs: every switch_* reference is deliberately left undefined.
if(APPLE)
  target_link_options(mod_audio_fork PRIVATE
    -undefined dynamic_lookup
    -Wl,-exported_symbol,_mod_audio_fork_module_interface)
else()
  target_link_options(mod_audio_fork PRIVATE
    -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/mod_audio_fork.map)
  set_property(TARGET mod_audio_fork APPEND PROPERTY
    LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/mod_audio_fork.map)
endif()
