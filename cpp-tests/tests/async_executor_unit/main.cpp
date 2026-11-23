#include "async_runtime_test.hpp"

using namespace std::chrono_literals;

namespace executor_tests {

uniffi::detail::AsyncExecutor &executor_from_peer();

namespace {

using uniffi::detail::AsyncExecutor;
using uniffi::detail::async_executor;

// A standalone executor runs tasks in FIFO order and continues after a task throws.
void test_executor_queue() {
    AsyncExecutor executor;
    const auto caller = std::this_thread::get_id();
    std::vector<int> visited;

    ASSERT_TRUE(executor.accepting());
    ASSERT_FALSE(executor.is_worker());

    ASSERT_TRUE(executor.enqueue([&] {
        ASSERT_TRUE(executor.is_worker());
        ASSERT_NE(std::this_thread::get_id(), caller);
        visited.push_back(1);
        EXPECT_EXCEPTION(executor.shutdown(), std::logic_error);
    }));
    ASSERT_TRUE(executor.enqueue([&] {
        ASSERT_TRUE(executor.is_worker());
        visited.push_back(2);
        throw std::runtime_error("test task failure");
    }));
    ASSERT_TRUE(executor.enqueue([&] {
        ASSERT_TRUE(executor.is_worker());
        visited.push_back(3);
    }));

    executor.shutdown();
    ASSERT_EQ(visited, (std::vector<int>{1, 2, 3}));
    ASSERT_FALSE(executor.accepting());
    ASSERT_FALSE(executor.enqueue([] {}));
    ASSERT_EQ(executor.track([] {}), 0u);
    executor.shutdown();

    // A new thread can reuse the ID of the joined worker.
    std::thread caller_after_join([&] {
        ASSERT_FALSE(executor.is_worker());
        executor.shutdown();
    });
    caller_after_join.join();
}

// Shutdown requests cancellation and waits for queued cleanup of tracked work.
void test_executor_shutdown_drains_active_work() {
    AsyncExecutor executor;
    auto cancelling = std::make_shared<std::promise<void>>();
    auto cancellation_started = cancelling->get_future();
    const auto finished = executor.track([] { assert(false && "completed operation was cancelled"); });
    ASSERT_NE(finished, 0u);
    executor.untrack(finished);
    const auto active = executor.track([&executor, cancelling] {
        ASSERT_FALSE(executor.accepting());
        cancelling->set_value();
    });
    ASSERT_NE(active, 0u);
    ASSERT_NE(active, finished);

    auto stopping = std::async(std::launch::async, [&] { executor.shutdown(); });
    wait_for_future(cancellation_started);
    ASSERT_EQ(executor.track([] {}), 0u);
    ASSERT_EQ(stopping.wait_for(0ms), std::future_status::timeout);

    bool cleaned_up = false;
    ASSERT_TRUE(executor.enqueue([&] {
        ASSERT_TRUE(executor.is_worker());
        executor.untrack(active);
        cleaned_up = true;
    }));
    wait_for_future(stopping);
    ASSERT_TRUE(cleaned_up);
    ASSERT_FALSE(executor.enqueue([] {}));
}

// A standalone executor restarts with a worker and continues assigning new operation IDs.
void test_executor_restart() {
    AsyncExecutor executor;
    executor.start();
    const auto previous = executor.track([] {});
    ASSERT_NE(previous, 0u);
    executor.untrack(previous);
    executor.shutdown();
    ASSERT_FALSE(executor.enqueue([] {}));

    executor.start();
    executor.start();
    ASSERT_TRUE(executor.accepting());
    const auto active = executor.track([] {});
    ASSERT_TRUE(active > previous);

    bool ran = false;
    ASSERT_TRUE(executor.enqueue([&] {
        ASSERT_TRUE(executor.is_worker());
        executor.start();
        ran = true;
        executor.untrack(active);
    }));
    executor.shutdown();
    ASSERT_TRUE(ran);
    ASSERT_FALSE(executor.enqueue([] {}));
}

// The shared executor is lazy and its accessor resolves to one worker across translation units.
void test_shared_executor() {
    auto &executor = async_executor();
    ASSERT_EQ(&executor, &executor_from_peer());

    std::promise<void> ran;
    auto task = ran.get_future();
    ASSERT_TRUE(executor_from_peer().enqueue([&] {
        ASSERT_TRUE(executor.is_worker());
        ran.set_value();
    }));
    wait_for_future(task);
    uniffi::shutdown_async_executor();
    ASSERT_FALSE(executor.accepting());
    uniffi::start_async_executor();
    ASSERT_TRUE(executor.accepting());
    uniffi::shutdown_async_executor();
}

// Closing waits for caller work and keeps the queue available for cleanup and new first polls.
void test_shutdown_waits_for_inline_work() {
    AsyncExecutor executor;
    std::promise<void> entered;
    auto inline_entered = entered.get_future();
    std::promise<void> release;
    auto inline_released = release.get_future();

    auto caller = std::async(std::launch::async, [&] {
        bool poll_on_caller = false;
        const auto id = executor.track([] {}, &poll_on_caller);
        ASSERT_NE(id, 0u);
        ASSERT_TRUE(poll_on_caller);
        executor.run_inline([&] {
            EXPECT_EXCEPTION(executor.shutdown(), std::logic_error);
            executor.start();
            entered.set_value();
            wait_for_future(inline_released);
            executor.untrack(id);
        });
    });
    wait_for_future(inline_entered);

    auto stopping = std::async(std::launch::async, [&] { executor.shutdown(); });
    wait_for_deferred_first_poll(executor);
    ASSERT_TRUE(executor.accepting());
    ASSERT_EQ(stopping.wait_for(0ms), std::future_status::timeout);

    std::promise<void> ran;
    auto queued = ran.get_future();
    ASSERT_TRUE(executor.enqueue([&] {
        ASSERT_TRUE(executor.is_worker());
        ran.set_value();
    }));
    wait_for_future(queued);

    release.set_value();
    wait_for_future(caller);
    wait_for_future(stopping);
    ASSERT_FALSE(executor.accepting());
}

} // namespace
} // namespace executor_tests

int main() {
    using namespace executor_tests;
    test_executor_queue();
    test_executor_shutdown_drains_active_work();
    test_executor_restart();
    test_shared_executor();
    test_shutdown_waits_for_inline_work();
}
