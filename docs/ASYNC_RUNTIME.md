# C++17 async runtime

## Execution and ownership

All generated components linked into one C++ executable or shared library use the same inline
runtime instance. This is not a singleton shared across independently built DLLs/DSOs.

One lazy, joinable worker executes short control tasks. Each Rust future has at most one queued or
running driver and one outstanding UniFFI poll. Wakes enqueue work only when progress is possible;
there is no timer scanning, blocking waiter, or C++ thread per operation. A driver yields after 64
actions so repeated immediate wakes do not monopolize the queue.

The executor's registry owns an active Rust state until completion frees its handle. This also lets
poll callbacks reuse the state's address instead of allocating a new userdata holder for every poll.
Future destruction and explicit cancellation request work through that state; neither calls
`rust_future_cancel` directly from an application thread. Rust's terminal call status decides whether
completion or cancellation won the race, so a successful result is not discarded or leaked.

A C++ foreign operation returns `ForeignFuture<T>` containing a start function. That function arranges
nonblocking application work and returns a cancellation action. The bridge keeps the implementation
alive until success, failure, or cancellation wins. Application captures are released outside internal
state locks. A `FutureContinuation` owns its cancellation action; retain it until delivery. Its return
from `then` is marked `nodiscard` to catch accidental immediate cancellation.

Rust still owns its I/O runtimes. This executor supplies the missing C++ control/runtime boundary;
it does not replace Tokio or implement another network stack.

## Limits and contract

- Continuations, foreign method startup, cancellation actions, and Rust polls must return promptly.
  Blocking I/O or CPU-heavy work belongs on an application's existing workers / async I/O service,
  or the Rust runtime's appropriate task pool. A single long-running poll cannot be preempted.
- Shutdown is final for the binding image, not an operation to perform on every room disconnect.
  Stop producers, call `shutdown_async_dispatcher()` on an application thread, and release generated
  objects before unloading Rust. Restartable runtime instances are not implemented.
- A foreign cancellation action must release its operation's resources and prevent unbounded work.
  The bridge cannot forcibly join application-owned workers. A nonreturning callback can stall
  executor shutdown.
- Queue/state memory grows with requested concurrency and captured payloads. Driver wakeups are
  coalesced; this is not a fixed-capacity or hard-real-time executor. Allocation exhaustion is fatal
  where abandoning accepted cleanup would leak a handle or violate the FFI protocol.
- Async trait interfaces retain the recovered `Foo` / `FooForeign` / `FooImpl` split. A Rust-backed
  async trait object returned as the common base needs a cast to `FooImpl` before invoking methods.
  The generic fixtures verify foreign implementations crossing into Rust, not that reverse API.

## Verification

The 22 fixture tests cover async functions, methods, constructors, typed errors, cancellation,
continuations, foreign implementations, and executor shutdown. The async API client test exercises
the upstream example with application-owned workers for blocking work.

Downstream SDK integration tests belong in their consuming repositories. This repository keeps
only the fixtures needed to verify generated bindings and the async runtime protocol.
