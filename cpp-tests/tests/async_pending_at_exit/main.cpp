#include <futures.hpp>

#include <cstdlib>
#include <future>
#include <memory>

namespace {

// Keep both owners reachable at exit without invoking their cancellation destructors.
uniffi::Future<void> *volatile pending_future;
uniffi::FutureContinuation *volatile pending_continuation;

} // namespace

int main() {
    pending_future = new uniffi::Future<void>(futures::pending());
    pending_continuation = new uniffi::FutureContinuation(
        futures::pending().then([](auto) { std::abort(); }));

    auto barrier = std::make_shared<std::promise<void>>();
    auto polled = barrier->get_future();
    if (!uniffi::detail::async_executor().enqueue([barrier] { barrier->set_value(); })) {
        return EXIT_FAILURE;
    }
    polled.get();
    return futures::pending_count() == 2 ? EXIT_SUCCESS : EXIT_FAILURE;
}
