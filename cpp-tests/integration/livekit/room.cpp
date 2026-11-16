// Copyright 2026 LiveKit, Inc.
// SPDX-License-Identifier: Apache-2.0
// Compiled into the isolated C++ SDK so generated calls and the legacy client share the same library.
#include <cassert>
#include <iostream>

#include "ffi.pb.h"
#include "ffi_client.h"
#include "livekit_ffi.hpp"
#include "uniffi_bindgen_adapter.h"

#ifdef _WIN32
#define PROBE_EXPORT __declspec(dllexport)
#else
#define PROBE_EXPORT __attribute__((visibility("default")))
#endif

extern "C" PROBE_EXPORT bool livekitLocalAsyncProbe() {
  auto& client = livekit::FfiClient::instance();
  if (!client.initialize(false)) {
    return false;
  }
  try {
    assert(livekit::uniffiBindgenBuildVersion().has_value());
    auto future = livekit_ffi::async_probe_roundtrip(42);

    // Legacy protobuf creates the object while a generated async call is in flight.
    livekit::proto::FfiRequest request;
    auto* options = request.mutable_new_sox_resampler();
    options->set_input_rate(48000);
    options->set_output_rate(24000);
    options->set_num_channels(1);
    options->set_input_data_type(livekit::proto::SOXR_DATATYPE_INT16I);
    options->set_output_data_type(livekit::proto::SOXR_DATATYPE_INT16I);
    options->set_quality_recipe(livekit::proto::SOXR_QUALITY_HIGH);
    const auto response = client.sendRequest(request);
    assert(response.new_sox_resampler().has_resampler());
    const auto id = response.new_sox_resampler().resampler().handle().id();

    // UniFFI takes co-ownership; releasing the legacy handle must leave the object usable.
    auto resampler = livekit_ffi::SoxResampler::from_ffi_handle_id(id);
    assert(livekit::dropFfiHandle(id));
    auto samples = resampler->push(std::vector<int16_t>(4800, 100));
    auto tail = resampler->flush();
    assert(samples.size() + tail.size() == 2400);
    assert(future.get() == 42);

    // Publish it back to legacy FFI, take ownership again, then release every owner.
    assert(resampler->ffi_handle_id() == id);
    resampler = resampler->take_ffi_handle_id();
    assert(!livekit::dropFfiHandle(id));
    resampler.reset();
    auto pending = livekit_ffi::async_probe_pending();
    uniffi::shutdown_async_dispatcher();
    try {
      pending.get();
      assert(false);
    } catch (const uniffi::AsyncCancelledError&) {
    }
    client.shutdown();
    std::cout << "C++ SDK: generated async + protobuf FFI + bidirectional resampler ownership passed\n";
    return true;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    uniffi::shutdown_async_dispatcher();
    client.shutdown();
    return false;
  }
}
