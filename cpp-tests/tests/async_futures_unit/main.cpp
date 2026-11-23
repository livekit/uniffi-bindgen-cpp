#include "async_runtime_test.hpp"

using namespace std::chrono_literals;

namespace {

using executor_tests::wait_for_future;
using uniffi::AsyncCancelledError;
using uniffi::Future;
using uniffi::FutureResult;
using uniffi::detail::FutureCompletionState;
using uniffi::detail::async_executor;

// Waits retain a result until get() consumes it, including void and error results.
void test_result_and_waits() {
    Future<int> empty;
    ASSERT_FALSE(empty.valid());
    EXPECT_EXCEPTION(empty.get(), std::future_error);

    auto value = std::make_shared<FutureCompletionState<int>>();
    Future<int> future(value, nullptr);
    ASSERT_EQ(future.wait_for(1ms), std::future_status::timeout);
    value->complete(FutureResult<int>(42));
    future.wait();
    ASSERT_EQ(wait_for_future(future), 42);
    ASSERT_FALSE(future.valid());

    auto unit = std::make_shared<FutureCompletionState<void>>();
    unit->complete(FutureResult<void>());
    wait_for_future(Future<void>(unit, nullptr));

    auto error = std::make_shared<FutureCompletionState<int>>();
    error->complete(FutureResult<int>(std::make_exception_ptr(AsyncCancelledError())));
    EXPECT_EXCEPTION(wait_for_future(Future<int>(error, nullptr)), AsyncCancelledError);
}

// Both attachment orders deliver the same result on the executor.
void test_continuation_delivery(bool complete_first) {
    auto state = std::make_shared<FutureCompletionState<int>>();
    Future<int> future(state, nullptr);
    if (complete_first) {
        state->complete(FutureResult<int>(42));
    }

    auto delivered = std::make_shared<std::promise<int>>();
    auto result = delivered->get_future();
    auto token = std::move(future).then([delivered](auto completed) {
        ASSERT_TRUE(async_executor().is_worker());
        delivered->set_value(std::move(completed).get());
    });
    ASSERT_FALSE(future.valid());
    if (!complete_first) {
        ASSERT_EQ(result.wait_for(0ms), std::future_status::timeout);
        state->complete(FutureResult<int>(42));
    }
    ASSERT_EQ(wait_for_future(result), 42);
}

// An unresolved future cannot block the executor or lose ownership on a rejected wait.
void test_worker_wait_rejected() {
    std::promise<void> checked;
    auto result = checked.get_future();
    ASSERT_TRUE(async_executor().enqueue([&] {
        auto state = std::make_shared<FutureCompletionState<int>>();
        Future<int> future(state, nullptr);
        EXPECT_EXCEPTION(future.get(), std::logic_error);
        EXPECT_EXCEPTION(future.wait(), std::logic_error);
        EXPECT_EXCEPTION(future.wait_for(1ms), std::logic_error);
        ASSERT_EQ(future.wait_for(0ms), std::future_status::timeout);
        state->complete(FutureResult<int>(42));
        ASSERT_EQ(future.get(), 42);
        checked.set_value();
    }));
    wait_for_future(result);
}

// Move assignment cancels the old owner and leaves the new result available.
void test_move_ownership() {
    unsigned cancellations = 0;
    auto old = std::make_shared<FutureCompletionState<void>>();
    Future<void> owned(old, [&] { ++cancellations; });
    auto replacement = std::make_shared<FutureCompletionState<void>>();
    replacement->complete(FutureResult<void>());
    Future<void> ready(replacement, nullptr);
    owned = std::move(ready);
    ASSERT_FALSE(ready.valid());
    ASSERT_EQ(cancellations, 1u);
    wait_for_future(owned);
    ASSERT_EQ(cancellations, 1u);

    uniffi::FutureContinuation old_token([&] { ++cancellations; });
    uniffi::FutureContinuation new_token([&] { ++cancellations; });
    old_token = std::move(new_token);
    ASSERT_EQ(cancellations, 2u);
    new_token.cancel();
    ASSERT_EQ(cancellations, 2u);
    old_token.cancel();
    ASSERT_EQ(cancellations, 3u);
}

// A detached token leaves the result pending until a copied handle cancels from another thread.
void test_detach_and_cancel_handle() {
    auto state = std::make_shared<FutureCompletionState<void>>();
    Future<void> future(state, [state] {
        state->complete(FutureResult<void>(std::make_exception_ptr(AsyncCancelledError())));
    });
    auto handle = future.cancel_handle();
    auto delivered = std::make_shared<std::promise<void>>();
    auto result = delivered->get_future();
    std::move(future).then([delivered](auto completed) {
        EXPECT_EXCEPTION(std::move(completed).get(), AsyncCancelledError);
        delivered->set_value();
    }).detach();
    ASSERT_EQ(state->wait_for(0ms), std::future_status::timeout);

    auto canceller = std::async(std::launch::async, [handle] { handle.cancel(); });
    wait_for_future(canceller);
    wait_for_future(result);
}

} // namespace

int main() {
    test_result_and_waits();
    test_continuation_delivery(true);
    test_continuation_delivery(false);
    test_worker_wait_rejected();
    test_move_ownership();
    test_detach_and_cancel_handle();
    uniffi::shutdown_async_executor();
}
