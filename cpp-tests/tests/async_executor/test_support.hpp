#pragma once

#include "futures.hpp"
#include "test_common.hpp"

namespace executor_tests {

inline constexpr std::chrono::seconds test_timeout{10};

template <typename Future>
decltype(auto) wait_for_future(Future &&future, std::chrono::milliseconds timeout = test_timeout) {
    ASSERT_EQ(future.wait_for(timeout), std::future_status::ready);
    return future.get();
}

uniffi::detail::AsyncExecutor &executor_from_peer();
void test_rust_future_lifecycle();
void test_shutdown_during_handle_creation();
void test_foreign_callback_after_shutdown();
void test_foreign_states_across_restart();

} // namespace executor_tests
