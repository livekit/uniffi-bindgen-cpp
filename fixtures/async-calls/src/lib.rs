/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

use async_trait::async_trait;
use futures_timer::Delay;
use std::{
    sync::{
        atomic::{AtomicU64, Ordering},
        Arc, Mutex,
    },
    time::Duration,
};
use tokio::sync::{mpsc, Mutex as AsyncMutex};

static PENDING_COUNT: AtomicU64 = AtomicU64::new(0);
static SAMPLE_STREAM_COUNT: AtomicU64 = AtomicU64::new(0);

struct PendingGuard;

impl Drop for PendingGuard {
    fn drop(&mut self) {
        PENDING_COUNT.fetch_sub(1, Ordering::SeqCst);
    }
}

#[uniffi::export]
pub async fn pending() {
    PENDING_COUNT.fetch_add(1, Ordering::SeqCst);
    let _guard = PendingGuard;
    std::future::pending::<()>().await;
}

#[uniffi::export]
pub fn pending_count() -> u64 {
    PENDING_COUNT.load(Ordering::SeqCst)
}

#[derive(uniffi::Object)]
pub struct SampleStream {
    sender: Mutex<Option<mpsc::Sender<u32>>>,
    receiver: AsyncMutex<mpsc::Receiver<u32>>,
}

impl Drop for SampleStream {
    fn drop(&mut self) {
        SAMPLE_STREAM_COUNT.fetch_sub(1, Ordering::SeqCst);
    }
}

#[uniffi::export]
pub fn sample_stream_count() -> u64 {
    SAMPLE_STREAM_COUNT.load(Ordering::SeqCst)
}

#[uniffi::export(async_runtime = "tokio")]
impl SampleStream {
    #[uniffi::constructor]
    pub fn new() -> Arc<Self> {
        let (sender, receiver) = mpsc::channel(2);
        SAMPLE_STREAM_COUNT.fetch_add(1, Ordering::SeqCst);
        Arc::new(Self {
            sender: Mutex::new(Some(sender)),
            receiver: AsyncMutex::new(receiver),
        })
    }

    #[uniffi::constructor]
    pub async fn open() -> Arc<Self> {
        Self::new()
    }

    pub fn push(&self, sample: u32) -> bool {
        self.sender
            .lock()
            .unwrap()
            .as_ref()
            .is_some_and(|sender| sender.try_send(sample).is_ok())
    }

    pub async fn next(&self) -> Option<u32> {
        self.receiver.lock().await.recv().await
    }

    pub fn close_stream(&self) {
        drop(self.sender.lock().unwrap().take());
    }

    // A pending read holds the receiver lock until it receives a sample, closes, or is dropped.
    pub fn is_read_pending(&self) -> bool {
        self.receiver.try_lock().is_err()
    }
}

#[derive(thiserror::Error, uniffi::Error, Debug)]
pub enum MyError {
    #[error("Foo")]
    Foo,
}

#[derive(uniffi::Record)]
pub struct MyRecord {
    pub a: String,
    pub b: u32,
}

#[uniffi::export]
impl MyRecord {
    pub async fn value_after(&self, ms: u16) -> u32 {
        Delay::new(Duration::from_millis(ms.into())).await;
        self.b
    }
}

#[derive(uniffi::Enum)]
pub enum ValidationMode {
    Allow,
    Reject,
}

#[uniffi::export]
impl ValidationMode {
    pub async fn validate(&self) -> Result<(), MyError> {
        match self {
            Self::Allow => Ok(()),
            Self::Reject => Err(MyError::Foo),
        }
    }
}

#[derive(Debug, thiserror::Error, uniffi::Error)]
pub enum AsyncParserError {
    #[error("invalid integer")]
    InvalidInteger,
    #[error("unexpected callback error")]
    Unexpected,
}

impl From<uniffi::UnexpectedUniFFICallbackError> for AsyncParserError {
    fn from(_: uniffi::UnexpectedUniFFICallbackError) -> Self {
        Self::Unexpected
    }
}

#[uniffi::export(with_foreign)]
#[async_trait::async_trait]
pub trait TestParser: Send + Sync {
    async fn stringify(&self, value: i32) -> String;
    async fn parse(&self, value: String) -> Result<i32, AsyncParserError>;
    async fn wait(&self);
}

#[uniffi::export]
pub async fn stringify_using_parser(parser: Arc<dyn TestParser>, value: i32) -> String {
    parser.stringify(value).await
}

#[uniffi::export]
pub async fn parse_using_parser(
    parser: Arc<dyn TestParser>,
    value: String,
) -> Result<i32, AsyncParserError> {
    parser.parse(value).await
}

#[uniffi::export]
pub async fn wait_using_parser(parser: Arc<dyn TestParser>) {
    parser.wait().await
}

#[uniffi::export(async_runtime = "tokio")]
pub async fn stringify_on_tokio(parser: Arc<dyn TestParser>, value: i32) -> String {
    let parent = std::thread::current().id();
    tokio::spawn(async move {
        assert_ne!(std::thread::current().id(), parent);
        parser.stringify(value).await
    })
    .await
    .expect("async parser task failed")
}

struct RustAsyncParser;

#[async_trait::async_trait]
impl TestParser for RustAsyncParser {
    async fn stringify(&self, value: i32) -> String {
        value.to_string()
    }

    async fn parse(&self, value: String) -> Result<i32, AsyncParserError> {
        value.parse().map_err(|_| AsyncParserError::InvalidInteger)
    }

    async fn wait(&self) {}
}

#[uniffi::export]
pub fn rust_async_parser() -> Arc<dyn TestParser> {
    Arc::new(RustAsyncParser)
}

#[uniffi::export(callback_interface)]
#[async_trait::async_trait]
pub trait AsyncGreeter: Send + Sync {
    async fn greeting(&self) -> String;
}

#[uniffi::export]
pub async fn greet_with(greeter: Box<dyn AsyncGreeter>) -> String {
    greeter.greeting().await
}

uniffi::setup_scaffolding!("async_calls");
