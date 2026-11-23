#pragma once

#include "test_common.hpp"

#include <chrono>
#include <future>
#include <thread>

namespace executor_tests {

inline constexpr std::chrono::seconds test_timeout{10};

template <typename Future>
decltype(auto) wait_for_future(Future &&future, std::chrono::milliseconds timeout = test_timeout) {
    ASSERT_EQ(future.wait_for(timeout), std::future_status::ready);
    return future.get();
}

// Called while an inline poll is gated. Detect that shutdown is deferring new first polls.
template <typename Executor> void wait_for_deferred_first_poll(Executor &executor) {
    const auto deadline = std::chrono::steady_clock::now() + test_timeout;
    for (;;) {
        bool poll_on_caller = false;
        const auto id = executor.track([] {}, &poll_on_caller);
        ASSERT_NE(id, 0u);
        if (poll_on_caller) {
            executor.run_inline([] {});
        }
        executor.untrack(id);
        if (!poll_on_caller) {
            return;
        }
        ASSERT_TRUE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

} // namespace executor_tests
