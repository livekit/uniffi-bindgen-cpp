// Copyright 2026 LiveKit, Inc.
// SPDX-License-Identifier: Apache-2.0
#include <cassert>
#include <chrono>
#include <iostream>

#include "livekit_ffi.hpp"
#include "livekit_net.hpp"

using namespace std::chrono_literals;
namespace net = livekit_net;

class Http final : public net::HttpClientForeign {
public:
  uniffi::ForeignFuture<net::HttpResponse> request(const net::HttpMethod&, const std::string& url,
                                                   const std::vector<net::Header>&,
                                                   std::optional<std::vector<uint8_t>>) override {
    assert(uniffi::detail::default_async_dispatcher().is_worker());
    return uniffi::ForeignFuture<net::HttpResponse>([this, url](auto success, auto failure) {
      assert(uniffi::detail::default_async_dispatcher().is_worker());
      if (url == "pending") {
        {
          std::lock_guard<std::mutex> lock(mutex);
          pending = std::move(success);
        }
        return uniffi::ForeignFuture<net::HttpResponse>::Cancel([this] {
          assert(uniffi::detail::default_async_dispatcher().is_worker());
          std::lock_guard<std::mutex> lock(mutex);
          pending = {};
          ++cancelled;
        });
      }
      if (url == "error") {
        failure(std::make_exception_ptr(net::transport_error::Closed()));
      } else {
        success(net::HttpResponse{200, {{"x-probe", "ok"}}, {1, 2, 3}});
      }
      return uniffi::ForeignFuture<net::HttpResponse>::Cancel{};
    });
  }

  std::mutex mutex;
  std::function<void(net::HttpResponse)> pending;
  std::atomic<int> cancelled{0};
};

class Connection final : public net::WsConnectionForeign {
public:
  uniffi::ForeignFuture<void> send(const std::vector<uint8_t>& frame) override {
    return uniffi::ForeignFuture<void>([this, frame](auto success, auto) {
      assert(uniffi::detail::default_async_dispatcher().is_worker());
      frame_ = frame;
      success();
      return [] {};
    });
  }
  uniffi::ForeignFuture<std::optional<std::vector<uint8_t>>> recv() override {
    return uniffi::ForeignFuture<std::optional<std::vector<uint8_t>>>([this](auto success, auto) {
      success(std::optional<std::vector<uint8_t>>(frame_));
      return [] {};
    });
  }
  uniffi::ForeignFuture<void> close() override {
    return uniffi::ForeignFuture<void>([](auto success, auto) {
      success();
      return [] {};
    });
  }

private:
  std::vector<uint8_t> frame_;
};

class Ws final : public net::WsClientForeign {
public:
  uniffi::ForeignFuture<net::WsConnectResult> connect(const std::string&, const std::vector<net::Header>&,
                                                      uint64_t) override {
    return uniffi::ForeignFuture<net::WsConnectResult>([](auto success, auto) {
      success(net::WsConnectResult{std::make_shared<Connection>()});
      return [] {};
    });
  }
};

int main() {
  auto http = std::make_shared<Http>();
  net::set_http_client(http);
  net::set_ws_client(std::make_shared<Ws>());
  const auto response = net::self_test_http_get("ok").get();
  assert(response.status == 200 && response.body == std::vector<uint8_t>({1, 2, 3}));
  try {
    net::self_test_http_get("error").get();
    assert(false);
  } catch (const net::transport_error::Closed&) {
  }

  std::vector<uint8_t> bytes(4096, 42);
  assert(net::self_test_ws_echo("mock", bytes).get() == bytes);

  auto pending = net::self_test_http_get("pending");
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(http->mutex);
      if (http->pending) {
        break;
      }
    }
    assert(std::chrono::steady_clock::now() < deadline);
    std::this_thread::yield();
  }
  pending.cancel();
  try {
    pending.get();
    assert(false);
  } catch (const uniffi::AsyncCancelledError&) {
  }

  // Different generated components must share one executor and cancellation model.
  livekit_ffi::log_forward_bootstrap(livekit_ffi::LogForwardFilter::kOff);
  auto log = livekit_ffi::log_forward_receive();
  auto outstanding = net::self_test_http_get("pending");
  uniffi::shutdown_async_dispatcher();
  try {
    log.get();
    assert(false);
  } catch (const uniffi::AsyncCancelledError&) {
  }
  try {
    outstanding.get();
    assert(false);
  } catch (const uniffi::AsyncCancelledError&) {
  }
  assert(http->cancelled.load() >= 1);
  std::cout << "LiveKit core: HTTP, WebSocket, typed errors, callbacks, cancellation, shutdown passed\n";
}
