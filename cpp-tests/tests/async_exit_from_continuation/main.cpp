#include <futures.hpp>

#include <cstdlib>
#include <future>
#include <utility>

int main() {
    auto continuation = futures::always_ready().then([](auto result) {
        std::exit(std::move(result).get() ? EXIT_SUCCESS : EXIT_FAILURE);
    });

    // Keep the continuation alive until the worker exits the process.
    std::promise<void> running;
    running.get_future().wait();
    return EXIT_FAILURE;
}
