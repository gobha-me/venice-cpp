# Thread sanitizer toolchain. Usage:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/thread.cmake
# TSan is usually built at -O2 for usable performance and readable stacks. We do
# NOT force it here (that would clobber the Debug default's -O0 -g). Opt in per
# build with a release-ish build type:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/thread.cmake \
#     -DCMAKE_BUILD_TYPE=RelWithDebInfo
# Compose with clang via CXX=clang++ (see address.cmake).
include(${CMAKE_CURRENT_LIST_DIR}/default.cmake)

message(STATUS "thread sanitizer: -fsanitize=thread")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=thread")
set(CMAKE_EXE_LINKER_FLAGS    "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=thread")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=thread")
