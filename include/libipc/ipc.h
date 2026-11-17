/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef LIBIPC_IPC_H
#define LIBIPC_IPC_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ipc_context ipc_context;
typedef struct ipc_listener ipc_listener;
typedef struct ipc_channel ipc_channel;
typedef struct ipc_region ipc_region;
typedef struct ipc_sync ipc_sync;
typedef uint64_t ipc_operation;
/* Fixed width even on vendor compilers configured for small enums. */
typedef int32_t ipc_status;
enum {
    IPC_OK, IPC_INVALID, IPC_SYSTEM, IPC_BUSY, IPC_CLOSED,
    IPC_CANCELLED, IPC_TIMEOUT, IPC_LIMIT, IPC_DISABLED
};
typedef struct ipc_message { void *data; size_t size; } ipc_message;
typedef struct ipc_result {
    ipc_operation operation;
    ipc_status status;
    ipc_channel *channel;
    ipc_message *message;
} ipc_result;
typedef void (*ipc_callback)(const ipc_result *, void *);
typedef struct ipc_options {
    /* Zero selects the default: 1 MiB per message, 8 MiB of queued sends. */
    size_t max_message_bytes;
    size_t max_queued_bytes;
    /* Zero disables asynchronous semaphore waits; at most 64 workers. */
    unsigned sync_workers;
} ipc_options;
/* Contexts and their handles belong to one application thread. Only stop and
   wakeup may be called from another thread. Do not reuse a context after fork.
   NULL options select defaults. On failure, output arguments are unchanged. */
ipc_status ipc_context_create(const ipc_options *, ipc_context **);
/* Dispatch callbacks on the calling thread. run_once returns the number of
   callbacks, or -1 on failure. A timeout of -1 means infinite; other values are
   milliseconds. Do not drive or destroy the context from a callback. */
int ipc_run_once(ipc_context *, int timeout_ms);
void ipc_run(ipc_context *);
/* stop ends run; wakeup interrupts the current native wait without stopping it.
   The context must remain alive until a concurrent call has returned. */
void ipc_stop(ipc_context *);
void ipc_wakeup(ipc_context *);
/* Cancels pending requests and dispatches their callbacks before releasing all
   context-owned listeners and channels. */
void ipc_context_destroy(ipc_context *);
const char *ipc_build_info(void);
/* Endpoint names: 1-40 ASCII letters, digits, hyphens or underscores. */
ipc_status ipc_listen(ipc_context *, const char *name, ipc_listener **);
void ipc_listener_close(ipc_listener *);
/* Submission failures have no callback. Every accepted request has exactly one
   deferred callback, including cancellation, timeout and context destruction.
   Timeouts are measured from submission; -1 means infinite. The result pointer
   is valid only during the callback; user data must remain valid until then.
   Successful accept/connect results contain
   a channel owned by the context. Successful receive transfers a message to the
   application, which must call ipc_message_release. */
ipc_status ipc_accept(ipc_listener *, int, ipc_callback, void *, ipc_operation *);
ipc_status ipc_connect(ipc_context *, const char *, int, ipc_callback, void *, ipc_operation *);
/* Copies the bytes at submission. Successful completion means the complete
   frame reached the local OS, without implying peer receipt or processing. */
ipc_status ipc_send(ipc_channel *, const void *, size_t, int, ipc_callback, void *, ipc_operation *);
ipc_status ipc_receive(ipc_channel *, int, ipc_callback, void *, ipc_operation *);
/* Close releases native resources. Handle storage remains valid until context
   destruction; closing again is harmless. Pending operations complete CLOSED
   unless cancellation or their deadline takes precedence. */
void ipc_channel_close(ipc_channel *);
void ipc_message_release(ipc_message *);
/* Requests cancellation, dispatched by the driver. Cancelling a partial send
   closes its channel; cancelling a receive preserves its partial frame. */
ipc_status ipc_cancel(ipc_context *, ipc_operation);
/* Region and semaphore names have the same alphabet as endpoints, with a
   12-character limit. Creation is exclusive and regions have a fixed size.
   Applications synchronize mapped accesses and define their own byte layout. */
ipc_status ipc_region_create(const char *, size_t, ipc_region **);
ipc_status ipc_region_open(const char *, ipc_region **);
void *ipc_region_data(ipc_region *);
size_t ipc_region_size(const ipc_region *);
void ipc_region_close(ipc_region *);
/* POSIX destroy unlinks the name without invalidating existing handles.
   Windows removes named objects after the final handle/view closes; destroy
   checks the name but cannot unlink it while handles remain. */
ipc_status ipc_region_destroy(const char *);
/* initial=1 for a cooperative nonrecursive lock, initial=0 for counting
   notifications. There is no ownership checking or automatic crash recovery.
   Do not use one sync handle from multiple contexts or application threads. */
ipc_status ipc_sync_create(const char *, unsigned initial, ipc_sync **);
ipc_status ipc_sync_open(const char *, ipc_sync **);
void ipc_sync_close(ipc_sync *);
ipc_status ipc_sync_destroy(const char *);
ipc_status ipc_sync_post(ipc_sync *);
ipc_status ipc_sync_try_wait(ipc_sync *);
/* Requires sync_workers > 0. Retains the native semaphore until completion;
   the caller may close its handle after an accepted wait. */
ipc_status ipc_sync_wait(ipc_context *, ipc_sync *, int, ipc_callback, void *, ipc_operation *);
#ifdef __cplusplus
}
#endif
#endif
