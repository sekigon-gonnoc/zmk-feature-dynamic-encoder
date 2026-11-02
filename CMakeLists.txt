# SPDX-License-Identifier: MIT

if(CONFIG_ZMK_DYNAMIC_ENCODER)
  zephyr_include_directories(include)
  target_sources(app PRIVATE src/binding_dispatch.c)
  if(CONFIG_ZMK_DYNAMIC_ENCODER_RUNTIME)
    target_sources(app PRIVATE src/encoder.c)
    zephyr_linker_sources(SECTIONS linker/dynamic_encoder_binding_provider.ld)
  endif()
  if(CONFIG_ZMK_DYNAMIC_ENCODER_RUNTIME AND CONFIG_ZMK_DYNAMIC_SETTINGS_SCHEMA)
    set_property(GLOBAL APPEND PROPERTY DYNAMIC_SETTINGS_SCHEMA_CONTRIBUTORS
      "${CMAKE_CURRENT_SOURCE_DIR}/scripts/settings_schema.py")
  endif()
endif()
