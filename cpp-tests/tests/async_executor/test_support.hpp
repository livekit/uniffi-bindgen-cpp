#pragma once

#include "async_calls.hpp"
#include "async_test_support.hpp"

namespace executor_tests {

void test_rust_future_lifecycle();
void test_shutdown_during_handle_creation();

} // namespace executor_tests
