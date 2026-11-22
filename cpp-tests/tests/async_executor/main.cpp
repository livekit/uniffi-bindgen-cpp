#include "test_support.hpp"

#include <string>

namespace executor_tests {
namespace {

using namespace std::chrono_literals;
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

void wait_for_queued_work(AsyncExecutor &executor) {
    auto reached = std::make_shared<std::promise<void>>();
    auto barrier = reached->get_future();
    ASSERT_TRUE(executor.enqueue([reached] { reached->set_value(); }));
    wait_for_future(barrier);
}

// Generated components share the singleton and deliver ready continuations on its worker.
void test_singleton_across_components() {
    auto &executor = async_executor();
    ASSERT_EQ(&executor, &executor_from_peer());

    auto worker_id = std::make_shared<std::promise<std::thread::id>>();
    auto observed_worker = worker_id->get_future();
    ASSERT_TRUE(executor_from_peer().enqueue([worker_id] {
        ASSERT_TRUE(async_executor().is_worker());
        worker_id->set_value(std::this_thread::get_id());
    }));
    const auto worker = wait_for_future(observed_worker);
    ASSERT_NE(worker, std::this_thread::get_id());

    // Exercise then() both after a result is ready and while a result is pending (below).
    auto future = futures::always_ready();
    ASSERT_EQ(future.wait_for(test_timeout), std::future_status::ready);
    auto delivered = std::make_shared<std::promise<void>>();
    auto result = delivered->get_future();
    auto continuation = std::move(future).then([worker, delivered](auto completed) {
        ASSERT_EQ(std::this_thread::get_id(), worker);
        ASSERT_TRUE(std::move(completed).get());
        delivered->set_value();
    });
    wait_for_future(result);
}

// Two Rust streams receive independently, with bounded buffering and clean end-of-stream.
void test_interleaved_stream_reads() {
    const auto first = futures::SampleStream::init();
    const auto second = futures::SampleStream::init();
    auto first_read = first->next();
    auto second_read = second->next();
    wait_for_queued_work(async_executor());
    ASSERT_TRUE(first->is_read_pending());
    ASSERT_TRUE(second->is_read_pending());

    // Push from the caller thread while both readers are parked on the executor.
    ASSERT_TRUE(second->push(20));
    ASSERT_EQ(wait_for_future(second_read), std::optional<std::uint32_t>(20));
    ASSERT_EQ(first_read.wait_for(0ms), std::future_status::timeout);

    auto delivered = std::make_shared<std::promise<std::optional<std::uint32_t>>>();
    auto result = delivered->get_future();
    auto continuation = std::move(first_read).then([delivered](auto completed) {
        ASSERT_TRUE(async_executor().is_worker());
        delivered->set_value(std::move(completed).get());
    });
    ASSERT_TRUE(first->push(10));
    ASSERT_EQ(wait_for_future(result), std::optional<std::uint32_t>(10));

    auto end_of_stream = first->next();
    auto surviving_read = second->next();
    wait_for_queued_work(async_executor());
    ASSERT_TRUE(first->is_read_pending());
    ASSERT_TRUE(second->is_read_pending());
    first->close_stream();
    first->close_stream();
    ASSERT_EQ(wait_for_future(end_of_stream), std::nullopt);
    ASSERT_EQ(wait_for_future(first->next()), std::nullopt);
    ASSERT_FALSE(first->push(11));
    ASSERT_EQ(surviving_read.wait_for(0ms), std::future_status::timeout);

    ASSERT_TRUE(second->push(21));
    ASSERT_EQ(wait_for_future(surviving_read), std::optional<std::uint32_t>(21));
    ASSERT_TRUE(second->push(22));
    ASSERT_TRUE(second->push(23));
    ASSERT_FALSE(second->push(24));
    ASSERT_EQ(wait_for_future(second->next()), std::optional<std::uint32_t>(22));
    ASSERT_EQ(wait_for_future(second->next()), std::optional<std::uint32_t>(23));
}

// Cancelling a pending read releases the receiver for the next read.
void test_cancel_pending_read() {
    const auto stream = futures::SampleStream::init();
    auto read = stream->next();
    wait_for_queued_work(async_executor());
    ASSERT_TRUE(stream->is_read_pending());
    read.cancel();
    EXPECT_EXCEPTION(wait_for_future(read), uniffi::AsyncCancelledError);
    ASSERT_FALSE(stream->is_read_pending());

    // Cancelling an individual read releases its lock and leaves the stream usable.
    ASSERT_TRUE(stream->push(42));
    ASSERT_EQ(wait_for_future(stream->next()), std::optional<std::uint32_t>(42));
}

// A separate cancellation handle releases a thread waiting in get().
void test_cancel_blocking_get() {
    auto pending = futures::pending();
    const auto cancel = pending.cancel_handle();
    auto entering = std::make_shared<std::promise<void>>();
    auto entering_get = entering->get_future();
    auto waiter = std::async(std::launch::async, [pending = std::move(pending), entering]() mutable {
        entering->set_value();
        EXPECT_EXCEPTION(pending.get(), uniffi::AsyncCancelledError);
    });

    wait_for_future(entering_get);
    ASSERT_EQ(waiter.wait_for(20ms), std::future_status::timeout);
    const auto copy = cancel;
    copy.cancel();
    wait_for_future(waiter);
    cancel.cancel();
}

// Handles survive completion, destruction of a future, and transfer into then().
void test_cancel_handle_lifetime() {
    uniffi::FutureCancelHandle finished;
    {
        auto ready = futures::always_ready();
        finished = ready.cancel_handle();
        ASSERT_TRUE(wait_for_future(ready));
        finished.cancel();
    }
    finished.cancel();

    auto pending = futures::pending();
    const auto cancel = pending.cancel_handle();
    auto delivered = std::make_shared<std::promise<void>>();
    auto result = delivered->get_future();
    auto continuation = std::move(pending).then([delivered](auto completed) {
        EXPECT_EXCEPTION(completed.get(), uniffi::AsyncCancelledError);
        delivered->set_value();
    });
    wait_for_queued_work(async_executor());
    cancel.cancel();
    wait_for_future(result);
}

// Shutdown delivers cancellation, rejects new reads, and allows the Rust receiver to drop.
void test_singleton_shutdown() {
    ASSERT_EQ(futures::sample_stream_count(), 0u);
    auto stream = futures::SampleStream::init();
    auto delivered = std::make_shared<std::promise<void>>();
    auto result = delivered->get_future();
    auto continuation = stream->next().then([delivered](auto completed) {
        ASSERT_TRUE(async_executor().is_worker());
        EXPECT_EXCEPTION(std::move(completed).get(), uniffi::AsyncCancelledError);
        delivered->set_value();
    });
    wait_for_queued_work(async_executor());
    ASSERT_TRUE(stream->is_read_pending());

    test_shutdown_during_handle_creation();
    wait_for_future(result, 0ms);
    ASSERT_FALSE(stream->is_read_pending());
    ASSERT_FALSE(executor_from_peer().accepting());
    ASSERT_FALSE(executor_from_peer().enqueue([] {}));
    EXPECT_EXCEPTION(wait_for_future(stream->next()), uniffi::AsyncExecutorError);
    stream.reset();
    ASSERT_EQ(futures::sample_stream_count(), 0u);
    uniffi::shutdown_async_executor();
}

class RestartParser final : public futures::AsyncParserForeign {
  public:
    uniffi::ForeignFuture<std::string> stringify(std::int32_t value) override {
        return uniffi::ForeignFuture<std::string>([value](auto success, auto) {
            success(std::to_string(value));
            return nullptr;
        });
    }
    uniffi::ForeignFuture<std::int32_t> parse(const std::string &) override { return {}; }
    uniffi::ForeignFuture<void> wait() override { return {}; }
};

// After explicit restart, the singleton can poll Rust and deliver an async C++ callback.
void test_singleton_restart() {
    uniffi::start_async_executor();
    uniffi::start_async_executor();
    ASSERT_TRUE(wait_for_future(futures::always_ready()));
    ASSERT_EQ(wait_for_future(futures::stringify_using_parser(std::make_shared<RestartParser>(), 42)),
              "42");
    uniffi::shutdown_async_executor();
    EXPECT_EXCEPTION(wait_for_future(futures::always_ready()), uniffi::AsyncExecutorError);
}

} // namespace
} // namespace executor_tests

int main() {
    using namespace executor_tests;
    test_executor_queue();
    test_executor_shutdown_drains_active_work();
    test_executor_restart();
    test_singleton_across_components();
    test_rust_future_lifecycle();
    test_interleaved_stream_reads();
    test_cancel_pending_read();
    test_cancel_blocking_get();
    test_cancel_handle_lifetime();
    // Check shutdown rejection before explicitly restarting the singleton.
    test_singleton_shutdown();
    test_foreign_callback_after_shutdown();
    test_foreign_states_across_restart();
    test_singleton_restart();
}
