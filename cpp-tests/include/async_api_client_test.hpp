// Copyright 2026 LiveKit, Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <async_api_client.hpp>

namespace async_api_client_test {
namespace api = async_api_client;

// Application workers for potentially blocking RustTask::execute, not runtime control workers.
class WorkerPool {
  public:
    explicit WorkerPool(size_t count = 1) : capacity_(1024) {
        for (size_t i = 0; i < count; ++i) {
            workers_.emplace_back([this] { run(); });
        }
    }
    ~WorkerPool() { shutdown(); }

    bool enqueue(uniffi::AsyncTask task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!accepting_ || tasks_.size() >= capacity_) {
                return false;
            }
            tasks_.push_back(std::move(task));
        }
        ready_.notify_one();
        return true;
    }

    size_t queued() {
        std::lock_guard<std::mutex> lock(mutex_);
        return tasks_.size();
    }

    bool wait_idle(std::chrono::seconds timeout = std::chrono::seconds(10)) {
        std::unique_lock<std::mutex> lock(mutex_);
        return idle_.wait_for(lock, timeout, [this] { return tasks_.empty() && running_ == 0; });
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            accepting_ = false;
        }
        ready_.notify_all();
        for (auto &worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    std::atomic<bool> ran_on_control{false};

  private:
    void run() {
        for (;;) {
            uniffi::AsyncTask task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [this] { return !accepting_ || !tasks_.empty(); });
                if (tasks_.empty()) {
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop_front();
                ++running_;
            }
            if (uniffi::detail::async_executor().is_worker()) {
                ran_on_control = true;
            }
            task();
            task = {};
            {
                std::lock_guard<std::mutex> lock(mutex_);
                --running_;
            }
            idle_.notify_all();
        }
    }
    std::mutex mutex_;
    std::condition_variable ready_, idle_;
    std::deque<uniffi::AsyncTask> tasks_;
    std::vector<std::thread> workers_;
    const size_t capacity_;
    size_t running_ = 0;
    bool accepting_ = true;
};

class TestHttpClient final : public api::HttpClientForeign {
  public:
    explicit TestHttpClient(std::shared_ptr<WorkerPool> workers,
                            std::string response = api::test_response_data())
        : workers_(std::move(workers)),
          response_(std::make_shared<std::string>(std::move(response))) {}

    uniffi::ForeignFuture<std::string> fetch(const std::string &url,
                                             const std::string &credentials) override {
        return uniffi::ForeignFuture<std::string>([workers = workers_, response = response_, url,
                                                   credentials](auto success, auto failure) {
            auto cancelled = std::make_shared<std::atomic<bool>>(false);
            if (!workers->enqueue([response, url, credentials, success, failure, cancelled] {
                    if (cancelled->load()) {
                        return;
                    }
                    if (credentials != "username:password" ||
                        url != "https://api.github.com/repos/mozilla/uniffi-rs/issues/2017") {
                        api::api_error::Http error;
                        error.reason = credentials != "username:password" ? "Unauthorized"
                                                                          : "Wrong URL: " + url;
                        failure(std::make_exception_ptr(error));
                    } else {
                        success(*response);
                    }
                })) {
                failure(std::make_exception_ptr(std::runtime_error("HTTP worker rejected work")));
            }
            return [cancelled] { cancelled->store(true); };
        });
    }

  private:
    std::shared_ptr<WorkerPool> workers_;
    std::shared_ptr<const std::string> response_;
};

class TestTaskRunner final : public api::TaskRunnerForeign {
  public:
    explicit TestTaskRunner(std::shared_ptr<WorkerPool> workers) : workers_(std::move(workers)) {}
    uniffi::ForeignFuture<void> run_task(const std::shared_ptr<api::RustTask> &task) override {
        return uniffi::ForeignFuture<void>([counters = counters_, workers = workers_,
                                            task](auto success, auto failure) {
            auto cancelled = std::make_shared<std::atomic<bool>>(false);
            if (!workers->enqueue([counters, task, success, failure, cancelled] {
                    if (cancelled->load()) {
                        return;
                    }
                    try {
                        task->execute();
                        ++counters->executed;
                        success();
                    } catch (...) {
                        failure(std::current_exception());
                    }
                })) {
                failure(std::make_exception_ptr(std::runtime_error("task worker rejected work")));
            }
            return [counters, cancelled] {
                cancelled->store(true);
                ++counters->cancellations;
            };
        });
    }
    size_t executed() const { return counters_->executed.load(); }
    size_t cancellations() const { return counters_->cancellations.load(); }

  private:
    struct Counters {
        std::atomic<size_t> executed{0}, cancellations{0};
    };
    std::shared_ptr<Counters> counters_ = std::make_shared<Counters>();
    std::shared_ptr<WorkerPool> workers_;
};
} // namespace async_api_client_test
