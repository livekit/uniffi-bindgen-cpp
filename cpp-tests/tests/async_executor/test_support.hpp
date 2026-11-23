#pragma once

#include "async_calls.hpp"
#include "futures.hpp"
#include "async_test_support.hpp"

namespace executor_tests {

uniffi::detail::AsyncExecutor &executor_from_peer();
void test_rust_future_lifecycle();
void test_shutdown_during_handle_creation();

void test_foreign_callback_lifecycle();
void test_foreign_callback_after_shutdown();
void test_foreign_states_across_restart();

} // namespace executor_tests
