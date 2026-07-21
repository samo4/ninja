#
# Copyright (c) 2025 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

target_sources_ifdef(CONFIG_APP_LOCATION_CLOUD app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/location_cloud.c
)
target_include_directories(app PRIVATE .
	${CMAKE_CURRENT_SOURCE_DIR}/../location
)
