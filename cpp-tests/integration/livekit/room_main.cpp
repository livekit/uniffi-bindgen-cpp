// Copyright 2026 LiveKit, Inc.
// SPDX-License-Identifier: Apache-2.0
extern "C" bool livekitLocalAsyncProbe();
int main() { return livekitLocalAsyncProbe() ? 0 : 1; }
