// Copyright 2026 LiveKit, Inc.
// SPDX-License-Identifier: Apache-2.0
// Test-only exports installed into an isolated rust-sdks snapshot, never into shipped SDKs.
#[uniffi::export(async_runtime = "tokio")]
pub async fn async_probe_roundtrip(value: u64) -> u64 {
    tokio::time::sleep(std::time::Duration::from_millis(1)).await;
    value
}

#[uniffi::export]
pub async fn async_probe_pending() {
    std::future::pending::<()>().await;
}
