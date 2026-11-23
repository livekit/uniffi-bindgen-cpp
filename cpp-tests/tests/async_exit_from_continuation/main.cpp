#include "async_runtime_test.hpp"

#include <cstdlib>

int main() {
    auto state = std::make_shared<uniffi::detail::FutureCompletionState<bool>>();
    state->complete(uniffi::FutureResult<bool>(true));
    auto continuation = uniffi::Future<bool>(state, nullptr).then([](auto result) {
        std::exit(std::move(result).get() ? EXIT_SUCCESS : EXIT_FAILURE);
    });

    // Keep the continuation alive until the worker exits the process.
    std::promise<void> running;
    running.get_future().wait();
    return EXIT_FAILURE;
}
