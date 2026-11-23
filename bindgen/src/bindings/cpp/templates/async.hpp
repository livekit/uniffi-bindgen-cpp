// Used for queued tasks and registered cancellation actions.
using AsyncTask = std::function<void()>;

namespace detail {

// Generated components in a linked C++ image share one worker and its FIFO queue.
// Keep worker tasks short. Shutdown also tracks first polls that run on calling threads.
class AsyncExecutor {
  public:
    AsyncExecutor() : worker_([this] { run(); }) {}
    AsyncExecutor(const AsyncExecutor &) = delete;
    AsyncExecutor &operator=(const AsyncExecutor &) = delete;

    // Destroy a directly owned executor on an application thread so it can join its worker.
    ~AsyncExecutor() { shutdown(); }

    // Queue a task, including cleanup after admission closes. Returns false once stopped.
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

    // Register a cancel action and return its ID, or zero if admission is closed.
    // When the poll_on_caller output is true, finish that first poll through run_inline().
    uint64_t track(AsyncTask cancel, bool *poll_on_caller = nullptr) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!accepting_.load(std::memory_order_relaxed)) {
            return 0;
        }
        const auto id = ++next_id_;
        active_.emplace(id, std::move(cancel));
        if (poll_on_caller) {
            *poll_on_caller = !closing_.load();
            if (*poll_on_caller) {
                ++inline_polls_;
            }
        }
        return id;
    }

    // Remove a registration so a waiting shutdown can recheck whether work has drained.
    void untrack(uint64_t id) noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        active_.erase(id);
        if (!accepting()) {
            drained_.notify_all();
        }
    }

    // Admission stays open while shutdown waits for caller-thread first polls.
    bool accepting() const noexcept { return accepting_.load(std::memory_order_acquire); }

    // Identifies the worker for blocking wait and lifecycle checks.
    bool is_worker() const noexcept { return current_worker_ == this; }

    // Run the first poll counted by track() and release its count so shutdown can proceed.
    template <typename Call> void run_inline(Call call) noexcept {
        const auto previous = current_inline_;
        current_inline_ = this;
        call();
        current_inline_ = previous;
        // Sequentially consistent with shutdown(): it sees zero polls, or this sees closing_.
        if (--inline_polls_ == 0 && closing_.load()) {
            std::lock_guard<std::mutex> guard(mutex_);
            drained_.notify_all();
        }
    }

    // Restart the worker and reopen admission after a completed shutdown.
    void start() {
        if (is_worker() || current_inline_ == this) {
            // Shutdown may be waiting for this call to finish.
            return;
        }
        std::lock_guard<std::mutex> shutdown_guard(shutdown_mutex_);
        std::lock_guard<std::mutex> guard(mutex_);
        if (stopped_) {
            worker_ = std::thread([this] { run(); });
            stopped_ = false;
            accepting_.store(true, std::memory_order_release);
        }
    }

    // Wait for caller-thread first polls, then close admission, cancel and drain work, and join.
    // New calls queue their first poll on the worker during the initial wait.
    void shutdown() {
        if (is_worker() || current_inline_ == this) {
            throw std::logic_error("UniFFI executor cannot shut down from its own callback");
        }
        std::lock_guard<std::mutex> shutdown_guard(shutdown_mutex_);
        std::vector<AsyncTask> cancellations;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stopped_) {
                return;
            }
            // Let caller-thread polls finish while callbacks can still start, so none of them
            // consumes a callback result that shutdown rejected.
            closing_.store(true);
            drained_.wait(lock, [this] { return inline_polls_.load() == 0; });
            closing_.store(false);
            accepting_.store(false, std::memory_order_release);
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
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            ready_.wait(lock, [this] { return !tasks_.empty() || stopped_; });
            if (tasks_.empty()) {
                current_worker_ = nullptr;
                return;
            }
            auto task = std::move(tasks_.front());
            tasks_.pop_front();
            running_ = true;
            lock.unlock();

            try {
                task();
            } catch (...) {
                // User continuations cannot throw across the executor boundary.
            }
            // Release captured objects before announcing that the executor has drained.
            task = {};
            lock.lock();
            running_ = false;
            if (!accepting()) {
                drained_.notify_all();
            }
        }
    }

    // Protects the queue, active operations, and worker flags.
    mutable std::mutex mutex_;
    // Serializes start and shutdown through the worker join.
    std::mutex shutdown_mutex_;
    // Wakes the worker when a task is queued or shutdown signals it to exit.
    std::condition_variable ready_;
    // Wakes shutdown to recheck its first-poll and drain conditions.
    std::condition_variable drained_;
    // Holds tasks in the order the worker will run them.
    std::deque<AsyncTask> tasks_;
    // Maps active operation IDs to shutdown cancellation actions in registration order.
    std::map<uint64_t, AsyncTask> active_;
    // Supplies increasing operation IDs across restarts.
    uint64_t next_id_ = 0;
    // Lets callers check admission with an atomic read.
    std::atomic<bool> accepting_{true};
    // Closes the task queue and tells the worker to exit.
    bool stopped_ = false;
    // Keeps shutdown waiting while a worker task runs and releases its captures.
    bool running_ = false;
    // Defers new first polls to the worker while shutdown waits for caller-thread polls.
    std::atomic<bool> closing_{false};
    // Counts first polls assigned to callers by track() until run_inline() finishes them.
    std::atomic<unsigned> inline_polls_{0};
    // Identifies the executor that owns the current worker thread.
    inline static thread_local const AsyncExecutor *current_worker_ = nullptr;
    // Identifies the executor running an inline first poll for reentrancy checks.
    inline static thread_local const AsyncExecutor *current_inline_ = nullptr;
    // Runs queued work and is joined by shutdown().
    std::thread worker_;
};

// Get the shared executor, creating its worker on first use.
inline AsyncExecutor &async_executor() {
    // Explicit shutdown avoids running callbacks or joining during static destruction.
    static auto *const executor = new AsyncExecutor();
    return *executor;
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

} // namespace detail

// Joins the shared worker after waiting for first polls, cancelling calls, and draining cleanup.
// Stop producers first and call from an application thread before unloading the bindings or Rust.
// Calling from the worker or inside a caller-thread first poll throws std::logic_error.
inline void shutdown_async_executor() { detail::async_executor().shutdown(); }

// Start the shared worker or reopen admission after shutdown.
// Calls from the worker or inside a caller-thread first poll return immediately.
inline void start_async_executor() { detail::async_executor().start(); }
