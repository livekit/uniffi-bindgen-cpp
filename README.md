# uniffi-bindgen-cpp - UniFFI C++ bindings/scaffolding generator

Generate [UniFFI](https://github.com/mozilla/uniffi-rs) bindings for C++. `uniffi-bindgen-cpp` lives
as a separate project from `uniffi-rs`, as per
[uniffi-rs #1355](https://github.com/mozilla/uniffi-rs/issues/1355).

# How to install

Minimum Rust version required to install `uniffi-bindgen-cpp` is `1.74`.
Newer Rust versions should also work fine.

```bash
cargo install uniffi-bindgen-cpp --git https://github.com/NordSecurity/uniffi-bindgen-cpp --tag v0.8.1+v0.29.4
```

For this UniFFI 0.31 development branch, install directly from the checkout:

```bash
cargo install --path bindgen
```

# How to generate bindings

## Generating with a single UDL file

```bash
uniffi-bindgen-cpp path/to/definitions.udl
```

## Generating using a library file

```bash
uniffi-bindgen-cpp --library your_rust_library.so --out-dir output_directory
```

Regardless of the generation method, these files are produced:

* `path/to/definitions.hpp`
* `path/to/definitions.cpp`
* `path/to/definitions_scaffolding.hpp`

# How to integrate bindings

To integrate the bindings into your projects, simply add the generated bindings files to your project.
C++17 or newer is required to compile the bindings.

# Async functions

UniFFI expects the foreign bindings to provide the scheduler for Rust futures. C++17 provides
futures for waiting on results, so the generated bindings supply scheduling through `AsyncExecutor`.
See [why C++ needs an executor](docs/ASYNC_RUNTIME.md#overview).

Rust async functions, constructors, and methods return a move-only `uniffi::Future<T>`.
The future supports `get()`, `wait()`, `wait_for()`, `cancel()`, and a consuming `then()` operation.
Destroying an incomplete future requests cancellation. Cancellation races are resolved by the
`RustCallStatus` returned by UniFFI completion, so a result that won the race is not discarded.
Keep the `FutureContinuation` returned by `then()` alive until the callback runs; destroying it
requests cancellation.

One shared control executor drives all generated components linked into the same C++ library or
executable. It owns one lazily started, joinable worker thread. Pending calls own state, not threads.
Rust's waker schedules work only when a future can make progress; there is no polling timer or
blocking waiter per future. Each future's poll/cancel/complete/free operations are serialized.

Continuations use that same executor:

```cpp
auto subscription = std::move(service.fetch()).then([](uniffi::FutureResult<Response> result) {
    try {
        consume(std::move(result).get());
    } catch (const std::exception &error) {
        report(error);
    }
});
```

Keep continuations and foreign callback startup/cancellation actions short and nonblocking. Delegate
blocking application work to an existing application worker pool or async I/O service. Waiting on an
incomplete generated future from the control executor throws `std::logic_error` instead of deadlocking.

Stop application producers, then call `uniffi::shutdown_async_executor()` from an application
thread before unloading the Rust library. Shutdown rejects new C++ calls to async Rust exports
with `uniffi::AsyncExecutorError`, cancels active calls, drains queued cleanup and continuations,
and joins the worker. Shutdown is terminal. A nonreturning application callback can prevent
shutdown from completing.

Async callback-interface methods return `uniffi::ForeignFuture<T>` from their C++ implementation.
The start function arranges application work, invokes exactly one supplied completion callback, and
returns a cancellation action:

```cpp
uniffi::ForeignFuture<Response> fetch(Request request) override {
    return uniffi::ForeignFuture<Response>(
        [request = std::move(request)](auto success, auto failure) {
            return http_pool.start(std::move(request), std::move(success), std::move(failure));
        }
    );
}
```

Callback startup, cancellation, result delivery and continuations use the shared control executor.
Completion callbacks may be invoked from any application thread; the bridge defers delivery to Rust.
The generated bridge arbitrates success, failure, and Rust-side drop exactly once. A dropped Rust
future invokes the returned cancellation action and closes the UniFFI completion channel.

See [the async runtime design](docs/ASYNC_RUNTIME.md) for handle ownership, scheduling, and shutdown.

# Configuration options

It's possible to [configure some settings](docs/CONFIGURATION.md) by passing `--config`
argument to the generator.

Note: configuration is not supported when using library mode

```bash
uniffi-bindgen-cpp path/to/definitions.udl --config path/to/uniffi.toml
```

# C++ Scaffolding

It is possible to generate C++ uniffi scaffolding that allows bridging C++ code with any other uniffi supported language (except Rust).
More documentation and limitations can be found in the [scaffolding documentation](docs/SCAFFOLDING.md)

# Versioning

`uniffi-bindgen-cpp` is versioned separately from `uniffi-rs`. UniFFI follows the [SemVer rules from
the Cargo Book](https://doc.rust-lang.org/cargo/reference/resolver.html#semver-compatibility)
which states "Versions are considered compatible if their left-most non-zero
major/minor/patch component is the same". A breaking change is any modification to the C++ bindings
that demands the consumer of the bindings to make corresponding changes to their code to ensure that
the bindings continue to function properly. `uniffi-bindgen-cpp` is young, and it's unclear how stable
the generated bindings are going to be between versions. For this reason, major version is currently
0, and most changes are probably going to bump minor version.

To ensure consistent feature set across external binding generators, `uniffi-bindgen-cpp` targets
a specific `uniffi-rs` version. A consumer using Go bindings (in `uniffi-bindgen-go`) and C#
bindings (in `uniffi-bindgen-cs`) expects the same features to be available across multiple bindings
generators. This means that the consumer should choose external binding generator versions such that
each generator targets the same `uniffi-rs` version.

To simplify this choice `uniffi-bindgen-cpp`, `uniffi-bindgen-cs` and `uniffi-bindgen-go` use tag naming convention
as follows: `vX.Y.Z+vA.B.C`, where `X.Y.Z` is the version of the generator itself, and `A.B.C` is
the version of uniffi-rs it is based on.

The table shows `uniffi-rs` version history for tags that were published before tag naming convention described above was introduced.

| uniffi-bindgen-cpp version               | uniffi-rs version                                |
|------------------------------------------|--------------------------------------------------|
| v0.1.0                                   | v0.25.0                                          |
| v0.2.0                                   | v0.25.0                                          |
| v0.2.1                                   | v0.25.0                                          |
| v0.2.2                                   | v0.25.0                                          |
| v0.7.0                                   | v0.28.3                                          |
| v0.8.0                                   | v0.29.4                                          |

# Documentation

More documentation is available in [docs](docs) directory.

# Contributing

For contribution guidelines, read [CONTRIBUTING.md](CONTRIBUTING.md).
