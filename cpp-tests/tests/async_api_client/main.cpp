#include <async_api_client_test.hpp>
#include <test_common.hpp>

using namespace std::chrono_literals;
namespace api = async_api_client;
using namespace async_api_client_test;

template <typename Predicate> void eventually(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!predicate()) {
        ASSERT_TRUE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}

int main() {
    auto workers = std::make_shared<WorkerPool>();
    auto runner = std::make_shared<TestTaskRunner>(workers);
    auto client = api::ApiClient::init(std::make_shared<TestHttpClient>(workers), runner);

    // The upstream success case, with credentials executed on application workers.
    auto issue = client->get_issue("mozilla", "uniffi-rs", 2017).get();
    ASSERT_EQ("Foreign-implemented async traits", issue.title);
    ASSERT_EQ(api::IssueState::kOpen, issue.state);
    ASSERT_EQ("https://api.github.com/repos/mozilla/uniffi-rs/issues/2017", issue.url);
    ASSERT_EQ(1, runner->executed());
    ASSERT_FALSE(workers->ran_on_control.load());

    // A declared foreign HTTP error must survive C++ -> Rust -> C++ as the same variant and fields.
    try {
        client->get_issue("wrong", "repository", 2017).get();
        ASSERT_TRUE(false);
    } catch (const api::api_error::Http &error) {
        ASSERT_EQ("Wrong URL: https://api.github.com/repos/wrong/repository/issues/2017",
                  error.reason);
    }
    auto invalid =
        api::ApiClient::init(std::make_shared<TestHttpClient>(workers, "invalid JSON"), runner);
    EXPECT_EXCEPTION(invalid->get_issue("mozilla", "uniffi-rs", 2017).get(), api::api_error::Json);
    invalid.reset();

    std::vector<uniffi::Future<api::Issue>> calls;
    for (int i = 0; i < 32; ++i) {
        calls.push_back(client->get_issue("mozilla", "uniffi-rs", 2017));
    }
    for (auto &call : calls) {
        ASSERT_EQ("Foreign-implemented async traits", call.get().title);
    }
    ASSERT_TRUE(workers->wait_idle());

    // Hold the sole application worker so a queued RustTask can be deterministically cancelled.
    auto entered = std::make_shared<std::promise<void>>();
    auto release = std::make_shared<std::promise<void>>();
    auto released = release->get_future().share();
    ASSERT_TRUE(workers->enqueue([entered, released] {
        entered->set_value();
        released.wait();
    }));
    ASSERT_EQ(std::future_status::ready, entered->get_future().wait_for(5s));
    const auto before = runner->executed();
    const auto cancelled_before = runner->cancellations();
    auto cancelled = client->get_issue("mozilla", "uniffi-rs", 2017);
    eventually([&] { return workers->queued() != 0; });
    cancelled.cancel();
    EXPECT_EXCEPTION(cancelled.get(), uniffi::AsyncCancelledError);
    eventually([&] { return runner->cancellations() > cancelled_before; });
    release->set_value();
    ASSERT_TRUE(workers->wait_idle());
    ASSERT_EQ(before, runner->executed());
    ASSERT_FALSE(workers->ran_on_control.load());

    client.reset();
    uniffi::shutdown_async_executor();
    workers->shutdown();
}
