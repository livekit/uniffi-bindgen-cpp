#include "test_support.hpp"

// Include the generated implementation once to exercise its private rust_call_async() driver.
// Scripted Rust FFI functions record the call order so we can verify repolling on WAKE, completion
// on READY, and exactly one free on success, cancellation, Rust errors, or C++ conversion failures.
// The stream tests in main.cpp exercise the same binding through its public API.
#include "async_calls.cpp"

namespace executor_tests {
namespace {

// https://mozilla.github.io/uniffi-rs/latest/internals/async-ffi.html#managing-the-rustfuture-handle
constexpr std::int8_t ready = 0;
constexpr std::int8_t wake = 1;
constexpr std::int8_t unexpected_error = 2;
constexpr std::int8_t cancelled = 3;

enum class Step { Start, Poll, Cancel, Complete, Free };
enum class PollReply { Park, WakeInline, Ready, Invalid };
enum class Completion { Value, ErrorStatus, LiftError };

struct PollCallback {
    RustFutureContinuationCallback callback;
    std::uint64_t data;

    void notify(std::int8_t result) const { callback(data, result); }
};

class PausedTask {
  public:
    void pause() {
        paused_.set_value();
        wait_for_future(resume_signal_);
    }

    void wait_until_paused() { wait_for_future(paused_signal_); }
    void resume() { resumed_.set_value(); }

  private:
    std::promise<void> paused_;
    std::future<void> paused_signal_ = paused_.get_future();
    std::promise<void> resumed_;
    std::future<void> resume_signal_ = resumed_.get_future();
};

class RustFutureProbe {
  public:
    explicit RustFutureProbe(std::vector<PollReply> replies, PausedTask *pause = nullptr)
        : replies_(std::move(replies)), pause_(pause) {}

    RustFutureProbe(std::initializer_list<PollReply> replies)
        : RustFutureProbe(std::vector<PollReply>(replies)) {}

    uniffi::Future<int> start(Completion completion = Completion::Value) {
        return start_with_lift([](int value) { return value + 1; }, completion);
    }

    template <typename Lift>
    uniffi::Future<std::invoke_result_t<Lift, int>>
    start_with_lift(Lift lift, Completion completion = Completion::Value) {
        return async_calls::uniffi::rust_call_async<std::invoke_result_t<Lift, int>>(
            [this] {
                caller_ = std::this_thread::get_id();
                record(Step::Start);
                return reinterpret_cast<std::uint64_t>(this);
            },
            [](std::uint64_t handle, RustFutureContinuationCallback callback, std::uint64_t data) {
                from_handle(handle).poll(handle, {callback, data});
            },
            [](std::uint64_t handle) {
                from_handle(handle).cancel(handle);
            },
            [this, completion](auto handle, auto *status) {
                record_ffi(handle, Step::Complete, replies_.front() == PollReply::Ready);
                ASSERT_NE(status, nullptr);
                if (was_cancelled_) {
                    status->code = cancelled;
                } else if (completion == Completion::ErrorStatus) {
                    status->code = unexpected_error;
                    status->error_buf = async_calls::uniffi::FfiConverterString::lower("Rust completion failed");
                }
                return 41;
            },
            [](std::uint64_t handle) {
                auto &probe = from_handle(handle);
                probe.record_ffi(handle, Step::Free, probe.replies_.front() == PollReply::Ready);
                probe.handle_freed = true;
            },
            [this, completion, lift = std::move(lift)](int value) mutable {
                ASSERT_FALSE(was_cancelled_);
                ASSERT_NE(completion, Completion::ErrorStatus);
                if (completion == Completion::LiftError) {
                    throw std::runtime_error("C++ lifting failed");
                }
                return lift(value);
            },
            nullptr);
    }

    PollCallback wait_for_pending_poll() { return wait_for_future(parked_.get_future()); }

    void wake_from_caller(PollCallback callback) {
        ASSERT_FALSE(uniffi::detail::async_executor().is_worker());
        wake_sent_ = true;
        callback.notify(wake);
    }

    void expect_steps(std::initializer_list<Step> expected) {
        const std::lock_guard<std::mutex> lock(mutex_);
        ASSERT_EQ(steps_, std::vector<Step>(expected));
    }

    std::atomic<bool> handle_freed{false};

  private:
    static RustFutureProbe &from_handle(std::uint64_t handle) {
        return *reinterpret_cast<RustFutureProbe *>(handle);
    }

    void cancel(std::uint64_t handle) {
        record_ffi(handle, Step::Cancel, false);
        was_cancelled_ = true;
        if (parked_callback_) {
            const auto callback = *parked_callback_;
            parked_callback_.reset();
            callback.notify(ready);
        }
    }

    void record(Step step) {
        const std::lock_guard<std::mutex> lock(mutex_);
        steps_.push_back(step);
    }

    void record_ffi(std::uint64_t handle, Step step, bool on_caller) {
        ASSERT_EQ(handle, reinterpret_cast<std::uint64_t>(this));
        if (on_caller) {
            ASSERT_EQ(std::this_thread::get_id(), caller_);
        } else {
            ASSERT_TRUE(uniffi::detail::async_executor().is_worker());
        }
        // Even an inline Rust notification must return before the next FFI operation.
        ASSERT_FALSE(in_poll_);
        record(step);
    }

    void poll(std::uint64_t handle, PollCallback callback) {
        record_ffi(handle, Step::Poll, next_reply_ == 0);
        ASSERT_NE(callback.callback, nullptr);
        ASSERT_NE(callback.data, 0u);
        ASSERT_TRUE(next_reply_ < replies_.size());
        if (parked_callback_) {
            ASSERT_TRUE(wake_sent_.load());
            parked_callback_.reset();
        }

        in_poll_ = true;
        if (next_reply_ == 0) {
            EXPECT_EXCEPTION(uniffi::shutdown_async_executor(), std::logic_error);
        }
        switch (replies_[next_reply_++]) {
        case PollReply::Park:
            parked_callback_ = callback;
            parked_.set_value(callback);
            break;
        case PollReply::WakeInline:
            callback.notify(wake);
            break;
        case PollReply::Ready:
            callback.notify(ready);
            break;
        case PollReply::Invalid:
            callback.notify(-1);
            break;
        }
        if (next_reply_ == 1 && pause_) {
            pause_->pause();
        }
        in_poll_ = false;
    }

    const std::vector<PollReply> replies_;
    PausedTask *const pause_;
    std::thread::id caller_;
    std::size_t next_reply_ = 0;
    std::mutex mutex_;
    std::vector<Step> steps_;
    std::optional<PollCallback> parked_callback_;
    std::promise<PollCallback> parked_;
    std::atomic<bool> wake_sent_{false};
    bool in_poll_ = false;
    bool was_cancelled_ = false;
};

// A ready first poll completes on the caller even while the worker is paused.
void test_first_poll_runs_on_caller() {
    PausedTask worker;
    ASSERT_TRUE(uniffi::detail::async_executor().enqueue([&] { worker.pause(); }));
    worker.wait_until_paused();

    RustFutureProbe immediate{PollReply::Ready};
    auto ready_result = immediate.start();
    ASSERT_EQ(wait_for_future(ready_result, std::chrono::seconds(0)), 42);
    immediate.expect_steps({Step::Start, Step::Poll, Step::Complete, Step::Free});

    RustFutureProbe deferred{PollReply::WakeInline, PollReply::Ready};
    auto pending_result = deferred.start();
    ASSERT_EQ(pending_result.wait_for(std::chrono::seconds(0)), std::future_status::timeout);
    deferred.expect_steps({Step::Start, Step::Poll});

    worker.resume();
    ASSERT_EQ(wait_for_future(pending_result), 42);
    deferred.expect_steps({Step::Start, Step::Poll, Step::Poll, Step::Complete, Step::Free});
}

void test_pending_to_ready() {
    RustFutureProbe probe{PollReply::Park, PollReply::WakeInline, PollReply::Ready};
    auto future = probe.start();
    const auto callback = probe.wait_for_pending_poll();
    probe.expect_steps({Step::Start, Step::Poll});
    ASSERT_EQ(future.wait_for(std::chrono::seconds(0)), std::future_status::timeout);

    probe.wake_from_caller(callback);
    ASSERT_EQ(wait_for_future(future), 42);
    probe.expect_steps({Step::Start, Step::Poll, Step::Poll, Step::Poll, Step::Complete, Step::Free});
}

// A future that wakes itself repeatedly yields the worker to other queued tasks.
void test_rapid_wakes_yield() {
    auto &executor = uniffi::detail::async_executor();
    PausedTask worker;
    ASSERT_TRUE(executor.enqueue([&] { worker.pause(); }));
    worker.wait_until_paused();

    std::vector<PollReply> replies(128, PollReply::WakeInline);
    replies.push_back(PollReply::Ready);
    RustFutureProbe probe(std::move(replies));
    auto future = probe.start();

    std::promise<void> yielded;
    auto reached = yielded.get_future();
    ASSERT_TRUE(executor.enqueue([&] {
        ASSERT_FALSE(probe.handle_freed.load());
        yielded.set_value();
    }));
    worker.resume();
    wait_for_future(reached);
    ASSERT_EQ(wait_for_future(future), 42);
}

// A bad poll code becomes a C++ error after the Rust handle is freed.
void test_invalid_poll_code() {
    RustFutureProbe probe{PollReply::Invalid, PollReply::Ready};
    try {
        wait_for_future(probe.start());
        assert(false && "expected an invalid poll result to throw");
    } catch (const std::runtime_error &error) {
        ASSERT_EQ(std::string(error.what()), "Unexpected UniFFI Rust future poll result");
    }
    probe.expect_steps({Step::Start, Step::Poll, Step::Cancel, Step::Poll, Step::Complete, Step::Free});
}

void test_cleanup_on_failure(Completion completion, const char *message) {
    RustFutureProbe probe{PollReply::Ready};
    try {
        wait_for_future(probe.start(completion));
        assert(false && "expected future completion to throw");
    } catch (const std::runtime_error &error) {
        ASSERT_EQ(std::string(error.what()), message);
    }
    probe.expect_steps({Step::Start, Step::Poll, Step::Complete, Step::Free});
}

void test_cancellation_lifecycle() {
    RustFutureProbe probe{PollReply::Park};
    auto future = probe.start();
    probe.wait_for_pending_poll();
    future.cancel();
    future.cancel();
    EXPECT_EXCEPTION(wait_for_future(future), uniffi::AsyncCancelledError);
    probe.expect_steps({Step::Start, Step::Poll, Step::Cancel, Step::Complete, Step::Free});
}

class ValueMoveError : public std::exception {};

class ValueWithThrowingMove {
  public:
    explicit ValueWithThrowingMove(const std::atomic<bool> &fail) : fail_(fail) {}
    ValueWithThrowingMove(ValueWithThrowingMove &&other) : fail_(other.fail_) {
        if (fail_.load()) {
            throw ValueMoveError();
        }
    }

  private:
    const std::atomic<bool> &fail_;
};

void test_result_publication_move_failure() {
    RustFutureProbe probe{PollReply::Ready};
    auto result = probe.start_with_lift([&](int) { return ValueWithThrowingMove(probe.handle_freed); });
    EXPECT_EXCEPTION(wait_for_future(result), ValueMoveError);
    probe.expect_steps({Step::Start, Step::Poll, Step::Complete, Step::Free});
}

void test_continuation_move_failure() {
    std::atomic<bool> fail_move{false};
    RustFutureProbe probe{PollReply::Ready};
    auto result = probe.start_with_lift([&](int) { return ValueWithThrowingMove(fail_move); });
    ASSERT_EQ(result.wait_for(test_timeout), std::future_status::ready);
    fail_move = true;

    auto delivered = std::make_shared<std::promise<void>>();
    auto delivery = delivered->get_future();
    auto continuation = std::move(result).then([delivered](auto completed) {
        ASSERT_TRUE(uniffi::detail::async_executor().is_worker());
        EXPECT_EXCEPTION(std::move(completed).get(), ValueMoveError);
        delivered->set_value();
    });
    wait_for_future(delivery);
    probe.expect_steps({Step::Start, Step::Poll, Step::Complete, Step::Free});
}

} // namespace

void test_rust_future_lifecycle() {
    test_first_poll_runs_on_caller();
    test_pending_to_ready();
    test_rapid_wakes_yield();
    test_invalid_poll_code();
    test_cleanup_on_failure(Completion::ErrorStatus, "Rust completion failed");
    test_cleanup_on_failure(Completion::LiftError, "C++ lifting failed");
    test_cancellation_lifecycle();
    test_result_publication_move_failure();
    test_continuation_move_failure();
}

// Close admission after the start check while the caller still owns the new Rust handle.
void test_shutdown_during_handle_creation() {
    unsigned frees = 0;
    const auto unexpected_ffi_call = [](auto...) {
        assert(false && "the rejected Rust future entered the driver");
    };
    auto result = async_calls::uniffi::rust_call_async<int>(
        [&] {
            uniffi::shutdown_async_executor();
            return reinterpret_cast<std::uint64_t>(&frees);
        },
        unexpected_ffi_call, unexpected_ffi_call,
        [unexpected_ffi_call](auto... args) {
            unexpected_ffi_call(args...);
            return 0;
        },
        [](std::uint64_t handle) { ++*reinterpret_cast<unsigned *>(handle); },
        [](int value) { return value; }, nullptr);
    EXPECT_EXCEPTION(wait_for_future(result), uniffi::AsyncExecutorError);
    ASSERT_EQ(frees, 1u);
}

} // namespace executor_tests
