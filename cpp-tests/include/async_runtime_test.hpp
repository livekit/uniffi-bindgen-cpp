#pragma once

#include "async_test_support.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

// Use the same runtime fragment and namespace as a generated binding header.
namespace uniffi {
inline namespace cpp_async_v1 {
#include "async.hpp"
}
}
