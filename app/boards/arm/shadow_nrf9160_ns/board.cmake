#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

board_runner_args(nrfutil "--nrf-family=NRF91")
board_runner_args(jlink "--device=nRF9160_xxAA" "--speed=4000")

include(${ZEPHYR_BASE}/boards/common/nrfutil.board.cmake)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
