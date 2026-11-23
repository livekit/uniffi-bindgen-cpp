# Async Runtime

## Overview

UniFFI uses the foreign bindings' runtime to schedule Rust future polls and completions. C++ has
no default async runtime for this work. The generated bindings provide a shared
[`AsyncExecutor`](../bindgen/src/bindings/cpp/templates/async.hpp) for async Rust exports and
C++ callback methods that Rust awaits.

- **One worker thread.** Generated components in each linked executable or shared library share a
  worker and a FIFO queue. [Executor][executor]
- **UniFFI Rust future calls.** The wrapper obtains the future handle and normally calls
  `rust_future_poll` on the calling thread. If that poll reports `READY`, the wrapper calls
  `rust_future_complete` and `rust_future_free` there before returning `Future<T>`. The worker
  polls deferred calls, repolls on `WAKE`, calls `rust_future_cancel` to cancel, and completes and
  frees the remaining handles. [First poll][state-start] · [UniFFI handle rules][rust-handle-rules]
- **Pending calls and continuations.** The worker can run other tasks while a future waits for a
  wake. C++ continuations run on the worker, so keep them short and do blocking waits on
  application threads. [Continuation delivery][completion] · [Wait guard][wait-guard]
- **Async C++ callbacks.** When Rust awaits a C++ `ForeignFuture<T>`, the worker starts the
  callback and delivers its result or handles cancellation. [Callback bridge][callback-results]
- **Shutdown and restart.** Call `shutdown_async_executor()` from an application thread to wait
  for caller-thread first polls, close admission, cancel outstanding work, drain the queue, and
  join the worker. Call `start_async_executor()` to reopen admission.
  [Shutdown][shutdown] · [Restart][restart]

The sequence adapts the [UniFFI Async Overview][upstream-sequence] to the C++ executor.

```mermaid
sequenceDiagram
    participant C as C++ caller
    participant B as Generated C++ binding
    participant E as AsyncExecutor
    participant R as Rust scaffolding

    C->>B: call async function
    B->>R: call scaffolding function
    R-->>B: return RustFuture handle
    B->>B: Register RustFutureState with the executor
    alt Caller-thread first poll admitted
        B->>R: rust_future_poll(handle, continuation, state)
        alt READY on the first poll
            R->>B: continuation(state, READY)
            R-->>B: Return from poll
            B->>R: rust_future_complete(handle, &status)
            R-->>B: Lowered result and RustCallStatus
            B->>B: Check status and lift value
            B->>R: rust_future_free(handle)
            B->>B: Publish result and untrack the call
        else Pending on the first poll
            Note over B,R: WAKE may arrive during or after the poll
            R-->>B: Return from poll
        end
    else Shutdown is waiting for caller-thread polls
        B->>E: Queue the first poll
    end
    B-->>C: Return Future

    opt Call is pending or its first poll was queued
        opt Pending after a caller-thread poll
            R->>B: continuation(state, WAKE)
            B->>E: Schedule the driver after the first poll returns
        end
        loop One outstanding poll until READY
            E->>R: rust_future_poll(handle, continuation, state)
            alt WAKE
                Note over E: The worker can run other tasks while the future waits
                R->>B: continuation(state, WAKE)
                B->>E: Request another poll
            else READY
                R->>B: continuation(state, READY)
                B->>B: Record readiness
            end
        end
        E->>R: rust_future_complete(handle, &status)
        R-->>E: Lowered result and RustCallStatus
        E->>E: Check status and lift value
        E->>R: rust_future_free(handle)
        E->>B: Publish to FutureCompletionState
        E->>E: Untrack the call
    end
    Note over C,E: get() reads the result or throws<br/>then() delivers on the worker
```

## Execution model

The `async_executor()` singleton creates a joinable worker on first use. The worker takes short
tasks from a FIFO queue and sleeps on a condition variable when the queue is empty. The caller
normally runs the first poll before the wrapper returns. Rust code through its first suspension
or completion, including synchronous C++ callbacks it invokes, blocks the calling thread.
Pending Rust futures retain their state and wake callback. Each worker turn runs at most 64
driver actions before yielding to the queue. [Executor][executor] · [First poll][state-start] ·
[Driver][drive]

```mermaid
flowchart LR
    A[Application thread] -->|First poll, complete and free if ready| R[Rust scaffolding]
    A -->|Publish ready result| F[C++ future]
    A -->|Queue first poll during shutdown wait| Q[Control queue]
    Q --> W[One C++ worker]
    W -->|Poll, cancel or finish| R
    R -->|Wake callback| Q
    W -->|Publish result| F
    W -->|Deliver| T[C++ continuation]
```

A move-only `Future<T>` supports `wait()`, `wait_for()`, `get()`, `cancel()`, and a consuming
`then()`. `get()` returns the lifted value or rethrows the stored exception. `then()` transfers
cancellation ownership to its return value, a `FutureContinuation`, and delivers the result on
the worker. Hold that token until delivery or cancel it to request cancellation. Only application
threads may block on an incomplete call. The worker can inspect readiness or consume a ready result.
[Future API][future] · [Completion state][completion] · [Worker wait guard][wait-guard]

Access each `Future<T>` or `FutureContinuation` from one thread at a time. Before calling `get()`
or `then()`, use `cancel_handle()` to obtain a copyable `FutureCancelHandle` for a different thread.
The handle can request cancellation while `get()` waits or after `then()` takes ownership. Its
weak reference keeps cancellation harmless after completion. [Cancel handle][cancel-handle] ·
[Rust cancellation state][state-start]

## C++ calls an async Rust export

The generator uses the same driver for free functions, primary and secondary constructors, and
methods on objects, records, and enums. The wrapper checks the Rust contract version and checksums,
lowers the arguments, and calls the start symbol to obtain a `uint64_t` Rust future handle. The
generator selects poll, cancel, complete, and free symbols from UniFFI metadata for the lowered
result type. It also selects the value and error conversions, including the `void` case. An admitted
object method passes a cloned receiver handle. A record or enum method lowers its receiver by value
inside the start call. Rust owns the lifted receiver across polls and writes `RustCallStatus` through
an out pointer on completion. [Function template][function] · [Object template][object] ·
[Value methods][value-methods] · [Flat enum methods][flat-enum-methods] ·
[Async call macros][macros] · [Initialization][init] · [Rust scaffolding][tokio]

- **Create a handle and poll with a callback and opaque data.** `rust_call_async` gives the acquired
  handle to a scope guard. `start()` registers shared ownership of `RustFutureState` with the
  executor. `run_inline()` normally calls `poll_first_turn()` before the wrapper returns
  `Future<T>`. While shutdown waits for caller-thread polls, `start()` queues the first poll on
  the worker. The poll passes the state's address as callback data, and the callback takes a
  strong reference to the state on entry.
  [Handle guard][handle-guard] · [Start and callback lifetime][state-start] · [Poll][poll]
- **Poll again on `WAKE` and complete on `READY`.** The callback clears `poll_outstanding_` and
  records `poll_requested_` for `WAKE` (`1`) or `ready_` for `READY` (`0`) under the state mutex.
  `start()` sets `poll_outstanding_` for a caller-thread poll. `next_action()` sets it for worker
  polls and admits another poll only after the callback. Rust guarantees one callback per poll,
  which can arrive from another thread.
  [C++ state transitions][continuation] · [Rust poll guarantee][rust-poll]
- **Complete once and stop polling.** `poll_first_turn()` and `next_action()` give `READY` priority
  and set `terminal_` before completion. That flag closes the driver, callback, and cancellation
  paths. Completion checks `RustCallStatus` and lifts the value on success. A ready first poll
  completes on the caller. The worker completes pending and deferred calls.
  [First poll][state-start] · [Action selection][next-action] · [Result conversion][finish] ·
  [Status handling][status]
- **Free the handle as the last FFI operation.** The driver frees the handle, publishes the result,
  then removes the registry entry. Status, lifting, and result-transfer failures are stored as
  exceptions for the caller. The scope guard owns the handle until registration succeeds. It
  frees the handle if registration fails, including when admission closes during handle creation.
  The wrapper arms `Future<T>` cancellation after successful registration.
  [Completion and free][finish] · [Guard and handoff][call-async] · [Admission race][state-start]
- **Serialize optional cancellation with the rest of the lifecycle.** `cancel()` and token
  destruction request work on the driver. It sends at most one `rust_future_cancel`. After the
  driver sets `terminal_`, only completion and free can touch the handle.
  [Action selection][next-action] · [Cancellation FFI call][cancel] ·
  [Rust handle rules][rust-handle-rules]

Wake-ups also work when Rust invokes the callback inline during `poll`. `scheduled_` and `driving_`
coalesce driver requests. The driver rechecks the flags under the mutex before parking, covering a
callback that arrives as the driver is about to yield. FFI calls run with the state mutex released,
so inline callbacks can acquire it and return. [Driver scheduling and parking][drive]

Cancellation with a poll outstanding produces `READY` from the Rust scheduler. If cancellation
arrives before a callback is installed, the worker polls the cancelled handle to get `READY`.
`complete` consumes any result already produced by Rust. If Rust has no result, it reports
`Cancelled` and drops the pending future. The C++ wrapper maps that status to `AsyncCancelledError`
and runs the same cleanup path. This preserves results that win the cancellation race.
[C++ cancel path][cancel] · [Rust scheduler][rust-scheduler] · [Rust completion][rust-complete]

## Rust awaits an async C++ callback

A generated async callback method returns a move-only `ForeignFuture<T>` on the C++ side. Its start
function accepts success and failure callbacks and returns an optional cancellation action. In the
FFI vtable, the method returns `void` and accepts a completion function, its original `uint64_t`
context, and an output slot for the drop callback. The trampoline fills the drop slot, lifts the
arguments, and queues startup. Application code can arrange blocking work on an application
worker and invoke the supplied success or failure callback from any thread.
[C++ callback surface][callback-api] ·
[Vtable trampoline][callback-entry] · [Upstream async API client][api-client]

```mermaid
sequenceDiagram
    participant R as Rust future
    participant B as C++ callback bridge
    participant E as Control worker
    participant A as C++ implementation
    R->>B: Call vtable with completion and context
    B-->>R: Write drop callback and task handle
    B->>E: Queue startup
    E->>A: Start ForeignFuture
    A-->>E: Return cancellation action
    alt Application resolves or fails
        A->>E: Queue completion
    else Rust drops the future
        R->>B: Invoke drop callback
        B->>E: Release handle and queue cancellation
    end
    E->>R: Complete once with original context and status
```

The `finished_` gate in `ForeignFutureTaskState` is shared by success, error, and cancellation.
The first terminal action takes that gate. Success lowers a value into the metadata-defined result
structure. Failure sends a declared UniFFI error or an unexpected exception message, including
exceptions from moving a value into the queue or lowering it. A `void` completion carries just the
status. The completion function consumes Rust's one-shot sender using the original context. These
paths run on the control worker for an active executor, even when an application worker supplies
the result. [Completion lowering and queueing][callback-results] ·
[Terminal gate][foreign-state] · [Rust receiver and completion callback][rust-foreign]

The drop slot owns a separate handle to the C++ task state. The state retains the callback object
through the registered cancel action, and moves that action out of its mutex before running or
releasing it. If cancellation takes the terminal gate, the drop path requests application
cancellation and sends a cancelled completion to release Rust's sender. If completion took the
gate, a later drop just releases the C++ handle.
[Startup and retained callback object][callback-results] ·
[Drop callback and cancellation][foreign-drop] · [UniFFI foreign handle contract][foreign-rules]

## Shutdown

Stop producers before calling `shutdown_async_executor()` from an application thread. The executor
first waits for caller-thread first polls to finish with admission open. Calls that start during
that wait queue their first poll on the worker. A blocked first poll delays shutdown. After the
caller-thread polls finish, the executor closes admission, requests cancellation for registered
calls, drains the queue and in-flight work, and joins the worker. Continuation delivery retains its
own registration until its callback is released. Keep control callbacks short so other calls and
shutdown can make progress.
[Drain and join][shutdown] · [Continuation registration][registration]

Once admission closes, the driver sends cancellation before polling a pending Rust future.
This covers a C++ callback that reports shutdown before the executor reaches the outer Rust
call's registered cancellation. The driver still completes a future that has already signaled
`READY`. [Action selection][next-action]

New C++ calls to async Rust exports receive `AsyncExecutorError` until an application thread calls
`start_async_executor()`. Start waits for any concurrent shutdown to finish joining the worker,
then opens admission. Calling start on a running executor is harmless. Operation IDs continue
increasing across restarts. The callback trampoline checks admission and its own state before it
starts work, so rejected or retired callbacks stay inactive after restart. [Restart][restart] ·
[Callback bridge][callback-results]

On the worker or inside a caller-thread first poll, `shutdown_async_executor()` throws
`std::logic_error` and `start_async_executor()` returns immediately.
[Shutdown guard][shutdown] · [Start guard][restart]

The singleton has process lifetime. Returning from `main` or calling `std::exit` leaves pending
work and its Rust handles to process teardown. Python, Kotlin, and TypeScript UniFFI bindings
likewise rely on their host runtimes for process exit. Use explicit shutdown before unloading
the generated bindings or the Rust library. [Singleton lifetime][executor]

### Rust-owned tasks during shutdown

A task driven by a Rust runtime can still be awaiting a C++ callback when shutdown cancels it or
rejects its startup. A callback that Rust invokes while the executor is stopped is rejected the
same way. The bridge completes the callback with status code 3 and an empty error buffer.
UniFFI lifts that status through the same branch as code 2, creating an
`UnexpectedUniFFICallbackError`. [Callback cancellation status][callback-entry] ·
[UniFFI status handling][rust-foreign-return]

To receive this as an `Err` in Rust, declare the trait method with a `Result<T, E>` return and
implement `From<uniffi::UnexpectedUniFFICallbackError>` for `E`. UniFFI panics for an infallible
method or when conversion to the declared error type fails. [Result handling][rust-foreign-result] ·
[Error conversion][rust-callback-conversion]

[upstream-sequence]: https://mozilla.github.io/uniffi-rs/latest/internals/async-overview.html#rust-async-functions
[rust-handle-rules]: https://mozilla.github.io/uniffi-rs/latest/internals/async-ffi.html#managing-the-rustfuture-handle
[foreign-rules]: https://mozilla.github.io/uniffi-rs/latest/internals/async-ffi.html#managing-the-foreignfuture-handle
[api-client]: https://github.com/mozilla/uniffi-rs/tree/05b45b110f652d58d94c653992aeb3adc3a094f4/examples/async-api-client
[tokio]: https://github.com/mozilla/uniffi-rs/blob/05b45b110f652d58d94c653992aeb3adc3a094f4/uniffi_macros/src/export/scaffolding.rs#L271-L299
[rust-poll]: https://github.com/mozilla/uniffi-rs/blob/05b45b110f652d58d94c653992aeb3adc3a094f4/uniffi_core/src/ffi/rustfuture/mod.rs#L119-L149
[rust-scheduler]: https://github.com/mozilla/uniffi-rs/blob/05b45b110f652d58d94c653992aeb3adc3a094f4/uniffi_core/src/ffi/rustfuture/scheduler.rs#L80-L84
[rust-complete]: https://github.com/mozilla/uniffi-rs/blob/05b45b110f652d58d94c653992aeb3adc3a094f4/uniffi_core/src/ffi/rustfuture/future.rs#L162-L179
[rust-foreign]: https://github.com/mozilla/uniffi-rs/blob/05b45b110f652d58d94c653992aeb3adc3a094f4/uniffi_core/src/ffi/foreignfuture.rs#L67-L99
[rust-foreign-return]: https://github.com/mozilla/uniffi-rs/blob/05b45b110f652d58d94c653992aeb3adc3a094f4/uniffi_core/src/ffi_converter_traits.rs#L357-L395
[rust-foreign-result]: https://github.com/mozilla/uniffi-rs/blob/05b45b110f652d58d94c653992aeb3adc3a094f4/uniffi_core/src/ffi_converter_impls.rs#L516-L540
[rust-callback-conversion]: https://github.com/mozilla/uniffi-rs/blob/05b45b110f652d58d94c653992aeb3adc3a094f4/uniffi_core/src/ffi/callbackinterface.rs#L150-L210
[executor]: ../bindgen/src/bindings/cpp/templates/async.hpp#L20-L205
[drive]: ../bindgen/src/bindings/cpp/templates/async.cpp#L236-L322
[future]: ../bindgen/src/bindings/cpp/templates/async.hpp#L457-L588
[cancel-handle]: ../bindgen/src/bindings/cpp/templates/async.hpp#L435-L453
[completion]: ../bindgen/src/bindings/cpp/templates/async.hpp#L345-L430
[wait-guard]: ../bindgen/src/bindings/cpp/templates/async.hpp#L219-L223
[function]: ../bindgen/src/bindings/cpp/templates/fn_def.cpp#L1-L14
[object]: ../bindgen/src/bindings/cpp/templates/obj.cpp#L32-L72
[value-methods]: ../bindgen/src/bindings/cpp/templates/value_methods.cpp#L1-L14
[flat-enum-methods]: ../bindgen/src/bindings/cpp/templates/flat_enum_methods.cpp#L1-L14
[macros]: ../bindgen/src/bindings/cpp/templates/macros.cpp#L69-L102
[init]: ../bindgen/src/bindings/cpp/templates/wrapper.cpp#L13-L37
[handle-guard]: ../bindgen/src/bindings/cpp/templates/async.cpp#L117-L137
[state-start]: ../bindgen/src/bindings/cpp/templates/async.cpp#L145-L208
[poll]: ../bindgen/src/bindings/cpp/templates/async.cpp#L324-L328
[continuation]: ../bindgen/src/bindings/cpp/templates/async.cpp#L210-L234
[next-action]: ../bindgen/src/bindings/cpp/templates/async.cpp#L253-L275
[finish]: ../bindgen/src/bindings/cpp/templates/async.cpp#L340-L402
[status]: ../bindgen/src/bindings/cpp/templates/wrapper.cpp#L40-L66
[call-async]: ../bindgen/src/bindings/cpp/templates/async.cpp#L410-L445
[cancel]: ../bindgen/src/bindings/cpp/templates/async.cpp#L330-L338
[callback-api]: ../bindgen/src/bindings/cpp/templates/callback.hpp#L6-L37
[callback-entry]: ../bindgen/src/bindings/cpp/templates/callback_iface_tmpl.cpp#L7-L22
[callback-results]: ../bindgen/src/bindings/cpp/templates/callback_iface_tmpl.cpp#L24-L140
[foreign-state]: ../bindgen/src/bindings/cpp/templates/async.cpp#L7-L102
[foreign-drop]: ../bindgen/src/bindings/cpp/templates/async.cpp#L70-L113
[shutdown]: ../bindgen/src/bindings/cpp/templates/async.hpp#L105-L136
[restart]: ../bindgen/src/bindings/cpp/templates/async.hpp#L89-L101
[registration]: ../bindgen/src/bindings/cpp/templates/async.hpp#L335-L343
