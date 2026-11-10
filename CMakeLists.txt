# lime-juice: C++ port of Tomyun's "Juice" de/recompiler for PC-98 games
# Copyright (C) 2026 Fuzion
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

cmake_minimum_required(VERSION 3.16)
project(juice VERSION 0.2.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Iconv)

include(GNUInstallDirs)

# shared CLI utilities (used by both juice and juice-img)
add_library(cli_utils STATIC
    src/cli_utils.cpp
)
target_include_directories(cli_utils PUBLIC src)

# core library (all logic except main)
add_library(juice_lib STATIC
    src/config.cpp
    src/charset.cpp
    src/ast.cpp
    src/sexp_writer.cpp
    src/sexp_reader.cpp
    src/charset/pc98.cpp
    src/charset/english.cpp
    src/charset/europe.cpp
    src/charset/korean.cpp
    src/charset/korean_gamebox.cpp
    src/charset/korean_hannuri.cpp
    src/charset/korean_kk.cpp
    src/charset/korean_parangsae.cpp
    src/charset/chinese.cpp
    src/engine/ai5/opener.cpp
    src/engine/ai5/parser.cpp
    src/engine/ai5/loader.cpp
    src/engine/ai5/compiler.cpp
    src/engine/ai1/opener.cpp
    src/engine/ai1/parser.cpp
    src/engine/ai1/loader.cpp
    src/engine/ai1/compiler.cpp
    src/engine/adv/opener.cpp
    src/engine/adv/parser.cpp
    src/engine/adv/loader.cpp
    src/engine/adv/compiler.cpp
    src/auto_detect.cpp
    src/auto_wrap.cpp
)
target_include_directories(juice_lib PUBLIC src)

if(Iconv_FOUND)
    target_link_libraries(juice_lib PUBLIC Iconv::Iconv)
    target_compile_definitions(juice_lib PUBLIC HAVE_ICONV)
elseif(WIN32)
    # use vendored win-iconv on Windows when system iconv is not available
    add_library(win_iconv STATIC third_party/iconv/win_iconv.c)
    target_include_directories(win_iconv PUBLIC third_party/iconv)
    target_link_libraries(juice_lib PUBLIC win_iconv)
    target_compile_definitions(juice_lib PUBLIC HAVE_ICONV)
endif()

# main executable
add_executable(juice src/main.cpp)
target_link_libraries(juice PRIVATE juice_lib cli_utils)

# statically link gcc/c++ runtime on MinGW so users don't need the dlls
if(MINGW)
    target_link_options(juice PRIVATE -static)
endif()

# image codec library (separate from juice_lib, no shared dependencies)
add_library(juice_img_lib STATIC
    src/image/image.cpp
    src/image/png_io.cpp
    src/image/gif_io.cpp
    src/codec/gpc/gpc_decode.cpp
    src/codec/gpc/gpc_encode.cpp
    src/codec/gpa/gpa_decode.cpp
    src/codec/gpa/gpa_encode.cpp
    src/codec/gp4/gp4_decode.cpp
    src/codec/gp4/gp4_encode.cpp
    src/codec/csr/csr_decode.cpp
    src/codec/csr/csr_encode.cpp
    third_party/lodepng/lodepng.cpp
    third_party/lecram/gifdec.c
    third_party/lecram/gifenc.c
)
target_include_directories(juice_img_lib PUBLIC src third_party)

# image tool executable
add_executable(juice-img src/img_main.cpp)
target_link_libraries(juice-img PRIVATE juice_img_lib cli_utils)

if(MINGW)
    target_link_options(juice-img PRIVATE -static)
endif()

install(TARGETS juice juice-img)
