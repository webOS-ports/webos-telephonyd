# @@@LICENSE
#
# Copyright (c) 2012 Simon Busch <morphis@gravedo.de>
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# LICENSE@@@

cmake_minimum_required(VERSION 2.8.7)

project(telephonyd C)

include(webOS/webOS)
webos_modules_init(1 0 0 QUALIFIER RC2)
webos_component(0 1 0)

include(FindPkgConfig)

pkg_check_modules(GLIB2 REQUIRED glib-2.0)
include_directories(${GLIB2_INCLUDE_DIRS})
webos_add_compiler_flags(ALL ${GLIB2_CFLAGS})

pkg_check_modules(GIO2 REQUIRED gio-2.0)
include_directories(${GIO2_INCLUDE_DIRS})
webos_add_compiler_flags(ALL ${GIO2_CFLAGS})

pkg_check_modules(GOBJECT2 REQUIRED gobject-2.0)
include_directories(${GOBJECT2_INCLUDE_DIRS})
webos_add_compiler_flags(ALL ${GOBJECT2_CFLAGS})

pkg_check_modules(LUNASERVICE2 REQUIRED luna-service2)
include_directories(${LUNASERVICE2_INCLUDE_DIRS})
webos_add_compiler_flags(ALL ${LUNASERVICE2_CFLAGS})

pkg_check_modules(CJSON REQUIRED cjson)
include_directories(${CJSON_INCLUDE_DIRS})
webos_add_compiler_flags(ALL ${CJSON_CFLAGS})

webos_add_compiler_flags(ALL -Wall)
webos_add_linker_options(ALL --no-undefined)

include_directories(src src/ofono)
file(GLOB SOURCE_FILES src/*.c src/ofono/*.c)

add_executable(telephonyd ${SOURCE_FILES})
target_link_libraries(telephonyd
    ${GLIB2_LDFLAGS} ${LUNASERVICE2_LDFLAGS} ${CJSON_LDFLAGS}
    ${GIO2_LDFLAGS} ${GOBJECT2_LDFLAGS}
    rt pthread)

webos_build_daemon()
webos_build_system_bus_files()
