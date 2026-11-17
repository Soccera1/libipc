// SPDX-License-Identifier: LGPL-2.1-or-later
#![allow(non_camel_case_types)]
use std::ffi::{c_char, c_int, c_void};
pub type ipc_status = i32;
pub type ipc_operation = u64;
pub const IPC_OK: ipc_status = 0;
pub const IPC_INVALID: ipc_status = 1;
pub const IPC_SYSTEM: ipc_status = 2;
pub const IPC_BUSY: ipc_status = 3;
pub const IPC_CLOSED: ipc_status = 4;
pub const IPC_CANCELLED: ipc_status = 5;
pub const IPC_TIMEOUT: ipc_status = 6;
pub const IPC_LIMIT: ipc_status = 7;
pub const IPC_DISABLED: ipc_status = 8;
pub enum ipc_context {}
pub enum ipc_channel {}
pub enum ipc_listener {}
pub enum ipc_region {}
pub enum ipc_sync {}
#[repr(C)]
pub struct ipc_options {
    pub max_message_bytes: usize,
    pub max_queued_bytes: usize,
    pub sync_workers: u32,
}
#[repr(C)]
pub struct ipc_message {
    pub data: *mut c_void,
    pub size: usize,
}
#[repr(C)]
pub struct ipc_result {
    pub operation: u64,
    pub status: c_int,
    pub channel: *mut ipc_channel,
    pub message: *mut ipc_message,
}
pub type Callback = unsafe extern "C" fn(*const ipc_result, *mut c_void);
extern "C" {
    pub fn ipc_context_create(o: *const ipc_options, c: *mut *mut ipc_context) -> c_int;
    pub fn ipc_context_destroy(c: *mut ipc_context);
    pub fn ipc_run_once(c: *mut ipc_context, timeout: c_int) -> c_int;
    pub fn ipc_run(c: *mut ipc_context);
    pub fn ipc_stop(c: *mut ipc_context);
    pub fn ipc_wakeup(c: *mut ipc_context);
    pub fn ipc_build_info() -> *const c_char;
    pub fn ipc_cancel(c: *mut ipc_context, id: u64) -> c_int;
    pub fn ipc_listen(c: *mut ipc_context, name: *const c_char, l: *mut *mut ipc_listener)
        -> c_int;
    pub fn ipc_listener_close(l: *mut ipc_listener);
    pub fn ipc_channel_close(c: *mut ipc_channel);
    pub fn ipc_accept(
        l: *mut ipc_listener,
        t: c_int,
        cb: Callback,
        u: *mut c_void,
        id: *mut u64,
    ) -> c_int;
    pub fn ipc_connect(
        c: *mut ipc_context,
        n: *const c_char,
        t: c_int,
        cb: Callback,
        u: *mut c_void,
        id: *mut u64,
    ) -> c_int;
    pub fn ipc_send(
        c: *mut ipc_channel,
        p: *const c_void,
        n: usize,
        t: c_int,
        cb: Callback,
        u: *mut c_void,
        id: *mut u64,
    ) -> c_int;
    pub fn ipc_receive(
        c: *mut ipc_channel,
        t: c_int,
        cb: Callback,
        u: *mut c_void,
        id: *mut u64,
    ) -> c_int;
    pub fn ipc_message_release(m: *mut ipc_message);
    pub fn ipc_region_create(n: *const c_char, size: usize, r: *mut *mut ipc_region) -> c_int;
    pub fn ipc_region_open(n: *const c_char, r: *mut *mut ipc_region) -> c_int;
    pub fn ipc_region_data(r: *mut ipc_region) -> *mut c_void;
    pub fn ipc_region_size(r: *const ipc_region) -> usize;
    pub fn ipc_region_close(r: *mut ipc_region);
    pub fn ipc_region_destroy(n: *const c_char) -> c_int;
    pub fn ipc_sync_create(n: *const c_char, initial: u32, s: *mut *mut ipc_sync) -> c_int;
    pub fn ipc_sync_open(n: *const c_char, s: *mut *mut ipc_sync) -> c_int;
    pub fn ipc_sync_close(s: *mut ipc_sync);
    pub fn ipc_sync_post(s: *mut ipc_sync) -> c_int;
    pub fn ipc_sync_try_wait(s: *mut ipc_sync) -> c_int;
    pub fn ipc_sync_destroy(n: *const c_char) -> c_int;
    pub fn ipc_sync_wait(
        c: *mut ipc_context,
        s: *mut ipc_sync,
        t: c_int,
        cb: Callback,
        u: *mut c_void,
        id: *mut u64,
    ) -> c_int;
}
