#include "arithmetic.hpp"

namespace executor_tests {

uniffi::detail::AsyncExecutor &executor_from_peer() {
    return uniffi::detail::async_executor();
}

} // namespace executor_tests
