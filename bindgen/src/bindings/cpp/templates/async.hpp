class AsyncCancelledError : public std::runtime_error {
  public:
    AsyncCancelledError() : std::runtime_error("UniFFI async call cancelled") {}
};

class AsyncExecutorError : public std::runtime_error {
  public:
    AsyncExecutorError() : std::runtime_error("UniFFI async executor rejected a task") {}
};

using AsyncTask = std::function<void()>;

namespace detail {

// One control executor per linked C++ image, shared by all generated components. Tasks must not
// block.
class AsyncExecutor {
  public:
    AsyncExecutor() : worker_([this] { run(); }) {}
    AsyncExecutor(const AsyncExecutor &) = delete;
    AsyncExecutor &operator=(const AsyncExecutor &) = delete;
    ~AsyncExecutor() { shutdown(); }

    bool enqueue(AsyncTask task) {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (stopped_) {
                return false;
            }
            tasks_.push_back(std::move(task));
        }
        ready_.notify_one();
        return true;
    }

    uint64_t track(AsyncTask cancel) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!accepting_) {
            return 0;
        }
        const auto id = ++next_id_;
        active_.emplace(id, std::move(cancel));
        return id;
    }

    void untrack(uint64_t id) noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        active_.erase(id);
        drained_.notify_all();
    }

    bool accepting() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return accepting_;
    }

    bool is_worker() const noexcept { return current_worker_ == this; }

    void shutdown() {
        if (is_worker()) {
            throw std::logic_error("UniFFI executor cannot shut down from its own callback");
        }
        std::lock_guard<std::mutex> shutdown_guard(shutdown_mutex_);
        std::vector<AsyncTask> cancellations;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (stopped_) {
                return;
            }
            accepting_ = false;
            for (const auto &entry : active_) {
                cancellations.push_back(entry.second);
            }
        }
        for (auto &cancel : cancellations) {
            cancel();
        }
        {
            std::unique_lock<std::mutex> lock(mutex_);
            drained_.wait(lock, [this] { return active_.empty() && tasks_.empty() && !running_; });
            stopped_ = true;
        }
        ready_.notify_all();
        worker_.join();
    }

  private:
    void run() noexcept {
        current_worker_ = this;
        for (;;) {
            AsyncTask task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [this] { return !tasks_.empty() || stopped_; });
                if (tasks_.empty()) {
                    current_worker_ = nullptr;
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop_front();
                running_ = true;
            }
            try {
                task();
            } catch (...) {
                // User continuations cannot throw across the executor boundary.
            }
            // Release captured objects before announcing that the executor has drained.
            task = {};
            {
                std::lock_guard<std::mutex> guard(mutex_);
                running_ = false;
            }
            drained_.notify_all();
        }
    }

    mutable std::mutex mutex_;
    std::mutex shutdown_mutex_;
    std::condition_variable ready_;
    std::condition_variable drained_;
    std::deque<AsyncTask> tasks_;
    std::map<uint64_t, AsyncTask> active_;
    uint64_t next_id_ = 0;
    bool accepting_ = true;
    bool stopped_ = false;
    bool running_ = false;
    inline static thread_local const AsyncExecutor *current_worker_ = nullptr;
    std::thread worker_;
};

inline AsyncExecutor &async_executor() {
    static AsyncExecutor executor;
    return executor;
}

inline bool enqueue_async(AsyncTask task) noexcept {
    try {
        return async_executor().enqueue(std::move(task));
    } catch (...) {
        // An accepted operation must retain a deferred cleanup path even under allocation failure.
        std::terminate();
    }
}

inline bool begin_async_call() { return async_executor().accepting(); }

inline void check_blocking_wait(bool ready) {
    if (!ready && async_executor().is_worker()) {
        throw std::logic_error("Cannot block the UniFFI executor; use Future::then instead");
    }
}

} // namespace detail

// Cancels in-flight calls, drains their Rust handles and queued completions, and joins the worker.
// Stop producers first. Call from an application thread, before unloading the Rust library.
inline void shutdown_async_executor() { detail::async_executor().shutdown(); }

// C++17 adapter for an async callback-interface method. The implementation starts work when the
// generated bridge supplies success and failure callbacks, and returns an optional cancel action.
template <typename T> class ForeignFuture {
  public:
    using Cancel = std::function<void()>;
    using Success = std::function<void(T)>;
    using Failure = std::function<void(std::exception_ptr)>;
    using Start = std::function<Cancel(Success, Failure)>;

    ForeignFuture() = default;
    explicit ForeignFuture(Start start) : start_(std::move(start)) {}

    ForeignFuture(const ForeignFuture &) = delete;
    ForeignFuture &operator=(const ForeignFuture &) = delete;
    ForeignFuture(ForeignFuture &&) noexcept = default;
    ForeignFuture &operator=(ForeignFuture &&) noexcept = default;

    Cancel start(Success success, Failure failure) && {
        if (!start_) {
            throw std::logic_error("UniFFI foreign future is invalid or has already been started");
        }
        auto start = std::move(start_);
        return start(std::move(success), std::move(failure));
    }

  private:
    Start start_;
};

template <> class ForeignFuture<void> {
  public:
    using Cancel = std::function<void()>;
    using Success = std::function<void()>;
    using Failure = std::function<void(std::exception_ptr)>;
    using Start = std::function<Cancel(Success, Failure)>;

    ForeignFuture() = default;
    explicit ForeignFuture(Start start) : start_(std::move(start)) {}

    ForeignFuture(const ForeignFuture &) = delete;
    ForeignFuture &operator=(const ForeignFuture &) = delete;
    ForeignFuture(ForeignFuture &&) noexcept = default;
    ForeignFuture &operator=(ForeignFuture &&) noexcept = default;

    Cancel start(Success success, Failure failure) && {
        if (!start_) {
            throw std::logic_error("UniFFI foreign future is invalid or has already been started");
        }
        auto start = std::move(start_);
        return start(std::move(success), std::move(failure));
    }

  private:
    Start start_;
};

template <typename T> class FutureResult {
  public:
    explicit FutureResult(T value) : value_(std::move(value)) {}
    explicit FutureResult(std::exception_ptr error) noexcept : error_(std::move(error)) {}

    FutureResult(const FutureResult &) = default;
    FutureResult &operator=(const FutureResult &) = default;
    FutureResult &operator=(FutureResult &&) = default;

    FutureResult(FutureResult &&other) noexcept : error_(std::move(other.error_)) {
        if (other.value_) {
            try {
                value_.emplace(std::move(*other.value_));
            } catch (...) {
                // Deliver a failed value transfer through the future's normal error path.
                error_ = std::current_exception();
            }
        }
    }

    bool has_value() const noexcept { return !error_; }
    std::exception_ptr error() const noexcept { return error_; }

    T get() && {
        if (error_) {
            std::rethrow_exception(error_);
        }
        return std::move(*value_);
    }

  private:
    std::optional<T> value_;
    std::exception_ptr error_;
};

template <> class FutureResult<void> {
  public:
    FutureResult() = default;
    explicit FutureResult(std::exception_ptr error) noexcept : error_(std::move(error)) {}

    bool has_value() const noexcept { return !error_; }
    std::exception_ptr error() const noexcept { return error_; }

    void get() const {
        if (error_) {
            std::rethrow_exception(error_);
        }
    }

  private:
    std::exception_ptr error_;
};

namespace detail {

// Retains the executor through delivery, including when then() is attached to an already-ready
// call.
struct ContinuationRegistration {
    ContinuationRegistration() : id(async_executor().track([] {})) {
        if (!id) {
            throw AsyncExecutorError();
        }
    }
    ~ContinuationRegistration() { async_executor().untrack(id); }
    uint64_t id;
};

template <typename T> class FutureCompletionState {
  public:
    using Result = FutureResult<T>;
    using Callback = std::function<void(Result)>;

    void complete(Result result) noexcept {
        Callback callback;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (result_) {
                return;
            }
            result_.emplace(std::move(result));
            callback = std::move(callback_);
        }
        ready_.notify_all();
        if (callback) {
            enqueue_continuation(std::move(callback));
        }
    }

    T get() {
        std::unique_lock<std::mutex> lock(mutex_);
        check_blocking_wait(result_.has_value());
        claim_locked();
        ready_.wait(lock, [this] { return result_.has_value(); });
        auto result = std::move(*result_);
        lock.unlock();
        return std::move(result).get();
    }

    void wait() const {
        std::unique_lock<std::mutex> lock(mutex_);
        check_blocking_wait(result_.has_value());
        ready_.wait(lock, [this] { return result_.has_value(); });
    }

    template <typename Rep, typename Period>
    std::future_status wait_for(const std::chrono::duration<Rep, Period> &timeout) const {
        std::unique_lock<std::mutex> lock(mutex_);
        check_blocking_wait(result_.has_value() || timeout <= timeout.zero());
        return ready_.wait_for(lock, timeout, [this] { return result_.has_value(); })
                   ? std::future_status::ready
                   : std::future_status::timeout;
    }

    void then(Callback callback) {
        auto registration = std::make_shared<ContinuationRegistration>();
        callback = [registration, callback = std::move(callback)](Result result) mutable {
            callback(std::move(result));
        };
        bool ready;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            claim_locked();
            ready = result_.has_value();
            if (!ready) {
                callback_ = std::move(callback);
                return;
            }
        }
        enqueue_continuation(std::move(callback));
    }

  private:
    void claim_locked() {
        if (claimed_) {
            throw std::logic_error("UniFFI future result has already been consumed");
        }
        claimed_ = true;
    }

    void enqueue_continuation(Callback callback) noexcept {
        Result result = [&] {
            std::lock_guard<std::mutex> guard(mutex_);
            return std::move(*result_);
        }();
        auto delivery =
            std::make_shared<std::pair<Callback, Result>>(std::move(callback), std::move(result));
        if (!enqueue_async([delivery]() mutable {
                try {
                    delivery->first(std::move(delivery->second));
                } catch (...) {
                    // User exceptions do not escape the executor thread.
                }
            })) {
            std::terminate();
        }
    }

    mutable std::mutex mutex_;
    mutable std::condition_variable ready_;
    std::optional<Result> result_;
    bool claimed_ = false;
    Callback callback_;
};

template <> inline void FutureCompletionState<void>::get() {
    std::unique_lock<std::mutex> lock(mutex_);
    check_blocking_wait(result_.has_value());
    claim_locked();
    ready_.wait(lock, [this] { return result_.has_value(); });
    auto result = std::move(*result_);
    lock.unlock();
    result.get();
}

} // namespace detail

class FutureContinuation {
  public:
    FutureContinuation() = default;
    explicit FutureContinuation(std::function<void()> cancel) : cancel_(std::move(cancel)) {}

    FutureContinuation(const FutureContinuation &) = delete;
    FutureContinuation &operator=(const FutureContinuation &) = delete;

    FutureContinuation(FutureContinuation &&other) noexcept : cancel_(std::move(other.cancel_)) {
        other.cancel_ = nullptr;
    }

    FutureContinuation &operator=(FutureContinuation &&other) noexcept {
        if (this != &other) {
            cancel();
            cancel_ = std::move(other.cancel_);
            other.cancel_ = nullptr;
        }
        return *this;
    }

    ~FutureContinuation() { cancel(); }

    void cancel() noexcept {
        if (cancel_) {
            auto cancel = std::move(cancel_);
            cancel_ = nullptr;
            cancel();
        }
    }

  private:
    std::function<void()> cancel_;
};

template <typename T> class Future {
  public:
    using Result = FutureResult<T>;
    using Callback = std::function<void(Result)>;

    Future() = default;
    Future(std::shared_ptr<detail::FutureCompletionState<T>> state,
           std::function<void()> cancel) noexcept
        : state_(std::move(state)), cancel_(std::move(cancel)) {}

    Future(const Future &) = delete;
    Future &operator=(const Future &) = delete;

    Future(Future &&other) noexcept
        : state_(std::move(other.state_)), cancel_(std::move(other.cancel_)) {
        other.cancel_ = nullptr;
    }

    Future &operator=(Future &&other) noexcept {
        if (this != &other) {
            cancel();
            state_ = std::move(other.state_);
            cancel_ = std::move(other.cancel_);
            other.cancel_ = nullptr;
        }
        return *this;
    }

    ~Future() { cancel(); }

    bool valid() const noexcept { return state_ != nullptr; }

    decltype(auto) get() {
        require_state();
        detail::check_blocking_wait(state_->wait_for(std::chrono::seconds(0)) ==
                                    std::future_status::ready);
        auto state = std::move(state_);
        cancel_ = nullptr;
        return state->get();
    }

    void wait() const {
        require_state();
        state_->wait();
    }

    template <typename Rep, typename Period>
    std::future_status wait_for(const std::chrono::duration<Rep, Period> &timeout) const {
        require_state();
        return state_->wait_for(timeout);
    }

    [[nodiscard]] FutureContinuation then(Callback callback) && {
        require_state();
        if (!callback) {
            throw std::invalid_argument("UniFFI future continuation must not be empty");
        }
        state_->then(std::move(callback));
        auto cancel = std::move(cancel_);
        cancel_ = nullptr;
        state_.reset();
        return FutureContinuation(std::move(cancel));
    }

    void cancel() noexcept {
        if (cancel_) {
            auto cancel = std::move(cancel_);
            cancel_ = nullptr;
            cancel();
        }
    }

  private:
    void require_state() const {
        if (!state_) {
            throw std::future_error(std::future_errc::no_state);
        }
    }

    std::shared_ptr<detail::FutureCompletionState<T>> state_;
    std::function<void()> cancel_;
};
