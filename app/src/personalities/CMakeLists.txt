#
# Copyright (c) 2025 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

# Only one personality is compiled — selected by Kconfig choice.
target_sources_ifdef(CONFIG_APP_PERSONALITY_REPORTING app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/reporting.c
)
target_sources_ifdef(CONFIG_APP_PERSONALITY_LOW_POWER_TEST app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/low_power_test.c
)
target_sources_ifdef(CONFIG_APP_PERSONALITY_LOCATION_TEST app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/location_test.c
)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
