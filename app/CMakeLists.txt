#
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project("Ninja")

zephyr_include_directories(src/common)

target_sources(app PRIVATE src/main.c)

add_subdirectory(src/personalities)
add_subdirectory(src/modules/network)

# Optional modules
add_subdirectory_ifdef(CONFIG_APP_ENVIRONMENTAL src/modules/environmental)
add_subdirectory_ifdef(CONFIG_APP_LED src/modules/led)
add_subdirectory_ifdef(CONFIG_APP_LOCATION src/modules/location)
add_subdirectory_ifdef(CONFIG_APP_CLOUD_POST src/modules/cloud_post)
add_subdirectory_ifdef(CONFIG_APP_AGNSS_DATA src/modules/agnss_data)
