# Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.10)
project(org.webosports.service.killswitch C)

include(webOS/webOS)
webos_modules_init(1 0 0 QUALIFIER RC4)
webos_component(1 0 0)

include(FindPkgConfig)

pkg_check_modules(GLIB2 REQUIRED glib-2.0)
pkg_check_modules(LS2 REQUIRED luna-service2)
pkg_check_modules(PBNJSON_C REQUIRED pbnjson_c)

include_directories(${GLIB2_INCLUDE_DIRS} ${LS2_INCLUDE_DIRS} ${PBNJSON_C_INCLUDE_DIRS})

webos_add_compiler_flags(ALL -Wall -Wextra -Wno-unused-parameter)

add_executable(killswitchd src/main.c src/killswitch.c)
target_link_libraries(killswitchd ${GLIB2_LDFLAGS} ${LS2_LDFLAGS} ${PBNJSON_C_LDFLAGS})

# cmake-modules-webos sets this as a CMake variable, not as a C macro, so the
# default config path has to be handed to the compiler explicitly.
target_compile_definitions(killswitchd PRIVATE
                           WEBOS_INSTALL_SYSCONFDIR="${WEBOS_INSTALL_SYSCONFDIR}")

webos_build_daemon(NAME killswitchd LAUNCH files/launch)
webos_build_system_bus_files()

# Helpers a config may point on-blocked/on-open at. Installed unconditionally;
# they gate themselves on the device they are for.
install(PROGRAMS files/helpers/camera-blocked files/helpers/camera-open
        DESTINATION ${WEBOS_INSTALL_LIBEXEC}/killswitchd)
