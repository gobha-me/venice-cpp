# Undefined-behavior sanitizer toolchain. Usage:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/undefined.cmake
# Compose with clang via CXX=clang++ (see address.cmake).
#
# VENICE_UBSAN: GCC has no standard UBSan predefine before GCC 14's
# __has_feature. Defining it lets the sanitizer smoke test detect UBSan on
# GCC 13 as well; harmless on Clang / GCC 14+.
#
# Coupled to test/30sanitizer-smoke/test.cpp, which tests defined(VENICE_UBSAN).
# Rename it in both files or neither — check_artifacts.cmake rule B3 fails the
# build if the two sides drift, because a one-sided rename silently compiles the
# UBSan case out on GCC <= 13 while the binary still exits 0.
include(${CMAKE_CURRENT_LIST_DIR}/default.cmake)

message(STATUS "undefined-behavior sanitizer: -fsanitize=undefined")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=undefined -fno-omit-frame-pointer -DVENICE_UBSAN")
set(CMAKE_EXE_LINKER_FLAGS    "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=undefined")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=undefined")
