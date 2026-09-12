# SPDX-License-Identifier: Apache-2.0
#
# Board V1 has no on-board debugger. Flashing is over SWD through the TC2030-NL
# footprint (J2), from an external probe — in practice an nRF54L15-DK's
# DEBUG OUT. There is no USB CDC on this board, so RTT is the only console.
board_runner_args(jlink "--device=nRF54LM20A_M33" "--speed=1000")
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
