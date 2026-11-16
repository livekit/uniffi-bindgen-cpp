#include <test_common.hpp>

#include <async_api_client.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace api = async_api_client;

class WorkerPool {
public:
    WorkerPool(): worker_([this] { run(); }) {}

    ~WorkerPool() {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            accepting_ = false;
        }
        ready_.notify_all();
        worker_.join();
    }

    bool dispatch(uniffi::AsyncTask task) {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (!accepting_) {
                return false;
            }
            tasks_.push_back(std::move(task));
        }
        ready_.notify_one();
        return true;
    }

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
            }
            task();
        }
    }

    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<uniffi::AsyncTask> tasks_;
    bool accepting_ = true;
    std::thread worker_;
};

class TestHttpClient final : public api::HttpClientForeign {
public:
    uniffi::ForeignFuture<std::string> fetch(
        const std::string &url,
        const std::string &credentials
    ) override {
        return uniffi::ForeignFuture<std::string>(
            [url, credentials](auto success, auto failure) {
                if (
                    credentials == "username:password" &&
                    url == "https://api.github.com/repos/mozilla/uniffi-rs/issues/2017"
                ) {
                    success(api::test_response_data());
                } else {
                    failure(std::make_exception_ptr(
                        std::runtime_error("unexpected HTTP request")
                    ));
                }
                return [] {};
            }
        );
    }
};

class TestTaskRunner final : public api::TaskRunnerForeign {
public:
    explicit TestTaskRunner(std::shared_ptr<WorkerPool> workers):
        workers_(std::move(workers)) {}

    uniffi::ForeignFuture<void> run_task(
        const std::shared_ptr<api::RustTask> &task
    ) override {
        auto workers = workers_;
        return uniffi::ForeignFuture<void>(
            [workers, task](auto success, auto failure) {
                auto cancelled = std::make_shared<std::atomic<bool>>(false);
                if (!workers->dispatch([task, success, failure, cancelled] {
                    if (cancelled->load()) {
                        return;
                    }
                    try {
                        task->execute();
                        success();
                    } catch (...) {
                        failure(std::current_exception());
                    }
                })) {
                    failure(std::make_exception_ptr(
                        std::runtime_error("task worker rejected work")
                    ));
                }
                return [cancelled] { cancelled->store(true); };
            }
        );
    }

private:
    std::shared_ptr<WorkerPool> workers_;
};

int main() {
    auto workers = std::make_shared<WorkerPool>();
    auto client = api::ApiClient::init(
        std::make_shared<TestHttpClient>(),
        std::make_shared<TestTaskRunner>(workers)
    );

    auto issue = client->get_issue("mozilla", "uniffi-rs", 2017).get();
    ASSERT_EQ("Foreign-implemented async traits", issue.title);
    ASSERT_EQ(api::IssueState::kOpen, issue.state);

    return 0;
}
