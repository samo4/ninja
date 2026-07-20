#
# Copyright (c) 2025 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

target_sources_ifdef(CONFIG_APP_AGNSS_DATA app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/agnss_data.c
)
target_include_directories(app PRIVATE .)
