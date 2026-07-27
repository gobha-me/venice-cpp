# Address sanitizer toolchain. Usage:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/address.cmake
# Compose with clang by also selecting the compiler (default.cmake respects the
# environment; you cannot pass two toolchain files):
#   CXX=clang++ cmake -B build-asan-clang \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/address.cmake
include(${CMAKE_CURRENT_LIST_DIR}/default.cmake)

message(STATUS "address sanitizer: -fsanitize=address")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
set(CMAKE_EXE_LINKER_FLAGS    "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=address")
