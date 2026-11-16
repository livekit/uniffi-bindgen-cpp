# Local LiveKit integration probes

These are verification sources, not SDK changes or a new shipped API. Run against isolated snapshots
of the local SDK checkouts. Do not replace their generator pins or edit their submodules in place.

## Core modules

Build `livekit-ffi` from the local standalone `rust-sdks` checkout snapshot with
`--no-default-features --features core-modules`, using a separate Cargo target directory. Populate its
`livekit-protocol/protocol` submodule from that checkout too. Generate with this branch's
`target/debug/uniffi-bindgen-cpp --library <core-library> --out-dir <core-bindings>`.

```sh
cmake -S cpp-tests/integration/livekit -B /tmp/livekit-core-probe \
  -DLIVEKIT_BINDINGS_DIR=<absolute-core-bindings> \
  -DLIVEKIT_FFI_LIBRARY=<absolute-core-library>
cmake --build /tmp/livekit-core-probe -j 6
ctest --test-dir /tmp/livekit-core-probe --output-on-failure
```

`core.cpp` uses the real exported `livekit-net` HTTP and WebSocket self-tests with C++ async host
implementations. It covers records, bytes, optional values, nested object handles, typed errors,
cancellation, and shutdown with calls pending in multiple generated components. No server is needed.
The transports are mocks; this does not verify networking, TLS, or a LiveKit room connection.

## C++ SDK and legacy protobuf

Snapshot the local `client-sdk-cpp` checkout and **its checked-out `client-sdk-rust` submodule**.
A feature branch may require a protobuf schema that the standalone Rust checkout does not have.
Copy the Rust submodules (`livekit-protocol/protocol` and `yuv-sys/libyuv`) and C++ `cpp-tools` as well.
When reusing a build tree with newly archived sources, refresh source timestamps so Cargo and CMake
regenerate the protobufs rather than reusing output from a previous snapshot.

Only inside these disposable snapshots:

1. Copy `room_probe.rs` to `livekit-ffi/src/async_runtime_probe.rs`. Append
   `#[cfg(feature = "room-apis")] mod async_runtime_probe;` to that crate's `src/lib.rs`.
   These exports provide an async timer round trip and a permanently pending call without requiring
   the unmerged room-mode UniFFI PRs or a running LiveKit server.
2. Replace the generation command in `cmake/uniffi_cpp.cmake` with this branch's local generator,
   preserving its `--library` and `--out-dir` arguments.
3. Append these verification targets to the copied SDK's root `CMakeLists.txt`, replacing
   `<probes>` with this directory's absolute path:

   ```cmake
   target_sources(livekit PRIVATE "<probes>/room.cpp")
   add_executable(livekit_local_async_probe "<probes>/room_main.cpp")
   target_link_libraries(livekit_local_async_probe PRIVATE livekit)
   ```

4. Configure a **Debug** SDK build so assertions remain enabled. Build `livekit_local_async_probe`
   and run it. An existing matching WebRTC archive can be reused with `LK_CUSTOM_WEBRTC`.

`room.cpp` runs *inside* the SDK shared library, sharing its generated bindings and executor. It
starts a generated async call, creates a Sox resampler through the actual SDK `FfiClient` protobuf
request path, takes UniFFI co-ownership, releases the legacy handle, processes samples, republishes
the object to legacy FFI, takes ownership again, and releases it. It then checks cancellation and
executor shutdown. The SDK's existing generated build-version adapter is exercised too.

These probes do not modify production SDK source or publish artifacts. Full room/data-stream UniFFI
migration, live-server integration, and platform CI remain separate checks.
