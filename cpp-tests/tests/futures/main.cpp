#include <test_common.hpp>

#include <async_calls.hpp>
#include <futures.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <filesystem>
#endif

// Measure actual process threads as well as checking which thread executes callbacks.
std::optional<size_t> thread_count() {
#if defined(__APPLE__)
    thread_act_array_t threads;
    mach_msg_type_number_t count;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
        return {};
    }
    for (mach_msg_type_number_t i = 0; i < count; ++i) {
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads),
                  count * sizeof(thread_t));
    return count;
#elif defined(__linux__)
    size_t count = 0;
    for (const auto &entry : std::filesystem::directory_iterator("/proc/self/task")) {
        static_cast<void>(entry);
        ++count;
    }
    return count;
#else
    return {};
#endif
}

using namespace std::chrono_literals;

template <typename Predicate> void eventually(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!predicate()) {
        ASSERT_TRUE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
    }
}

class TestAsyncParser final : public async_calls::TestParserForeign {
  public:
    TestAsyncParser(std::shared_ptr<std::atomic<int>> started_count,
                    std::shared_ptr<std::atomic<int>> cancellation_count,
                    std::shared_ptr<std::atomic<int>> destruction_count)
        : started_count_(std::move(started_count)),
          cancellation_count_(std::move(cancellation_count)),
          destruction_count_(std::move(destruction_count)) {}

    ~TestAsyncParser() override { destruction_count_->fetch_add(1); }

    uniffi::ForeignFuture<std::string> stringify(int32_t value) override {
        ASSERT_TRUE(::uniffi::detail::async_executor().is_worker());
        return uniffi::ForeignFuture<std::string>([value](auto success, auto failure) {
            success(std::to_string(value));
            // Only the first completion should reach Rust.
            success("duplicate");
            failure(std::make_exception_ptr(std::runtime_error("duplicate failure")));
            return [] {};
        });
    }

    uniffi::ForeignFuture<int32_t> parse(const std::string &value) override {
        if (value == "throw") {
            throw std::runtime_error("C++ callback method threw");
        }
        if (value == "empty") {
            return {};
        }
        return uniffi::ForeignFuture<int32_t>([value](auto success, auto failure) {
            if (value == "unexpected") {
                failure(std::make_exception_ptr(std::runtime_error("C++ callback failed")));
            } else {
                try {
                    success(std::stoi(value));
                } catch (...) {
                    failure(std::make_exception_ptr(
                        async_calls::async_parser_error::InvalidInteger("invalid integer")));
                }
            }
            return [] {};
        });
    }

    uniffi::ForeignFuture<void> wait() override {
        auto started_count = started_count_;
        auto cancellation_count = cancellation_count_;
        return uniffi::ForeignFuture<void>([started_count, cancellation_count](auto, auto) {
            started_count->fetch_add(1);
            return [cancellation_count] { cancellation_count->fetch_add(1); };
        });
    }

  private:
    std::shared_ptr<std::atomic<int>> started_count_;
    std::shared_ptr<std::atomic<int>> cancellation_count_;
    std::shared_ptr<std::atomic<int>> destruction_count_;
};

class TestGreeter final : public async_calls::AsyncGreeter {
  public:
    uniffi::ForeignFuture<std::string> greeting() override {
        return uniffi::ForeignFuture<std::string>([](auto success, auto) {
            success("Hello from a C++ callback!");
            return nullptr;
        });
    }
};

int main() {
    ASSERT_EQ("Hello, C++", futures::greet("C++"));
    ASSERT_TRUE(futures::always_ready().get());
    futures::void_().get();
    ASSERT_EQ("Hello, Future!", futures::say().get());
    ASSERT_EQ("Hello, Future!", futures::say_after(1, "Future").get());
    ASSERT_TRUE(futures::sleep(1).get());
    futures::sleep_no_return(1).get();

    auto waitable = futures::say_after(1, "Wait");
    waitable.wait();
    ASSERT_EQ("Hello, Wait!", waitable.get());

    {
        auto pending = async_calls::pending();
        ASSERT_EQ(std::future_status::timeout, pending.wait_for(1ms));
        ASSERT_EQ(1, async_calls::pending_count());
        pending.cancel();
        EXPECT_EXCEPTION(pending.get(), uniffi::AsyncCancelledError);
        ASSERT_EQ(0, async_calls::pending_count());
    }

    {
        auto pending = async_calls::pending();
        ASSERT_EQ(1, async_calls::pending_count());
    }
    eventually([] { return async_calls::pending_count() == 0; });

    for (auto i = 0; i < 100; ++i) {
        auto racing = futures::say_after(1, "Race");
        if (i % 2 == 0) {
            std::this_thread::sleep_for(1ms);
        }
        racing.cancel();
        try {
            ASSERT_EQ("Hello, Race!", racing.get());
        } catch (const uniffi::AsyncCancelledError &) {
        }
    }

    ASSERT_EQ(42, futures::fallible_me(false).get());
    EXPECT_EXCEPTION(futures::fallible_me(true).get(), futures::my_error::Foo);

    auto megaphone = futures::new_megaphone();
    ASSERT_EQ("HELLO, WORLD!", megaphone->say_after(1, "World").get());
    ASSERT_EQ("", megaphone->silence().get());
    ASSERT_EQ(42, megaphone->fallible_me(false).get());
    EXPECT_EXCEPTION(megaphone->fallible_me(true).get(), futures::my_error::Foo);

    ASSERT_TRUE(futures::fallible_struct(false).get() != nullptr);
    EXPECT_EXCEPTION(futures::fallible_struct(true).get(), futures::my_error::Foo);
    ASSERT_TRUE(futures::async_new_megaphone().get() != nullptr);
    ASSERT_TRUE(futures::async_maybe_new_megaphone(true).get() != nullptr);
    ASSERT_TRUE(futures::async_maybe_new_megaphone(false).get() == nullptr);
    ASSERT_EQ("HELLO, FRIEND!", futures::say_after_with_megaphone(megaphone, 1, "Friend").get());

    ASSERT_TRUE(futures::Megaphone::init().get() != nullptr);
    ASSERT_TRUE(futures::Megaphone::secondary().get() != nullptr);
    EXPECT_EXCEPTION(futures::FallibleMegaphone::init().get(), futures::my_error::Foo);

    auto udl_megaphone = futures::UdlMegaphone::init().get();
    ASSERT_EQ("HELLO, UDL!", udl_megaphone->say_after(1, "Udl").get());
    ASSERT_TRUE(futures::UdlMegaphone::secondary().get() != nullptr);

    auto record = futures::new_my_record("value", 42).get();
    ASSERT_EQ("value", record.a);
    ASSERT_EQ(42, record.b);
    auto record_value = async_calls::MyRecord{"value", 17}.value_after(1);
    ASSERT_EQ(17, record_value.get());

    async_calls::validate(async_calls::ValidationMode::kAllow).get();
    EXPECT_EXCEPTION(async_calls::validate(async_calls::ValidationMode::kReject).get(),
                     async_calls::my_error::Foo);

    for (const auto &trait : futures::get_say_after_traits()) {
        ASSERT_EQ(trait->say_after(1, "Trait").get(), "Hello, Trait!");
    }
    for (const auto &trait : futures::get_say_after_udl_traits()) {
        ASSERT_EQ(trait->say_after(1, "UDL trait").get(), "Hello, UDL trait!");
    }

    auto holding = futures::use_shared_resource({2000, 4000});
    EXPECT_EXCEPTION(futures::use_shared_resource({0, 250}).get(), futures::async_error::Timeout);
    holding.get();

    auto started_count = std::make_shared<std::atomic<int>>(0);
    auto cancellation_count = std::make_shared<std::atomic<int>>(0);
    auto destruction_count = std::make_shared<std::atomic<int>>(0);
    auto parser =
        std::make_shared<TestAsyncParser>(started_count, cancellation_count, destruction_count);
    ASSERT_EQ("42", async_calls::stringify_using_parser(parser, 42).get());
    ASSERT_EQ("43", async_calls::stringify_on_tokio(parser, 43).get());
    const auto rust_parser =
        std::dynamic_pointer_cast<async_calls::TestParserImpl>(async_calls::rust_async_parser());
    ASSERT_TRUE(rust_parser);
    ASSERT_EQ(rust_parser->stringify(44).get(), "44");
    ASSERT_EQ(rust_parser->parse("45").get(), 45);
    EXPECT_EXCEPTION(rust_parser->parse("bad input").get(),
                     async_calls::async_parser_error::InvalidInteger);
    ASSERT_EQ(42, async_calls::parse_using_parser(parser, "42").get());
    EXPECT_EXCEPTION(async_calls::parse_using_parser(parser, "not an integer").get(),
                     async_calls::async_parser_error::InvalidInteger);
    for (const auto *input : {"unexpected", "throw", "empty"}) {
        EXPECT_EXCEPTION(async_calls::parse_using_parser(parser, input).get(),
                         async_calls::async_parser_error::Unexpected);
    }
    ASSERT_EQ(async_calls::greet_with(std::make_shared<TestGreeter>()).get(),
              "Hello from a C++ callback!");

    {
        auto waiting = async_calls::wait_using_parser(parser);
        eventually([&] { return started_count->load() != 0; });
        ASSERT_EQ(1, started_count->load());
        parser.reset();
        ASSERT_EQ(0, destruction_count->load());
        waiting.cancel();
        EXPECT_EXCEPTION(waiting.get(), uniffi::AsyncCancelledError);
    }
    eventually([&] { return cancellation_count->load() != 0 && destruction_count->load() != 0; });
    ASSERT_EQ(1, cancellation_count->load());
    ASSERT_EQ(1, destruction_count->load());

    auto continuation_result = std::make_shared<std::promise<std::string>>();
    auto continuation = std::move(futures::say_after(1, "Continuation"))
                            .then([continuation_result](uniffi::FutureResult<std::string> result) {
                                try {
                                    continuation_result->set_value(std::move(result).get());
                                } catch (...) {
                                    continuation_result->set_exception(std::current_exception());
                                }
                            });
    ASSERT_EQ("Hello, Continuation!", continuation_result->get_future().get());
    // Thousands of parked Rust futures must not consume thousands of C++ threads.
    const auto threads_before = thread_count();
    std::vector<uniffi::Future<void>> parked;
    for (int i = 0; i < 2000; ++i) {
        parked.push_back(async_calls::pending());
    }
    const auto parked_deadline = std::chrono::steady_clock::now() + 5s;
    while (async_calls::pending_count() != 2000) {
        ASSERT_TRUE(std::chrono::steady_clock::now() < parked_deadline);
        std::this_thread::yield();
    }
    if (auto threads_parked = thread_count(); threads_before && threads_parked) {
        ASSERT_TRUE(*threads_parked <= *threads_before + 1);
    }
    for (auto &future : parked) {
        future.cancel();
    }
    for (auto &future : parked) {
        EXPECT_EXCEPTION(future.get(), uniffi::AsyncCancelledError);
    }
    ASSERT_EQ(0, async_calls::pending_count());

    // A continuation runs on the shared executor and cannot block it waiting on another call.
    auto callback_done = std::make_shared<std::promise<void>>();
    auto nested = std::move(futures::always_ready()).then([callback_done](auto result) {
        ASSERT_TRUE(uniffi::detail::async_executor().is_worker());
        ASSERT_TRUE(std::move(result).get());
        auto inner = async_calls::pending();
        EXPECT_EXCEPTION(inner.get(), std::logic_error);
        EXPECT_EXCEPTION(inner.wait(), std::logic_error);
        EXPECT_EXCEPTION(inner.wait_for(1ms), std::logic_error);
        ASSERT_EQ(inner.wait_for(0ms), std::future_status::timeout);
        EXPECT_EXCEPTION(uniffi::shutdown_async_executor(), std::logic_error);
        callback_done->set_value();
    });
    ASSERT_EQ(std::future_status::ready, callback_done->get_future().wait_for(5s));

    std::atomic<bool> stop_producers{false};
    std::atomic<int> produced{0};
    std::vector<std::thread> producers;
    for (int i = 0; i < 4; ++i) {
        producers.emplace_back([&] {
            while (!stop_producers.load()) {
                try {
                    ASSERT_TRUE(futures::always_ready().get());
                    ++produced;
                } catch (const uniffi::AsyncExecutorError &) {
                    std::this_thread::yield();
                } catch (const uniffi::AsyncCancelledError &) {
                    // Shutdown also cancels calls accepted immediately before it.
                }
            }
        });
    }
    const auto producers_deadline = std::chrono::steady_clock::now() + 5s;
    while (produced.load() < 20) {
        ASSERT_TRUE(std::chrono::steady_clock::now() < producers_deadline);
        std::this_thread::yield();
    }
    auto late_continuation = futures::always_ready();
    late_continuation.wait();
    auto shutdown_pending = async_calls::pending();
    auto shutdown_foreign = async_calls::wait_using_parser(
        std::make_shared<TestAsyncParser>(started_count, cancellation_count, destruction_count));
    uniffi::shutdown_async_executor();
    stop_producers.store(true);
    for (auto &producer : producers) {
        producer.join();
    }
    EXPECT_EXCEPTION(static_cast<void>(std::move(late_continuation).then([](auto) {})),
                     uniffi::AsyncExecutorError);
    ASSERT_TRUE(late_continuation.get());
    uniffi::shutdown_async_executor(); // Idempotent.
    EXPECT_EXCEPTION(shutdown_pending.get(), uniffi::AsyncCancelledError);
    EXPECT_EXCEPTION(shutdown_foreign.get(), uniffi::AsyncCancelledError);
    EXPECT_EXCEPTION(futures::always_ready().get(), uniffi::AsyncExecutorError);

    return 0;
}
