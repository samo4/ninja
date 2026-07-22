#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

target_sources(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/motion.c
	${ZEPHYR_BASE}/../modules/hal/st/sensor/stmemsc/lis2dtw12_STdC/driver/lis2dtw12_reg.c
)
target_include_directories(app PRIVATE .
	${ZEPHYR_BASE}/../modules/hal/st/sensor/stmemsc/lis2dtw12_STdC/driver
    ${ZEPHYR_BASE}/drivers/sensor/st/lis2dw12
    ${ZEPHYR_BASE}/drivers/sensor/st/stmemsc
)
