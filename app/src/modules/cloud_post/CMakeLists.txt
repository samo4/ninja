#
# Copyright (c) 2025 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

target_sources_ifdef(CONFIG_APP_CLOUD_POST app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/cloud_post.c
)
target_include_directories(app PRIVATE .)
