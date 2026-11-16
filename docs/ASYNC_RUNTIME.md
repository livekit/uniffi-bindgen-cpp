# C++17 async runtime

## Branch and migration

`livekit/dev-async-support` starts at `livekit/dev` (`e558880`, UniFFI 0.31.2 / C++17).
The original Rust-to-C++ async commits (`9d86abe`, `c4ad947`) were cherry-picked, then the async
work from stash `3e98bc0` was copied onto that base. All original branches and stashes remain intact.
The two older stashes concern non-async generator work; they were inspected, not applied wholesale.
IDE-only changes from the async stash were left out.

Nothing was pushed, and neither SDK's production generator pin or checked-out sources were changed.

## Review of the recovered design

The original PR allocated a detached thread for every call. The recovered stash had already moved
to a shared dispatcher and introduced callback-based foreign futures and continuations. It was the
better starting point, but needed a consistent ownership and execution model:

- A rejecting custom dispatcher started a second fallback executor. This created two control paths
  and made cleanup depend on executor rejection policy. The implementation now has one owned control
  executor, with no replaceable dispatcher or fallback worker.
- Shutdown required every caller to make every future terminal first. The executor now registers
  active Rust and foreign operations, rejects new work at shutdown, cancels registered operations,
  drains completions and continuations, and joins its worker. Concurrent application callers and
  repeated shutdown calls are covered by tests.
- Foreign method startup could invoke user code inside Rust polling. Startup, cancellation, and
  completion delivery are now deferred onto the same executor. Application completion callbacks can
  come from other threads; they enqueue delivery instead of reentering Rust.
- Continuations required another executor argument and could fall back to inline execution on
  rejection. `then(callback)` now uses the control executor and retains a registration through
  delivery. Attaching a continuation after shutdown throws without consuming the original future.
- Blocking on an incomplete future in an executor callback could deadlock. Blocking waits now throw
  on that thread; zero-duration readiness checks remain allowed. Self-shutdown also throws.
- The executor's work and drain conditions are separate, so a shutdown waiter cannot consume the
  only notification intended for a sleeping worker.

The older experimental branch's saved review also identified cancellation/free races, discarded
lowered results, and leaked foreign oneshot senders. Those fixes are retained: only the driver touches
Rust handles, READY always goes through complete/lift/free, and foreign cancellation sends exactly
one cancelled completion to release the sender even after Rust drops its receiver.

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
  The integration probes verify foreign implementations crossing into Rust, not that reverse API.

## Local verification

Verified on macOS arm64 with Apple Clang 21 and C++17:

- All 22 fixture CTest targets passed.
- The futures suite passed 10 consecutive runs, including simultaneous callers and shutdown,
  cancellation/completion races, pending foreign calls, typed errors, methods and constructors.
- 2,000 simultaneously parked futures left process thread count unchanged: 4 before and 4 during.
- A debug-build batch of 20,000 ready calls measured about 0.06 seconds (roughly 330,000 calls/second)
  on this workstation. This is a local throughput check, not a production benchmark or latency SLA.
- The futures executable passed AddressSanitizer + UndefinedBehaviorSanitizer, and separately
  ThreadSanitizer. The Rust fixture library itself was not sanitizer-instrumented.
- A real `livekit-ffi/core-modules` build generated and compiled all four C++ components. The core
  probe passed HTTP/WebSocket round trips through foreign C++ async interfaces, typed transport
  errors, cancellation, and shutdown across `livekit-net` and `livekit-ffi`.
- A local C++ SDK shared library built with the unpublished generator. Its probe passed a generated
  async timer call alongside the real legacy protobuf client, and transferred a Sox resampler's
  ownership from protobuf FFI to UniFFI and back before cancellation and shutdown.

Inputs were committed snapshots of the standalone `rust-sdks` checkout (`0a0fbe31`),
`client-sdk-cpp` (`158d078`), and the C++ checkout's Rust submodule (`6047bebd`), with their checked-out
native/protocol submodules. The C++ capture branch requires its own Rust schema; substituting the
standalone Rust checkout causes capture protobuf compilation failures unrelated to async support.
Original dirty files and submodule selections were preserved.

The room-mode async exports exist only in the disposable snapshot: current main has not merged the
room-mode data-track/data-stream UniFFI APIs. See [the integration probes](../cpp-tests/integration/livekit/README.md)
for the test sources and reproduction steps. No live server, media traffic, Windows, or Linux runtime
was exercised. Cross-platform CI and long-duration / large-payload measurements remain necessary
before shipping.
