#pragma once

// venice-cpp — header-only Venice API client.
//
//   #include <venice/venice.hpp>
//
// Link: venice-cpp is header-only (INTERFACE). Consumers need cpp-httplib,
// nlohmann/json, and OpenSSL — provided transitively when linking the
// `venice-cpp::lib` CMake target.

#include "venice/error.hpp"
#include "venice/types.hpp"
#include "venice/client.hpp"
