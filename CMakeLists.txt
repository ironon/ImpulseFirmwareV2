# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

# Board V1 lives in this tree, not in Zephyr's.
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(impulse_firmware LANGUAGES C)

target_include_directories(app PRIVATE src)

target_sources(app PRIVATE
    src/main.c
    src/schedule/event.c
    src/schedule/schedule_blob.c
    src/schedule/schedule.c
    src/integrity/classify.c
    src/integrity/integrity.c
    src/enforcement/profiles.c
    src/enforcement/enforcement.c
    src/proximity/proximity.c
    src/proximity/cs_fuse.c
    src/storage/storage.c
    src/identity.c
    src/hal/outputs.c
    src/hal/led_ring.c
    src/hal/buttons.c
    src/hal/adc.c
    src/hal/servo.c
)

target_sources_ifdef(CONFIG_IMPULSE_ROLE_ANCHOR app PRIVATE
    src/anchor/anchor.c
)

# One BLE module per role: the two services are disjoint and neither device
# needs the other's characteristics.
if(CONFIG_IMPULSE_BLE)
  if(CONFIG_IMPULSE_ROLE_ANCHOR)
    target_sources(app PRIVATE src/ble/ble_anchor.c)
  else()
    target_sources(app PRIVATE src/ble/ble_watch.c)
  endif()
endif()

target_sources_ifdef(CONFIG_IMPULSE_CS_BACKEND app PRIVATE
    src/proximity/cs_backend_nrf.c
)
# The reflector is the anchor's half of the pair. Both files define
# impulse_cs_backend_start(), so exactly one of them may be compiled.
target_sources_ifdef(CONFIG_IMPULSE_CS_REFLECTOR app PRIVATE
    src/proximity/cs_reflector_nrf.c
)
target_sources_ifdef(CONFIG_IMPULSE_NET app PRIVATE
    src/net/net_wifi.c
    src/net/net_udp.c
)

target_sources_ifdef(CONFIG_IMPULSE_BRINGUP_SHELL app PRIVATE
    src/hal/bringup_shell.c
)
