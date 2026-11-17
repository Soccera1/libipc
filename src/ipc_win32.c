/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "libipc/ipc.h"
#include <windows.h>
#include <sddl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

enum operation_kind { OP_ACCEPT, OP_CONNECT, OP_SEND, OP_RECEIVE, OP_WAIT };
typedef struct win_job { OVERLAPPED overlapped; int reader; void *owner; } win_job;
typedef struct operation operation;
struct ipc_listener {
    ipc_context *context;
    HANDLE reserved;
    char name[256];
    PSECURITY_DESCRIPTOR security;
    int closed;
    struct ipc_listener *next;
};
struct ipc_channel {
    ipc_context *context;
    HANDLE handle;
    unsigned char header[4], *payload;
    size_t head, length, used;
    int reading, ready;
    win_job read_job;
    struct ipc_channel *next;
};
struct ipc_sync { HANDLE handle; unsigned references; };
struct ipc_region { HANDLE handle; unsigned char *mapping; size_t size; };
struct operation {
    ipc_operation id;
    enum operation_kind kind;
    int cancelled, done, pending, working, acquired;
    ULONGLONG deadline;
    ipc_status status;
    ipc_callback callback;
    void *user;
    ipc_listener *listener;
    ipc_channel *channel, *result_channel;
    ipc_sync *sync;
    ipc_message *message;
    HANDLE pipe;
    win_job job;
    char name[256];
    unsigned char *bytes;
    size_t size, offset;
    struct operation *next;
};
struct ipc_context {
    HANDLE port, stop_event, *workers;
    CRITICAL_SECTION mutex;
    CONDITION_VARIABLE condition;
    unsigned worker_count;
    LONG stopped;
    int quitting, driving;
    size_t maximum, capacity, queued, native_pending;
    ipc_operation serial, wait_cursor, wait_limit;
    operation *first, *last;
    ipc_channel *channels;
    ipc_listener *listeners;
};

static int valid_name(const char *s, size_t maximum) {
    size_t i;
    if (!s || !*s || strlen(s) > maximum) return 0;
    for (i = 0; s[i]; ++i) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
    }
    return 1;
}
/* Each named object is restricted to the creating process's user SID. */
static int object_security(const char *name, char kind, char *out, size_t size,
                           PSECURITY_DESCRIPTOR *security) {
    HANDLE token; DWORD needed = 0; TOKEN_USER *user; char *sid = NULL;
    char descriptor[256]; int ok = 0;
    if (!valid_name(name, kind == 'p' ? 40 : 12)) return 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return 0;
    GetTokenInformation(token, TokenUser, NULL, 0, &needed);
    user = malloc(needed);
    if (user && GetTokenInformation(token, TokenUser, user, needed, &needed) &&
        ConvertSidToStringSidA(user->User.Sid, &sid)) {
        int n = snprintf(out, size, kind == 'p' ? "\\\\.\\pipe\\libipc_%s_%c_%s" : "Local\\libipc_%s_%c_%s", sid, kind, name);
        if (n > 0 && (size_t)n < size) {
            snprintf(descriptor, sizeof(descriptor), "D:P(A;;GA;;;%s)", sid);
            ok = ConvertStringSecurityDescriptorToSecurityDescriptorA(descriptor, SDDL_REVISION_1, security, NULL) != 0;
        }
    }
    if (sid) LocalFree(sid);
    free(user); CloseHandle(token); return ok;
}
static int associate(ipc_context *c, HANDLE h) {
    return CreateIoCompletionPort(h, c->port, 0, 1) != NULL;
}
static HANDLE pipe_instance(ipc_listener *l, int first) {
    SECURITY_ATTRIBUTES sa = {sizeof(sa), l->security, FALSE};
    HANDLE h = CreateNamedPipeA(l->name,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, &sa);
    if (h != INVALID_HANDLE_VALUE && !associate(l->context, h)) { CloseHandle(h); return INVALID_HANDLE_VALUE; }
    return h;
}
const char *ipc_build_info(void) { return "libipc 0.1.0; messaging=win32_pipe; shm=win32_mapping; reactor=iocp"; }
void ipc_wakeup(ipc_context *c) { if (c) PostQueuedCompletionStatus(c->port, 0, 0, NULL); }
void ipc_stop(ipc_context *c) { if (c) { InterlockedExchange(&c->stopped, 1); ipc_wakeup(c); } }
static DWORD WINAPI sync_worker(void *arg) {
    ipc_context *c = arg;
    for (;;) {
        operation *o, *chosen = NULL; DWORD result; HANDLE handles[2]; int pass;
        EnterCriticalSection(&c->mutex);
        if (c->quitting) { LeaveCriticalSection(&c->mutex); break; }
        /* Capture a finite sweep so continuous submissions cannot starve old waits. */
        for (pass = 0; pass < 2 && !chosen; ++pass) {
            for (o = c->first; o; o = o->next) {
                if (o->kind == OP_WAIT && o->sync && !o->working && !o->done &&
                    o->id > c->wait_cursor && o->id <= c->wait_limit) {
                    chosen = o; break;
                }
            }
            if (!chosen) { c->wait_cursor = 0; c->wait_limit = c->serial; }
        }
        if (!chosen) {
            SleepConditionVariableCS(&c->condition, &c->mutex, INFINITE);
            LeaveCriticalSection(&c->mutex); continue;
        }
        o = chosen; o->working = 1; c->wait_cursor = o->id;
        handles[0] = c->stop_event; handles[1] = o->sync->handle;
        LeaveCriticalSection(&c->mutex);
        result = WaitForMultipleObjects(2, handles, FALSE, 1);
        EnterCriticalSection(&c->mutex);
        o->working = 0;
        o->acquired = result == WAIT_OBJECT_0 + 1;
        if (o->cancelled || c->quitting) { o->status = IPC_CANCELLED; o->done = 1; }
        else if (o->deadline && GetTickCount64() >= o->deadline) { o->status = IPC_TIMEOUT; o->done = 1; }
        else if (o->acquired) { o->status = IPC_OK; o->done = 1; }
        else if (result == WAIT_FAILED) { o->status = IPC_SYSTEM; o->done = 1; }
        if (o->acquired && o->status != IPC_OK) { ReleaseSemaphore(o->sync->handle, 1, NULL); o->acquired = 0; }
        if (o->done) ipc_wakeup(c);
        WakeAllConditionVariable(&c->condition);
        LeaveCriticalSection(&c->mutex);
    }
    return 0;
}
ipc_status ipc_context_create(const ipc_options *options, ipc_context **out) {
    ipc_context *c; unsigned i, workers = options ? options->sync_workers : 0;
    if (!out || workers > 64) return IPC_INVALID;
    c = calloc(1, sizeof(*c)); if (!c) return IPC_SYSTEM;
    c->maximum = options && options->max_message_bytes ? options->max_message_bytes : 1024 * 1024;
    c->capacity = options && options->max_queued_bytes ? options->max_queued_bytes : 8 * 1024 * 1024;
    if (c->maximum > UINT32_MAX) { free(c); return IPC_INVALID; }
    InitializeCriticalSection(&c->mutex); InitializeConditionVariable(&c->condition);
    c->port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    c->stop_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    c->workers = calloc(workers ? workers : 1, sizeof(HANDLE));
    if (!c->port || !c->stop_event || !c->workers) {
        if (c->port) CloseHandle(c->port);
        if (c->stop_event) CloseHandle(c->stop_event);
        DeleteCriticalSection(&c->mutex); free(c->workers); free(c); return IPC_SYSTEM;
    }
    for (i = 0; i < workers; ++i) {
        c->workers[i] = CreateThread(NULL, 0, sync_worker, c, 0, NULL);
        if (!c->workers[i]) { ipc_context_destroy(c); return IPC_SYSTEM; }
        ++c->worker_count;
    }
    *out = c; return IPC_OK;
}
static operation *allocate_operation(ipc_context *c, enum operation_kind kind,
    int timeout, ipc_callback cb, void *user, ipc_operation *id) {
    operation *o;
    if (!c || c->quitting || timeout < -1 || !cb || !id) return NULL;
    o = calloc(1, sizeof(*o)); if (!o) return NULL;
    o->kind = kind; o->callback = cb; o->user = user; o->pipe = INVALID_HANDLE_VALUE;
    o->deadline = timeout < 0 ? 0 : GetTickCount64() + (ULONGLONG)timeout;
    o->job.owner = o; return o;
}
static void enqueue(ipc_context *c, operation *o, ipc_operation *id) {
    EnterCriticalSection(&c->mutex);
    o->id = ++c->serial;
    if (c->last) c->last->next = o; else c->first = o;
    c->last = o; *id = o->id;
    WakeAllConditionVariable(&c->condition);
    LeaveCriticalSection(&c->mutex);
}
ipc_status ipc_listen(ipc_context *c, const char *name, ipc_listener **out) {
    ipc_listener *l;
    if (!c || c->quitting || !out) return IPC_INVALID;
    l = calloc(1, sizeof(*l)); if (!l) return IPC_SYSTEM;
    l->context = c;
    if (!object_security(name, 'p', l->name, sizeof(l->name), &l->security)) { free(l); return IPC_INVALID; }
    l->reserved = pipe_instance(l, 1);
    if (l->reserved == INVALID_HANDLE_VALUE) { LocalFree(l->security); free(l); return IPC_SYSTEM; }
    l->next = c->listeners; c->listeners = l; *out = l; return IPC_OK;
}
void ipc_listener_close(ipc_listener *l) {
    if (l && !l->closed) {
        l->closed = 1;
        if (l->reserved != INVALID_HANDLE_VALUE) { CloseHandle(l->reserved); l->reserved = INVALID_HANDLE_VALUE; }
    }
}
void ipc_channel_close(ipc_channel *ch) {
    if (ch && ch->handle != INVALID_HANDLE_VALUE) {
        CancelIoEx(ch->handle, NULL); CloseHandle(ch->handle); ch->handle = INVALID_HANDLE_VALUE;
    }
    if (ch && !ch->reading) {
        free(ch->payload); ch->payload = NULL;
        ch->head = ch->used = ch->length = 0; ch->ready = 0;
    }
}
static ipc_channel *channel_new(ipc_context *c, HANDLE h) {
    ipc_channel *ch = calloc(1, sizeof(*ch));
    if (!ch) { CloseHandle(h); return NULL; }
    ch->context = c; ch->handle = h; ch->read_job.reader = 1; ch->read_job.owner = ch;
    ch->next = c->channels; c->channels = ch; return ch;
}
ipc_status ipc_accept(ipc_listener *l, int timeout, ipc_callback cb, void *user, ipc_operation *id) {
    operation *o;
    if (!l || l->closed) return IPC_INVALID;
    o = allocate_operation(l->context, OP_ACCEPT, timeout, cb, user, id);
    if (!o) return IPC_INVALID;
    o->listener = l; enqueue(l->context, o, id); return IPC_OK;
}
ipc_status ipc_connect(ipc_context *c, const char *name, int timeout, ipc_callback cb, void *user, ipc_operation *id) {
    operation *o = allocate_operation(c, OP_CONNECT, timeout, cb, user, id);
    PSECURITY_DESCRIPTOR sd = NULL;
    if (!o) return IPC_INVALID;
    if (!object_security(name, 'p', o->name, sizeof(o->name), &sd)) { free(o); return IPC_INVALID; }
    LocalFree(sd); enqueue(c, o, id); return IPC_OK;
}
ipc_status ipc_send(ipc_channel *ch, const void *data, size_t size, int timeout, ipc_callback cb, void *user, ipc_operation *id) {
    operation *o; ipc_context *c;
    if (!ch || ch->handle == INVALID_HANDLE_VALUE || (size && !data)) return IPC_INVALID;
    c = ch->context;
    if (size > c->maximum || size > SIZE_MAX - 4 || size + 4 > c->capacity - c->queued) return IPC_LIMIT;
    o = allocate_operation(c, OP_SEND, timeout, cb, user, id); if (!o) return IPC_INVALID;
    o->bytes = malloc(size + 4); if (!o->bytes) { free(o); return IPC_SYSTEM; }
    o->bytes[0] = (unsigned char)(size >> 24); o->bytes[1] = (unsigned char)(size >> 16);
    o->bytes[2] = (unsigned char)(size >> 8); o->bytes[3] = (unsigned char)size;
    if (size) memcpy(o->bytes + 4, data, size);
    o->size = size + 4; o->channel = ch; c->queued += o->size;
    enqueue(c, o, id); return IPC_OK;
}
ipc_status ipc_receive(ipc_channel *ch, int timeout, ipc_callback cb, void *user, ipc_operation *id) {
    operation *o;
    if (!ch || ch->handle == INVALID_HANDLE_VALUE) return IPC_INVALID;
    o = allocate_operation(ch->context, OP_RECEIVE, timeout, cb, user, id); if (!o) return IPC_INVALID;
    o->channel = ch; enqueue(ch->context, o, id); return IPC_OK;
}
ipc_status ipc_cancel(ipc_context *c, ipc_operation id) {
    operation *o;
    if (!c) return IPC_INVALID;
    EnterCriticalSection(&c->mutex);
    for (o = c->first; o; o = o->next) if (o->id == id) { o->cancelled = 1; break; }
    LeaveCriticalSection(&c->mutex); return o ? IPC_OK : IPC_INVALID;
}
void ipc_message_release(ipc_message *m) { if (m) { free(m->data); free(m); } }
static void start_read(ipc_channel *ch) {
    DWORD amount, transferred; void *buffer; BOOL ok;
    if (ch->reading || ch->ready || ch->handle == INVALID_HANDLE_VALUE) return;
    if (ch->head < 4) { buffer = ch->header + ch->head; amount = (DWORD)(4 - ch->head); }
    else {
        if (ch->used == ch->length) { ch->ready = 1; return; }
        buffer = ch->payload + ch->used;
        amount = (DWORD)((ch->length - ch->used) > 65536 ? 65536 : ch->length - ch->used);
    }
    memset(&ch->read_job.overlapped, 0, sizeof(OVERLAPPED));
    ok = ReadFile(ch->handle, buffer, amount, &transferred, &ch->read_job.overlapped);
    if (!ok && GetLastError() != ERROR_IO_PENDING) { ipc_channel_close(ch); return; }
    ch->reading = 1; ++ch->context->native_pending;
}
static void process_packet(ipc_context *c, OVERLAPPED *overlapped, DWORD bytes, DWORD error) {
    win_job *job = (win_job *)overlapped;
    --c->native_pending;
    if (job->reader) {
        ipc_channel *ch = job->owner;
        ch->reading = 0;
        if (error || !bytes || ch->handle == INVALID_HANDLE_VALUE) { ipc_channel_close(ch); return; }
        if (ch->head < 4) {
            ch->head += bytes;
            if (ch->head == 4) {
                ch->length = ((uint32_t)ch->header[0] << 24) | ((uint32_t)ch->header[1] << 16) |
                             ((uint32_t)ch->header[2] << 8) | ch->header[3];
                if (ch->length > c->maximum) { ipc_channel_close(ch); return; }
                ch->payload = malloc(ch->length ? ch->length : 1);
                if (!ch->payload) { ipc_channel_close(ch); return; }
                if (!ch->length) ch->ready = 1;
            }
        } else { ch->used += bytes; ch->ready = ch->used == ch->length; }
    } else {
        operation *o = job->owner;
        o->pending = 0;
        if (o->done) return;
        if (error || (o->kind == OP_SEND && !bytes)) {
            o->status = IPC_CLOSED; o->done = 1;
            if (o->channel) ipc_channel_close(o->channel);
        } else if (o->kind == OP_ACCEPT) {
            o->result_channel = channel_new(c, o->pipe); o->pipe = INVALID_HANDLE_VALUE;
            o->status = o->result_channel ? IPC_OK : IPC_SYSTEM; o->done = 1;
        } else { o->offset += bytes; if (o->offset == o->size) o->done = 1; }
    }
}
static int advance(ipc_context *c) {
    operation *o, *previous = NULL; ipc_operation tail; int callbacks = 0;
    EnterCriticalSection(&c->mutex); tail = c->last ? c->last->id : 0;
    o = c->first;
    while (o && o->id <= tail) {
        operation *p; int blocked = 0;
        if (o->working) { previous = o; o = o->next; continue; }
        if (o->cancelled || c->quitting) { o->status = IPC_CANCELLED; o->done = 1; }
        else if (!o->done && o->deadline && GetTickCount64() >= o->deadline) { o->status = IPC_TIMEOUT; o->done = 1; }
        /* Closing native handles while draining a timed-out operation must not
           replace its already selected completion status. */
        else if (!o->done && o->listener && o->listener->closed) { o->status = IPC_CLOSED; o->done = 1; }
        else if (!o->done && o->channel && o->channel->handle == INVALID_HANDLE_VALUE) { o->status = IPC_CLOSED; o->done = 1; }
        if (o->done && o->pending) {
            if (o->kind == OP_SEND) ipc_channel_close(o->channel);
            else if (o->pipe != INVALID_HANDLE_VALUE) { CancelIoEx(o->pipe, NULL); CloseHandle(o->pipe); o->pipe = INVALID_HANDLE_VALUE; }
        }
        for (p = c->first; p != o; p = p->next)
            if (p->kind == o->kind && ((o->channel && p->channel == o->channel) ||
                (o->listener && p->listener == o->listener))) blocked = 1;
        if (!o->done && !o->pending && !blocked) {
            if (o->kind == OP_CONNECT) {
                HANDLE h = CreateFileA(o->name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
                if (h != INVALID_HANDLE_VALUE) {
                    if (!associate(c, h)) { CloseHandle(h); o->status = IPC_SYSTEM; }
                    else { o->result_channel = channel_new(c, h); if (!o->result_channel) o->status = IPC_SYSTEM; }
                    o->done = 1;
                } else if (GetLastError() != ERROR_PIPE_BUSY) { o->status = IPC_SYSTEM; o->done = 1; }
            } else if (o->kind == OP_ACCEPT) {
                BOOL ok; DWORD error;
                o->pipe = o->listener->reserved;
                o->listener->reserved = pipe_instance(o->listener, 0);
                if (o->pipe == INVALID_HANDLE_VALUE || o->listener->reserved == INVALID_HANDLE_VALUE) { o->status = IPC_SYSTEM; o->done = 1; }
                else {
                    memset(&o->job.overlapped, 0, sizeof(OVERLAPPED));
                    ok = ConnectNamedPipe(o->pipe, &o->job.overlapped); error = ok ? 0 : GetLastError();
                    if (!ok && error != ERROR_IO_PENDING && error != ERROR_PIPE_CONNECTED) { o->status = IPC_SYSTEM; o->done = 1; }
                    else {
                        o->pending = 1; ++c->native_pending;
                        if (error == ERROR_PIPE_CONNECTED) PostQueuedCompletionStatus(c->port, 0, 0, &o->job.overlapped);
                    }
                }
            } else if (o->kind == OP_SEND) {
                DWORD bytes, amount = (DWORD)((o->size - o->offset) > 65536 ? 65536 : o->size - o->offset); BOOL ok;
                memset(&o->job.overlapped, 0, sizeof(OVERLAPPED));
                ok = WriteFile(o->channel->handle, o->bytes + o->offset, amount, &bytes, &o->job.overlapped);
                if (!ok && GetLastError() != ERROR_IO_PENDING) { o->status = IPC_CLOSED; o->done = 1; ipc_channel_close(o->channel); }
                else { o->pending = 1; ++c->native_pending; }
            } else if (o->kind == OP_RECEIVE) {
                ipc_channel *ch = o->channel;
                start_read(ch);
                if (ch->ready) {
                    o->message = malloc(sizeof(*o->message));
                    if (!o->message) { o->status = IPC_SYSTEM; o->done = 1; }
                    else {
                        o->message->data = ch->payload; o->message->size = ch->length;
                        ch->payload = NULL; ch->head = ch->used = ch->length = 0; ch->ready = 0; o->done = 1;
                    }
                } else if (ch->handle == INVALID_HANDLE_VALUE) { o->status = IPC_CLOSED; o->done = 1; }
            }
        }
        if (!o->done || o->pending) { previous = o; o = o->next; continue; }
        if (o->kind == OP_SEND && o->offset && o->status != IPC_OK) ipc_channel_close(o->channel);
        if (o->acquired && o->status != IPC_OK) ReleaseSemaphore(o->sync->handle, 1, NULL);
        if (o->pipe != INVALID_HANDLE_VALUE) CloseHandle(o->pipe);
        if (previous) previous->next = o->next; else c->first = o->next;
        if (c->last == o) c->last = previous;
        if (o->kind == OP_SEND) c->queued -= o->size;
        if (o->sync) ipc_sync_close(o->sync);
        {
            ipc_result result; ipc_callback cb = o->callback; void *user = o->user;
            result.operation = o->id; result.status = o->status;
            result.channel = o->result_channel; result.message = o->message;
            if (result.status != IPC_OK && result.channel) { ipc_channel_close(result.channel); result.channel = NULL; }
            free(o->bytes); free(o);
            LeaveCriticalSection(&c->mutex); cb(&result, user); ++callbacks; EnterCriticalSection(&c->mutex);
        }
        o = previous ? previous->next : c->first;
    }
    LeaveCriticalSection(&c->mutex); return callbacks;
}
int ipc_run_once(ipc_context *c, int timeout) {
    int callbacks, i; DWORD wait; operation *o;
    if (!c || timeout < -1 || c->driving) return -1;
    c->driving = 1; callbacks = advance(c);
    wait = callbacks ? 0 : timeout < 0 ? INFINITE : (DWORD)timeout;
    EnterCriticalSection(&c->mutex);
    for (o = c->first; o; o = o->next) {
        if (o->done && !o->pending && !o->working) wait = 0;
        if (o->kind == OP_CONNECT && wait > 1) wait = 1;
        if (!o->working && o->deadline && !o->pending) {
            ULONGLONG now = GetTickCount64(), remaining = now >= o->deadline ? 0 : o->deadline - now;
            if (remaining < wait) wait = (DWORD)remaining;
        }
        if (o->pending && o->deadline && !o->done) {
            ULONGLONG now = GetTickCount64(), remaining = now >= o->deadline ? 0 : o->deadline - now;
            if (remaining < wait) wait = (DWORD)remaining;
        }
    }
    LeaveCriticalSection(&c->mutex);
    for (i = 0; i < 64; ++i) {
        DWORD bytes = 0, error; ULONG_PTR key; OVERLAPPED *overlapped = NULL;
        BOOL ok = GetQueuedCompletionStatus(c->port, &bytes, &key, &overlapped, wait);
        error = ok ? 0 : GetLastError(); wait = 0;
        if (overlapped) process_packet(c, overlapped, bytes, error);
        else if (!ok) { if (error != WAIT_TIMEOUT) callbacks = -1; break; }
    }
    if (callbacks >= 0) callbacks += advance(c);
    c->driving = 0; return callbacks;
}
void ipc_run(ipc_context *c) {
    if (!c) return;
    InterlockedExchange(&c->stopped, 0);
    while (!InterlockedCompareExchange(&c->stopped, 0, 0)) if (ipc_run_once(c, -1) < 0) break;
}
void ipc_context_destroy(ipc_context *c) {
    ipc_channel *ch; ipc_listener *l; unsigned i;
    if (!c || c->driving) return;
    EnterCriticalSection(&c->mutex); c->quitting = 1; WakeAllConditionVariable(&c->condition); LeaveCriticalSection(&c->mutex);
    SetEvent(c->stop_event);
    for (i = 0; i < c->worker_count; ++i) { WaitForSingleObject(c->workers[i], INFINITE); CloseHandle(c->workers[i]); }
    for (ch = c->channels; ch; ch = ch->next) ipc_channel_close(ch);
    for (l = c->listeners; l; l = l->next) ipc_listener_close(l);
    while (c->first || c->native_pending) ipc_run_once(c, 10);
    while ((ch = c->channels)) { c->channels = ch->next; free(ch->payload); free(ch); }
    while ((l = c->listeners)) { c->listeners = l->next; LocalFree(l->security); free(l); }
    CloseHandle(c->port); CloseHandle(c->stop_event); free(c->workers); DeleteCriticalSection(&c->mutex); free(c);
}

#define REGION_HEADER 64
#define REGION_MAGIC UINT64_C(0x4950435245473031)
static ipc_status region_open(const char *name, size_t size, int create, ipc_region **out) {
    char native[256]; PSECURITY_DESCRIPTOR security = NULL; SECURITY_ATTRIBUTES sa;
    ipc_region *r; ULONGLONG total = (ULONGLONG)size + REGION_HEADER; DWORD error;
    if (!out || (create && (!size || size > SIZE_MAX - REGION_HEADER))) return IPC_INVALID;
    if (!object_security(name, 'm', native, sizeof(native), &security)) return IPC_INVALID;
    sa.nLength = sizeof(sa); sa.lpSecurityDescriptor = security; sa.bInheritHandle = FALSE;
    r = calloc(1, sizeof(*r)); if (!r) { LocalFree(security); return IPC_SYSTEM; }
    r->handle = create ? CreateFileMappingA(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
        (DWORD)(total >> 32), (DWORD)total, native) : OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, native);
    error = GetLastError(); LocalFree(security);
    if (!r->handle || (create && error == ERROR_ALREADY_EXISTS)) {
        if (r->handle) CloseHandle(r->handle);
        free(r); return IPC_SYSTEM;
    }
    r->mapping = MapViewOfFile(r->handle, FILE_MAP_ALL_ACCESS, 0, 0, create ? (SIZE_T)total : REGION_HEADER);
    if (!r->mapping) { CloseHandle(r->handle); free(r); return IPC_SYSTEM; }
    if (create) {
        uint64_t metadata[2] = {REGION_MAGIC, (uint64_t)size};
        memcpy(r->mapping, metadata, sizeof(metadata)); r->size = size;
    } else {
        uint64_t metadata[2]; memcpy(metadata, r->mapping, sizeof(metadata));
        UnmapViewOfFile(r->mapping);
        if (metadata[0] != REGION_MAGIC || !metadata[1] || metadata[1] > SIZE_MAX - REGION_HEADER) {
            CloseHandle(r->handle); free(r); return IPC_SYSTEM;
        }
        r->size = (size_t)metadata[1];
        r->mapping = MapViewOfFile(r->handle, FILE_MAP_ALL_ACCESS, 0, 0, r->size + REGION_HEADER);
        if (!r->mapping) { CloseHandle(r->handle); free(r); return IPC_SYSTEM; }
    }
    *out = r; return IPC_OK;
}
ipc_status ipc_region_create(const char *n, size_t size, ipc_region **out) { return region_open(n, size, 1, out); }
ipc_status ipc_region_open(const char *n, ipc_region **out) { return region_open(n, 0, 0, out); }
void *ipc_region_data(ipc_region *r) { return r ? r->mapping + REGION_HEADER : NULL; }
size_t ipc_region_size(const ipc_region *r) { return r ? r->size : 0; }
void ipc_region_close(ipc_region *r) { if (r) { UnmapViewOfFile(r->mapping); CloseHandle(r->handle); free(r); } }
/* Windows removes the name only after the final handle/view is closed. */
ipc_status ipc_region_destroy(const char *n) {
    ipc_region *r; ipc_status s = ipc_region_open(n, &r);
    if (s == IPC_OK) ipc_region_close(r);
    return s;
}
static ipc_status sync_open(const char *name, unsigned initial, int create, ipc_sync **out) {
    char native[256]; PSECURITY_DESCRIPTOR security = NULL; SECURITY_ATTRIBUTES sa;
    ipc_sync *s; DWORD error;
    if (!out || initial > LONG_MAX || !object_security(name, 's', native, sizeof(native), &security)) return IPC_INVALID;
    sa.nLength = sizeof(sa); sa.lpSecurityDescriptor = security; sa.bInheritHandle = FALSE;
    s = malloc(sizeof(*s)); if (!s) { LocalFree(security); return IPC_SYSTEM; }
    s->handle = create ? CreateSemaphoreA(&sa, (LONG)initial, LONG_MAX, native) :
        OpenSemaphoreA(SEMAPHORE_ALL_ACCESS, FALSE, native);
    error = GetLastError(); LocalFree(security);
    if (!s->handle || (create && error == ERROR_ALREADY_EXISTS)) {
        if (s->handle) CloseHandle(s->handle);
        free(s); return IPC_SYSTEM;
    }
    s->references = 1; *out = s; return IPC_OK;
}
ipc_status ipc_sync_create(const char *n, unsigned initial, ipc_sync **out) { return sync_open(n, initial, 1, out); }
ipc_status ipc_sync_open(const char *n, ipc_sync **out) { return sync_open(n, 0, 0, out); }
void ipc_sync_close(ipc_sync *s) { if (s && --s->references == 0) { CloseHandle(s->handle); free(s); } }
ipc_status ipc_sync_destroy(const char *n) { ipc_sync *s; ipc_status result = ipc_sync_open(n, &s); if (result == IPC_OK) ipc_sync_close(s); return result; }
ipc_status ipc_sync_post(ipc_sync *s) { return !s ? IPC_INVALID : ReleaseSemaphore(s->handle, 1, NULL) ? IPC_OK : IPC_SYSTEM; }
ipc_status ipc_sync_try_wait(ipc_sync *s) {
    DWORD result;
    if (!s) return IPC_INVALID;
    result = WaitForSingleObject(s->handle, 0);
    return result == WAIT_OBJECT_0 ? IPC_OK : result == WAIT_TIMEOUT ? IPC_BUSY : IPC_SYSTEM;
}
ipc_status ipc_sync_wait(ipc_context *c, ipc_sync *s, int timeout, ipc_callback cb, void *user, ipc_operation *id) {
    operation *o;
    if (!c || !s) return IPC_INVALID;
    if (!c->worker_count) return IPC_DISABLED;
    o = allocate_operation(c, OP_WAIT, timeout, cb, user, id); if (!o) return IPC_INVALID;
    o->sync = s; ++s->references; enqueue(c, o, id); return IPC_OK;
}
