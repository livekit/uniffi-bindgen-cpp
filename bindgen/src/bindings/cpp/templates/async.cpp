constexpr int8_t UNIFFI_RUST_FUTURE_POLL_READY = 0;
constexpr int8_t UNIFFI_RUST_FUTURE_POLL_WAKE = 1;

// Coordinates completion and Rust-driven cancellation for a C++ implementation of an async
// callback-interface method.  Dropping the Rust future wins the completion race, invokes the
// operation's cancel action, and sends a cancelled result exactly once.
class ForeignFutureTaskState {
  public:
    void enqueue_or_cancel(::uniffi::AsyncTask task) noexcept {
        if (!::uniffi::detail::enqueue_async(std::move(task))) {
            cancel();
        }
    }

    void start(const std::shared_ptr<ForeignFutureTaskState> &self) {
        std::lock_guard<std::mutex> guard(mutex_);
        auto weak = std::weak_ptr<ForeignFutureTaskState>(self);
        operation_id_ = ::uniffi::detail::async_executor().track([weak] {
            if (auto state = weak.lock()) {
                state->enqueue_or_cancel([state] { state->cancel(); });
            }
        });
    }

    bool finish() noexcept {
        std::function<void()> cancel;
        std::function<void()> complete_cancelled;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (finished_) {
                return false;
            }
            finished_ = true;
            cancel = std::move(cancel_);
            complete_cancelled = std::move(complete_cancelled_);
        }
        ::uniffi::detail::async_executor().untrack(operation_id_);
        // Releasing captures can destroy application objects; do it outside the state mutex.
        return true;
    }

    void set_cancel(std::function<void()> cancel) noexcept {
        std::function<void()> cancel_now;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (cancel_requested_) {
                cancel_now = std::move(cancel);
            } else if (!finished_) {
                cancel_ = std::move(cancel);
            }
        }
        if (cancel_now) {
            try {
                cancel_now();
            } catch (...) {
            }
        }
    }

    void set_cancel_completion(std::function<void()> complete_cancelled) {
        std::lock_guard<std::mutex> guard(mutex_);
        complete_cancelled_ = std::move(complete_cancelled);
    }

    void cancel() noexcept {
        std::function<void()> cancel;
        std::function<void()> complete_cancelled;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (finished_) {
                return;
            }
            finished_ = true;
            cancel_requested_ = true;
            cancel = std::move(cancel_);
            complete_cancelled = std::move(complete_cancelled_);
        }
        if (cancel) {
            try {
                cancel();
            } catch (...) {
            }
        }
        if (complete_cancelled) {
            complete_cancelled();
        }
        ::uniffi::detail::async_executor().untrack(operation_id_);
    }

  private:
    std::mutex mutex_;
    uint64_t operation_id_ = 0;
    bool finished_ = false;
    bool cancel_requested_ = false;
    std::function<void()> cancel_;
    std::function<void()> complete_cancelled_;
};

inline uint64_t foreign_future_handle(const std::shared_ptr<ForeignFutureTaskState> &state) {
    return reinterpret_cast<uint64_t>(new std::shared_ptr<ForeignFutureTaskState>(state));
}

inline void foreign_future_drop(uint64_t handle) noexcept {
    auto state = std::unique_ptr<std::shared_ptr<ForeignFutureTaskState>>(
        reinterpret_cast<std::shared_ptr<ForeignFutureTaskState> *>(handle));
    auto task_state = *state;
    task_state->enqueue_or_cancel([task_state] { task_state->cancel(); });
}

template <typename Free> class RustFutureHandleGuard {
  public:
    RustFutureHandleGuard(uint64_t handle, Free &free) noexcept : handle_(handle), free_(free) {}

    RustFutureHandleGuard(const RustFutureHandleGuard &) = delete;
    RustFutureHandleGuard &operator=(const RustFutureHandleGuard &) = delete;

    ~RustFutureHandleGuard() {
        if (active_) {
            free_(handle_);
        }
    }

    void release() noexcept { active_ = false; }

  private:
    uint64_t handle_;
    Free &free_;
    bool active_ = true;
};

template <typename T, typename Poll, typename Cancel, typename Complete, typename Free,
          typename Lift, typename ErrorHandler>
class RustFutureState : public std::enable_shared_from_this<
                            RustFutureState<T, Poll, Cancel, Complete, Free, Lift, ErrorHandler>> {
  public:
    RustFutureState(uint64_t handle, Poll poll, Cancel cancel, Complete complete, Free free,
                    Lift lift, ErrorHandler error_handler,
                    std::shared_ptr<::uniffi::detail::FutureCompletionState<T>> completion_state)
        : handle_(handle), poll_(std::move(poll)), cancel_(std::move(cancel)),
          complete_(std::move(complete)), free_(std::move(free)), lift_(std::move(lift)),
          error_handler_(std::move(error_handler)), completion_state_(std::move(completion_state)) {
    }

    bool start() {
        const auto self = this->shared_from_this();
        {
            std::lock_guard<std::mutex> guard(mutex_);
            operation_id_ =
                ::uniffi::detail::async_executor().track([self] { self->cancel(); });
        }
        if (!operation_id_) {
            return false;
        }
        request_drive();
        return true;
    }

    void cancel() noexcept {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (terminal_) {
                return;
            }
            cancel_requested_ = true;
        }
        request_drive();
    }

  private:
    enum class Action {
        None,
        PollFuture,
        CancelFuture,
        CompleteFuture,
    };

    using State = RustFutureState<T, Poll, Cancel, Complete, Free, Lift, ErrorHandler>;

    static void continuation(uint64_t callback_data, int8_t poll_result) noexcept {
        // The active-operation registry owns the state until READY has been consumed and freed.
        auto state = reinterpret_cast<State *>(callback_data)->shared_from_this();
        state->on_continuation(poll_result);
    }

    void on_continuation(int8_t poll_result) noexcept {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (terminal_) {
                return;
            }
            poll_outstanding_ = false;
            if (poll_result == UNIFFI_RUST_FUTURE_POLL_READY) {
                ready_ = true;
            } else if (poll_result == UNIFFI_RUST_FUTURE_POLL_WAKE) {
                poll_requested_ = true;
            } else {
                failure_ = std::make_exception_ptr(
                    std::runtime_error("Unexpected UniFFI Rust future poll result"));
                cancel_requested_ = true;
            }
        }
        request_drive();
    }

    void request_drive() noexcept {
        std::shared_ptr<State> state;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (terminal_ || driving_ || scheduled_) {
                return;
            }
            scheduled_ = true;
            state = this->shared_from_this();
        }

        if (!::uniffi::detail::enqueue_async([state] { state->drive(); })) {
            // Active operations keep the executor alive until their handles have been released.
            std::terminate();
        }
    }

    Action next_action() noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        if (terminal_) {
            return Action::None;
        }
        if (ready_) {
            terminal_ = true;
            return Action::CompleteFuture;
        }
        if (cancel_requested_ && !cancel_sent_) {
            cancel_sent_ = true;
            return Action::CancelFuture;
        }
        if (poll_requested_ && !poll_outstanding_) {
            poll_requested_ = false;
            poll_outstanding_ = true;
            return Action::PollFuture;
        }
        return Action::None;
    }

    void drive() noexcept {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            scheduled_ = false;
            if (terminal_ || driving_) {
                return;
            }
            driving_ = true;
        }

        // Bound each turn so a rapidly waking future cannot starve other calls.
        for (unsigned turn = 0; turn < 64; ++turn) {
            switch (next_action()) {
            case Action::PollFuture:
                poll();
                break;
            case Action::CancelFuture:
                cancel_rust_future();
                break;
            case Action::CompleteFuture:
                complete_and_free();
                {
                    std::lock_guard<std::mutex> guard(mutex_);
                    driving_ = false;
                }
                return;
            case Action::None: {
                std::lock_guard<std::mutex> guard(mutex_);
                // A continuation can arrive after next_action() releases the mutex but before
                // this block acquires it.  It observes driving_ and deliberately does not enqueue
                // another driver, so re-check the state before yielding ownership.
                if (!terminal_ && (ready_ || (cancel_requested_ && !cancel_sent_) ||
                                   (poll_requested_ && !poll_outstanding_))) {
                    continue;
                }
                driving_ = false;
            }
                return;
            }
        }
        {
            std::lock_guard<std::mutex> guard(mutex_);
            driving_ = false;
        }
        request_drive();
    }

    void poll() noexcept {
        // UniFFI invokes exactly one continuation per poll. Reuse the state's address instead of
        // allocating callback userdata on every wake; the registry retains it until completion.
        poll_(handle_, &RustFutureState::continuation, reinterpret_cast<uint64_t>(this));
    }

    void cancel_rust_future() noexcept {
        cancel_(handle_);
        // If cancellation happened before the first poll there is no continuation to wake.  Polling
        // the cancelled Rust future yields READY immediately and keeps us on the documented path.
        std::lock_guard<std::mutex> guard(mutex_);
        if (!poll_outstanding_ && !ready_) {
            poll_requested_ = true;
        }
    }

    void complete_and_free() noexcept {
        std::optional<::uniffi::FutureResult<T>> result;
        try {
            RustCallStatus status{};
            if constexpr (std::is_void_v<T>) {
                complete_(handle_, &status);
                check_rust_call(status, error_handler_);
                result.emplace();
            } else {
                auto value = complete_(handle_, &status);
                check_rust_call(status, error_handler_);
                result.emplace(lift_(value));
            }
        } catch (...) {
            result.emplace(std::current_exception());
        }

        std::exception_ptr protocol_failure;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            protocol_failure = failure_;
        }

        free_(handle_);
        ::uniffi::detail::async_executor().untrack(operation_id_);

        if (protocol_failure) {
            result.emplace(std::move(protocol_failure));
        }
        completion_state_->complete(std::move(*result));
    }

    uint64_t handle_;
    uint64_t operation_id_ = 0;
    Poll poll_;
    Cancel cancel_;
    Complete complete_;
    Free free_;
    Lift lift_;
    ErrorHandler error_handler_;
    std::shared_ptr<::uniffi::detail::FutureCompletionState<T>> completion_state_;
    std::mutex mutex_;
    std::exception_ptr failure_;
    bool poll_requested_ = true;
    bool poll_outstanding_ = false;
    bool cancel_requested_ = false;
    bool cancel_sent_ = false;
    bool ready_ = false;
    bool scheduled_ = false;
    bool driving_ = false;
    bool terminal_ = false;
};

template <typename T, typename RustFuture, typename Poll, typename Cancel, typename Complete,
          typename Free, typename Lift, typename ErrorHandler>
Future<T> rust_call_async(RustFuture rust_future, Poll poll, Cancel cancel, Complete complete,
                          Free free, Lift lift, ErrorHandler error_handler) {
    initialize();
    auto completion_state = std::make_shared<::uniffi::detail::FutureCompletionState<T>>();
    auto rejected = [&completion_state] {
        completion_state->complete(
            ::uniffi::FutureResult<T>(std::make_exception_ptr(::uniffi::AsyncExecutorError())));
        return Future<T>(std::move(completion_state), nullptr);
    };
    if (!::uniffi::detail::begin_async_call()) {
        return rejected();
    }

    const auto handle = rust_future();
    RustFutureHandleGuard<Free> handle_guard(handle, free);
    using State = RustFutureState<T, Poll, Cancel, Complete, Free, Lift, ErrorHandler>;

    auto state =
        std::make_shared<State>(handle, std::move(poll), std::move(cancel), std::move(complete),
                                free, std::move(lift), std::move(error_handler), completion_state);
    // Prepare the callback while the guard owns the handle. Arm Future cancellation after start()
    // so a registration exception leaves the guard as the only cleanup path.
    std::function<void()> cancel_future = [weak_state = std::weak_ptr<State>(state)] {
        if (auto state = weak_state.lock()) {
            state->cancel();
        }
    };

    if (!state->start()) {
        return rejected();
    }
    handle_guard.release();
    return Future<T>(std::move(completion_state), std::move(cancel_future));
}
