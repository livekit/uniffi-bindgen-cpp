#include "test_support.hpp"

#include <string>

namespace executor_tests {
namespace {

using namespace std::chrono_literals;
using uniffi::detail::AsyncExecutor;
using uniffi::detail::async_executor;

void wait_for_queued_work(AsyncExecutor &executor) {
    auto reached = std::make_shared<std::promise<void>>();
    auto barrier = reached->get_future();
    ASSERT_TRUE(executor.enqueue([reached] { reached->set_value(); }));
    wait_for_future(barrier);
}

// Two Rust streams receive independently, with bounded buffering and clean end-of-stream.
void test_interleaved_stream_reads() {
    const auto first = wait_for_future(async_calls::SampleStream::open());
    const auto second = async_calls::SampleStream::init();
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
    const auto stream = async_calls::SampleStream::init();
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

// Generated record and enum wrappers lift a value and a declared error.
void test_value_receivers() {
    auto record_value = async_calls::MyRecord{"value", 17}.value_after(1);
    ASSERT_EQ(17, wait_for_future(record_value));

    wait_for_future(async_calls::validate(async_calls::ValidationMode::kAllow));
    EXPECT_EXCEPTION(wait_for_future(async_calls::validate(async_calls::ValidationMode::kReject)),
                     async_calls::my_error::Foo);
}

// A separate cancellation handle releases a thread waiting in get().
void test_cancel_blocking_get() {
    auto pending = async_calls::pending();
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
        auto ready = async_calls::SampleStream::open();
        finished = ready.cancel_handle();
        ASSERT_NE(wait_for_future(ready), nullptr);
        finished.cancel();
    }
    finished.cancel();

    auto pending = async_calls::pending();
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

// A detached continuation delivers a stream read after its token is destroyed.
void test_detached_continuation() {
    auto stream = async_calls::SampleStream::init();
    auto delivered = std::make_shared<std::promise<std::uint32_t>>();
    auto result = delivered->get_future();
    stream->next().then([delivered](auto completed) {
        delivered->set_value(std::move(completed).get().value());
    }).detach();
    ASSERT_TRUE(stream->push(42));
    ASSERT_EQ(wait_for_future(result), 42u);
}

// Move assignment cancels the previous owner and transfers the new result or continuation.
void test_future_move_assignment() {
    uniffi::Future<void> future;
    ASSERT_FALSE(future.valid());
    EXPECT_EXCEPTION(future.get(), std::future_error);

    future = async_calls::pending();
    ASSERT_EQ(async_calls::pending_count(), 1u);
    future = async_calls::validate(async_calls::ValidationMode::kAllow);
    wait_for_future(future);

    auto delivered = std::make_shared<std::promise<void>>();
    auto cancellation = delivered->get_future();
    auto continuation = async_calls::pending().then([delivered](auto result) {
        EXPECT_EXCEPTION(std::move(result).get(), uniffi::AsyncCancelledError);
        delivered->set_value();
    });
    const auto completed = std::make_shared<std::promise<void>>();
    auto completion = completed->get_future();
    continuation = async_calls::validate(async_calls::ValidationMode::kAllow).then([completed](auto result) {
        std::move(result).get();
        completed->set_value();
    });
    wait_for_future(cancellation);
    wait_for_future(completion);
    ASSERT_EQ(async_calls::pending_count(), 0u);
}

// Generated pending futures support wait, explicit cancellation, and abandonment.
void test_pending_future() {
    {
        auto pending = async_calls::pending();
        ASSERT_EQ(pending.wait_for(1ms), std::future_status::timeout);
        ASSERT_EQ(async_calls::pending_count(), 1u);
        pending.cancel();
        EXPECT_EXCEPTION(wait_for_future(pending), uniffi::AsyncCancelledError);
        ASSERT_EQ(async_calls::pending_count(), 0u);
    }
    {
        auto pending = async_calls::pending();
        ASSERT_EQ(async_calls::pending_count(), 1u);
    }
    wait_for_queued_work(async_executor());
    ASSERT_EQ(async_calls::pending_count(), 0u);
}

// Shutdown delivers cancellation, rejects new reads, and allows the Rust receiver to drop.
void test_singleton_shutdown() {
    ASSERT_EQ(async_calls::sample_stream_count(), 0u);
    auto stream = async_calls::SampleStream::init();
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
    ASSERT_FALSE(async_executor().accepting());
    ASSERT_FALSE(async_executor().enqueue([] {}));
    EXPECT_EXCEPTION(wait_for_future(stream->next()), uniffi::AsyncExecutorError);
    stream.reset();
    ASSERT_EQ(async_calls::sample_stream_count(), 0u);
    uniffi::shutdown_async_executor();
    uniffi::start_async_executor();
    ASSERT_NE(wait_for_future(async_calls::SampleStream::open()), nullptr);
    uniffi::shutdown_async_executor();
}

} // namespace
} // namespace executor_tests

int main() {
    using namespace executor_tests;
    test_rust_future_lifecycle();
    test_pending_future();
    test_interleaved_stream_reads();
    test_cancel_pending_read();
    test_value_receivers();
    test_cancel_blocking_get();
    test_cancel_handle_lifetime();
    test_detached_continuation();
    test_future_move_assignment();
    test_singleton_shutdown();
}
