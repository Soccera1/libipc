// SPDX-License-Identifier: LGPL-2.1-or-later
//! Explicitly driven IPC futures. Handles stay on their creating thread.
//!
//! The same API uses the messaging and event backends selected by CMake for the
//! target OS. No async runtime is required. Start the C echo example with
//! `ipc_echo --server demo`, then this client can exchange a framed message:
//!
//! ```no_run
//! use libipc::{Context, Error};
//!
//! fn main() -> Result<(), Error> {
//!     let context = Context::new(0)?;
//!     context.block_on(async {
//!         let connected = context.connect("demo", 5_000)?.await?;
//!         let channel = connected.channel.expect("successful connection");
//!         channel.send(b"hello", 5_000)?.await?;
//!         let reply = channel.receive(5_000)?.await?;
//!         println!("{}", String::from_utf8_lossy(&reply.message.expect("received message")));
//!         Ok(())
//!     })
//! }
//! ```
//!
//! Submission errors are returned immediately; an accepted operation is a future
//! whose result reports completion, cancellation, or deadline expiry. Dropping
//! it requests cancellation. Drive the context to dispatch pending completions;
//! dropping the final context and handle cancels and drains outstanding work.
//!
//! Set [`Options::sync_workers`] above zero to enable [`Synchronizer::wait`] and
//! [`Synchronizer::lock`]. A lock returns a [`Permit`] that posts its semaphore
//! on drop. Mapped memory from [`Region::as_ptr`] requires application-defined
//! synchronization and adherence to Rust's aliasing rules.
use libipc_sys as ffi;
use std::{
    cell::{Cell, RefCell},
    ffi::{c_void, CStr, CString},
    future::Future,
    pin::Pin,
    rc::{Rc, Weak},
    sync::{Arc, Mutex},
    task::{Context as TaskContext, Poll, Wake, Waker},
};
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Error(pub i32);
/// Reports the single messaging, memory, and reactor configuration linked into this crate.
pub fn build_info() -> &'static str {
    unsafe { CStr::from_ptr(ffi::ipc_build_info()) }
        .to_str()
        .expect("native build information is ASCII")
}
impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let description = match self.0 {
            1 => "invalid argument",
            2 => "native system error",
            3 => "operation would block",
            4 => "channel closed",
            5 => "operation cancelled",
            6 => "deadline expired",
            7 => "configured limit exceeded",
            8 => "synchronization workers disabled",
            _ => "unknown error",
        };
        write!(f, "libipc: {description} ({})", self.0)
    }
}
impl std::error::Error for Error {}
/// Zero byte limits select the native library defaults (1 MiB and 8 MiB).
#[derive(Clone, Copy, Debug, Default)]
pub struct Options {
    pub max_message_bytes: usize,
    pub max_queued_bytes: usize,
    pub sync_workers: u32,
}
fn check(s: i32) -> Result<(), Error> {
    if s == 0 {
        Ok(())
    } else {
        Err(Error(s))
    }
}
fn name(s: &str) -> Result<CString, Error> {
    CString::new(s).map_err(|_| Error(1))
}
struct Inner {
    raw: *mut ffi::ipc_context,
    driving: Cell<bool>,
}
impl Drop for Inner {
    fn drop(&mut self) {
        unsafe { ffi::ipc_context_destroy(self.raw) }
    }
}
#[derive(Clone)]
pub struct Context(Rc<Inner>);
pub struct Listener {
    raw: *mut ffi::ipc_listener,
    context: Context,
}
pub struct Channel {
    raw: *mut ffi::ipc_channel,
    context: Context,
}
impl Drop for Listener {
    fn drop(&mut self) {
        unsafe { ffi::ipc_listener_close(self.raw) }
    }
}
impl Drop for Channel {
    fn drop(&mut self) {
        unsafe { ffi::ipc_channel_close(self.raw) }
    }
}
pub struct Completion {
    pub channel: Option<Channel>,
    pub message: Option<Vec<u8>>,
}
struct State {
    result: Option<Result<Completion, Error>>,
    waker: Option<Waker>,
    context: Weak<Inner>,
    refund: Option<Rc<SyncInner>>,
}
impl Drop for State {
    fn drop(&mut self) {
        if matches!(self.result, Some(Ok(_))) {
            if let Some(sync) = &self.refund {
                unsafe {
                    ffi::ipc_sync_post(sync.0);
                }
            }
        }
    }
}
pub struct Operation {
    state: Rc<RefCell<State>>,
    context: Context,
    id: u64,
}
unsafe extern "C" fn completed(r: *const ffi::ipc_result, u: *mut c_void) {
    let state = Rc::from_raw(u as *const RefCell<State>);
    let r = &*r;
    let mut s = state.borrow_mut();
    let channel = if r.channel.is_null() {
        None
    } else {
        s.context.upgrade().map(|inner| Channel {
            raw: r.channel,
            context: Context(inner),
        })
    };
    let message = if r.message.is_null() {
        None
    } else {
        let m = &*r.message;
        let bytes = std::slice::from_raw_parts(m.data as *const u8, m.size).to_vec();
        ffi::ipc_message_release(r.message);
        Some(bytes)
    };
    s.result = Some(check(r.status).map(|_| Completion { channel, message }));
    let waker = s.waker.take();
    drop(s);
    if let Some(w) = waker {
        w.wake();
    }
}
impl Future for Operation {
    type Output = Result<Completion, Error>;
    fn poll(self: Pin<&mut Self>, cx: &mut TaskContext<'_>) -> Poll<Self::Output> {
        let mut s = self.state.borrow_mut();
        if let Some(r) = s.result.take() {
            s.refund = None;
            Poll::Ready(r)
        } else {
            s.waker = Some(cx.waker().clone());
            Poll::Pending
        }
    }
}
impl Drop for Operation {
    fn drop(&mut self) {
        unsafe {
            ffi::ipc_cancel(self.context.0.raw, self.id);
        }
    }
}
impl Context {
    pub fn new(sync_workers: u32) -> Result<Self, Error> {
        Self::with_options(Options {
            sync_workers,
            ..Options::default()
        })
    }
    /// Message limits must fit the wire length and Rust's slice-size bound.
    pub fn with_options(options: Options) -> Result<Self, Error> {
        if options.max_message_bytes > (u32::MAX as usize).min(isize::MAX as usize) {
            return Err(Error(7));
        }
        let options = ffi::ipc_options {
            max_message_bytes: options.max_message_bytes,
            max_queued_bytes: options.max_queued_bytes,
            sync_workers: options.sync_workers,
        };
        let mut raw = std::ptr::null_mut();
        unsafe {
            check(ffi::ipc_context_create(&options, &mut raw))?;
        }
        Ok(Self(Rc::new(Inner {
            raw,
            driving: Cell::new(false),
        })))
    }
    pub fn run_once(&self, timeout_ms: i32) -> Result<usize, Error> {
        if self.0.driving.replace(true) {
            return Err(Error(3));
        }
        let n = unsafe { ffi::ipc_run_once(self.0.raw, timeout_ms) };
        self.0.driving.set(false);
        if n < 0 {
            Err(Error(2))
        } else {
            Ok(n as usize)
        }
    }
    fn submit(
        &self,
        f: impl FnOnce(ffi::Callback, *mut c_void, &mut u64) -> i32,
    ) -> Result<Operation, Error> {
        let state = Rc::new(RefCell::new(State {
            result: None,
            waker: None,
            context: Rc::downgrade(&self.0),
            refund: None,
        }));
        let user = Rc::into_raw(state.clone()) as *mut c_void;
        let mut id = 0;
        let status = f(completed, user, &mut id);
        if status != 0 {
            unsafe {
                drop(Rc::from_raw(user as *const RefCell<State>));
            }
            return Err(Error(status));
        }
        Ok(Operation {
            state,
            context: self.clone(),
            id,
        })
    }
    pub fn connect(&self, endpoint: &str, timeout_ms: i32) -> Result<Operation, Error> {
        let n = name(endpoint)?;
        self.submit(|cb, u, id| unsafe {
            ffi::ipc_connect(self.0.raw, n.as_ptr(), timeout_ms, cb, u, id)
        })
    }
    pub fn listen(&self, endpoint: &str) -> Result<Listener, Error> {
        let n = name(endpoint)?;
        let mut raw = std::ptr::null_mut();
        unsafe {
            check(ffi::ipc_listen(self.0.raw, n.as_ptr(), &mut raw))?;
        }
        Ok(Listener {
            raw,
            context: self.clone(),
        })
    }
    /// Drives this context while polling a future. Other runtimes require an explicit driver.
    pub fn block_on<F: Future>(&self, future: F) -> F::Output {
        struct DriverWake(Mutex<usize>);
        impl Wake for DriverWake {
            fn wake(self: Arc<Self>) {
                self.wake_by_ref();
            }
            fn wake_by_ref(self: &Arc<Self>) {
                let pointer = self.0.lock().unwrap();
                if *pointer != 0 {
                    unsafe {
                        ffi::ipc_wakeup(*pointer as *mut ffi::ipc_context);
                    }
                }
            }
        }
        struct Invalidate(Arc<DriverWake>);
        impl Drop for Invalidate {
            fn drop(&mut self) {
                *self.0 .0.lock().unwrap() = 0;
            }
        }
        let driver = Arc::new(DriverWake(Mutex::new(self.0.raw as usize)));
        let _invalidate = Invalidate(driver.clone());
        let w = Waker::from(driver);
        let mut cx = TaskContext::from_waker(&w);
        let mut future = Box::pin(future);
        loop {
            if let Poll::Ready(value) = future.as_mut().poll(&mut cx) {
                return value;
            }
            self.run_once(-1).expect("IPC driver failed");
        }
    }
}
impl Listener {
    pub fn accept(&self, timeout_ms: i32) -> Result<Operation, Error> {
        self.context
            .submit(|cb, u, id| unsafe { ffi::ipc_accept(self.raw, timeout_ms, cb, u, id) })
    }
}
impl Channel {
    pub fn send(&self, bytes: &[u8], timeout_ms: i32) -> Result<Operation, Error> {
        self.context.submit(|cb, u, id| unsafe {
            ffi::ipc_send(
                self.raw,
                bytes.as_ptr().cast(),
                bytes.len(),
                timeout_ms,
                cb,
                u,
                id,
            )
        })
    }
    pub fn receive(&self, timeout_ms: i32) -> Result<Operation, Error> {
        self.context
            .submit(|cb, u, id| unsafe { ffi::ipc_receive(self.raw, timeout_ms, cb, u, id) })
    }
}
pub struct Region(*mut ffi::ipc_region);
impl Region {
    pub fn create(n: &str, size: usize) -> Result<Self, Error> {
        let n = name(n)?;
        let mut raw = std::ptr::null_mut();
        unsafe {
            check(ffi::ipc_region_create(n.as_ptr(), size, &mut raw))?;
        }
        Ok(Self(raw))
    }
    pub fn open(n: &str) -> Result<Self, Error> {
        let n = name(n)?;
        let mut raw = std::ptr::null_mut();
        unsafe {
            check(ffi::ipc_region_open(n.as_ptr(), &mut raw))?;
        }
        Ok(Self(raw))
    }
    pub fn len(&self) -> usize {
        unsafe { ffi::ipc_region_size(self.0) }
    }
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
    /// The pointer is valid while this mapping lives. Dereferencing it requires
    /// coordinating all processes and avoiding Rust aliasing violations.
    pub fn as_ptr(&self) -> *mut u8 {
        unsafe { ffi::ipc_region_data(self.0).cast() }
    }
    pub fn destroy(n: &str) -> Result<(), Error> {
        let n = name(n)?;
        unsafe { check(ffi::ipc_region_destroy(n.as_ptr())) }
    }
}
impl Drop for Region {
    fn drop(&mut self) {
        unsafe { ffi::ipc_region_close(self.0) }
    }
}
struct SyncInner(*mut ffi::ipc_sync);
impl Drop for SyncInner {
    fn drop(&mut self) {
        unsafe { ffi::ipc_sync_close(self.0) }
    }
}
pub struct Synchronizer {
    raw: Rc<SyncInner>,
    context: Context,
}
pub struct Permit(Rc<SyncInner>);
impl Drop for Permit {
    fn drop(&mut self) {
        unsafe {
            ffi::ipc_sync_post(self.0 .0);
        }
    }
}
pub struct LockOperation {
    operation: Operation,
    sync: Rc<SyncInner>,
}
impl Future for LockOperation {
    type Output = Result<Permit, Error>;
    fn poll(mut self: Pin<&mut Self>, cx: &mut TaskContext<'_>) -> Poll<Self::Output> {
        match Pin::new(&mut self.operation).poll(cx) {
            Poll::Pending => Poll::Pending,
            Poll::Ready(result) => Poll::Ready(result.map(|_| Permit(self.sync.clone()))),
        }
    }
}
impl Synchronizer {
    pub fn create(context: &Context, n: &str, initial: u32) -> Result<Self, Error> {
        let n = name(n)?;
        let mut raw = std::ptr::null_mut();
        unsafe {
            check(ffi::ipc_sync_create(n.as_ptr(), initial, &mut raw))?;
        }
        Ok(Self {
            raw: Rc::new(SyncInner(raw)),
            context: context.clone(),
        })
    }
    pub fn open(context: &Context, n: &str) -> Result<Self, Error> {
        let n = name(n)?;
        let mut raw = std::ptr::null_mut();
        unsafe {
            check(ffi::ipc_sync_open(n.as_ptr(), &mut raw))?;
        }
        Ok(Self {
            raw: Rc::new(SyncInner(raw)),
            context: context.clone(),
        })
    }
    pub fn post(&self) -> Result<(), Error> {
        unsafe { check(ffi::ipc_sync_post(self.raw.0)) }
    }
    pub fn try_wait(&self) -> Result<(), Error> {
        unsafe { check(ffi::ipc_sync_try_wait(self.raw.0)) }
    }
    pub fn wait(&self, timeout_ms: i32) -> Result<Operation, Error> {
        let operation = self.context.submit(|cb, u, id| unsafe {
            ffi::ipc_sync_wait(self.context.0.raw, self.raw.0, timeout_ms, cb, u, id)
        })?;
        operation.state.borrow_mut().refund = Some(self.raw.clone());
        Ok(operation)
    }
    pub fn lock(&self, timeout_ms: i32) -> Result<LockOperation, Error> {
        Ok(LockOperation {
            operation: self.wait(timeout_ms)?,
            sync: self.raw.clone(),
        })
    }
    pub fn destroy(n: &str) -> Result<(), Error> {
        let n = name(n)?;
        unsafe { check(ffi::ipc_sync_destroy(n.as_ptr())) }
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn rejects_unrepresentable_message_limits() {
        assert!(matches!(
            Context::with_options(Options {
                max_message_bytes: isize::MAX as usize + 1,
                ..Options::default()
            }),
            Err(Error(7))
        ));
        #[cfg(target_pointer_width = "64")]
        assert!(matches!(
            Context::with_options(Options {
                max_message_bytes: u32::MAX as usize + 1,
                ..Options::default()
            }),
            Err(Error(7))
        ));
    }
    #[test]
    fn reports_selected_backend() {
        let info = build_info();
        assert!(info.starts_with("libipc "));
        if std::env::var_os("LIBIPC_LIB_DIR").is_some() {
            return;
        }
        let explicit = std::env::var("IPC_EVENT_BACKEND").ok();
        if explicit.is_none() && std::env::var_os("CMAKE_TOOLCHAIN_FILE").is_some() {
            return;
        }
        let default = if cfg!(target_os = "linux") {
            "epoll"
        } else if cfg!(target_os = "windows") {
            "iocp"
        } else if cfg!(any(
            target_os = "macos",
            target_os = "freebsd",
            target_os = "openbsd",
            target_os = "netbsd",
            target_os = "dragonfly"
        )) {
            "kqueue"
        } else {
            "poll"
        };
        assert!(info.contains(&format!(
            "reactor={}",
            explicit.as_deref().unwrap_or(default)
        )));
    }
    #[test]
    fn messaging_and_cancel() {
        let c = Context::with_options(Options {
            max_message_bytes: 16,
            max_queued_bytes: 20,
            sync_workers: 0,
        })
        .unwrap();
        let name = format!("rust{}", std::process::id());
        let l = c.listen(&name).unwrap();
        let accept = l.accept(1000).unwrap();
        let client = c
            .block_on(c.connect(&name, 1000).unwrap())
            .unwrap()
            .channel
            .unwrap();
        let server = c.block_on(accept).unwrap().channel.unwrap();
        assert!(matches!(client.send(&[0; 17], 1000), Err(Error(7))));
        c.block_on(client.send(b"hello", 1000).unwrap()).unwrap();
        assert_eq!(
            c.block_on(server.receive(1000).unwrap())
                .unwrap()
                .message
                .unwrap(),
            b"hello"
        );
        drop(server.receive(-1).unwrap());
        c.run_once(0).unwrap();
    }
    #[test]
    fn external_waker_and_expired_driver() {
        use std::sync::atomic::{AtomicBool, Ordering};
        struct Signal {
            ready: Arc<AtomicBool>,
            started: bool,
        }
        impl Future for Signal {
            type Output = Waker;
            fn poll(mut self: Pin<&mut Self>, cx: &mut TaskContext<'_>) -> Poll<Waker> {
                if self.ready.load(Ordering::Acquire) {
                    return Poll::Ready(cx.waker().clone());
                }
                if !self.started {
                    self.started = true;
                    let ready = self.ready.clone();
                    let wake = cx.waker().clone();
                    std::thread::spawn(move || {
                        std::thread::sleep(std::time::Duration::from_millis(5));
                        ready.store(true, Ordering::Release);
                        wake.wake();
                    });
                }
                Poll::Pending
            }
        }
        let context = Context::new(0).unwrap();
        let wake = context.block_on(Signal {
            ready: Arc::new(AtomicBool::new(false)),
            started: false,
        });
        drop(context);
        wake.wake();
    }
    #[test]
    fn semaphore_drop_refunds_and_guard_releases() {
        let c = Context::new(2).unwrap();
        let name = format!("s{}", std::process::id());
        let s = Synchronizer::create(&c, &name, 1).unwrap();
        let operation = s.wait(1000).unwrap();
        for _ in 0..100 {
            if operation.state.borrow().result.is_some() {
                break;
            }
            c.run_once(10).unwrap();
        }
        assert!(operation.state.borrow().result.is_some());
        drop(operation);
        assert_eq!(s.try_wait(), Ok(()));
        s.post().unwrap();
        let guard = c.block_on(s.lock(1000).unwrap()).unwrap();
        assert_eq!(s.try_wait(), Err(Error(3)));
        drop(guard);
        assert_eq!(s.try_wait(), Ok(()));
        Synchronizer::destroy(&name).unwrap();
    }
    #[test]
    fn interoperates_with_c_process() {
        use std::io::{BufRead, BufReader};
        use std::process::{Command, Stdio};
        let Some(peer) = option_env!("LIBIPC_ECHO_BINARY") else {
            return;
        };
        let name = format!("interop{}", std::process::id());
        let mut child = Command::new(peer)
            .args(["--server", &name])
            .stdout(Stdio::piped())
            .spawn()
            .unwrap();
        let mut line = String::new();
        BufReader::new(child.stdout.take().unwrap())
            .read_line(&mut line)
            .unwrap();
        assert_eq!(line.trim(), "ready");
        let c = Context::new(0).unwrap();
        let channel = c
            .block_on(c.connect(&name, 1000).unwrap())
            .unwrap()
            .channel
            .unwrap();
        c.block_on(channel.send(b"Rust to C", 1000).unwrap())
            .unwrap();
        assert_eq!(
            c.block_on(channel.receive(1000).unwrap())
                .unwrap()
                .message
                .unwrap(),
            b"Rust to C"
        );
        assert!(child.wait().unwrap().success());
    }
}
