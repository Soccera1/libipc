/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "libipc/ipc.h"
#include "check.h"
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define PROCESS_ID GetCurrentProcessId()
#else
#include <unistd.h>
#include <sys/resource.h>
#define PROCESS_ID getpid()
#endif
typedef struct capture {
    unsigned calls;
    ipc_status status;
    ipc_channel *channel;
    unsigned char bytes[1024];
    size_t size;
} capture;
static void complete(const ipc_result *r, void *user) {
    capture *v = user;
    ++v->calls; v->status = r->status; v->channel = r->channel;
    if (r->message) {
        CHECK(r->message->size <= sizeof(v->bytes));
        v->size = r->message->size;
        memcpy(v->bytes, r->message->data, v->size);
        ipc_message_release(r->message);
    }
}
static void pump(ipc_context *c, capture *v, unsigned target) {
    int i;
    for (i = 0; v->calls < target && i < 500; ++i) CHECK(ipc_run_once(c, 10) >= 0);
    CHECK(v->calls == target);
}
static void connected_pair(ipc_context *c, const char *name, ipc_channel **a, ipc_channel **b) {
    ipc_listener *listener; ipc_operation id; capture accepted = {0}, connected = {0};
    CHECK(ipc_listen(c, name, &listener) == IPC_OK);
    CHECK(ipc_accept(listener, 1000, complete, &accepted, &id) == IPC_OK);
    CHECK(ipc_connect(c, name, 1000, complete, &connected, &id) == IPC_OK);
    pump(c, &accepted, 1); pump(c, &connected, 1);
    CHECK(accepted.status == IPC_OK && connected.status == IPC_OK);
    *a = accepted.channel; *b = connected.channel;
    ipc_listener_close(listener);
}
static void framing_limits(const char *name) {
    ipc_context *c; ipc_channel *a, *b; ipc_operation id;
    ipc_options options = {16, 20, 0};
    capture sent = {0}, received = {0}; unsigned char source[16];
    memset(source, 42, sizeof(source));
    CHECK(ipc_context_create(&options, &c) == IPC_OK);
    connected_pair(c, name, &a, &b);
    CHECK(ipc_send(a, source, 17, -1, complete, &sent, &id) == IPC_LIMIT);
    CHECK(ipc_send(a, source, 16, 1000, complete, &sent, &id) == IPC_OK);
    memset(source, 0, sizeof(source));
    CHECK(ipc_send(a, NULL, 0, -1, complete, &sent, &id) == IPC_LIMIT);
    CHECK(ipc_receive(b, 1000, complete, &received, &id) == IPC_OK);
    pump(c, &received, 1); pump(c, &sent, 1);
    CHECK(received.status == IPC_OK && received.size == 16 && received.bytes[0] == 42);
    CHECK(ipc_send(a, NULL, 0, 1000, complete, &sent, &id) == IPC_OK);
    CHECK(ipc_receive(b, 1000, complete, &received, &id) == IPC_OK);
    pump(c, &received, 2); pump(c, &sent, 2);
    CHECK(received.status == IPC_OK && received.size == 0);
    CHECK(ipc_receive(b, -1, complete, &received, &id) == IPC_OK);
    ipc_channel_close(a); pump(c, &received, 3); CHECK(received.status == IPC_CLOSED);
    ipc_context_destroy(c);
}
static void cancellation_and_pool(const char *name) {
    ipc_options options = {0, 0, 2}; ipc_context *c; ipc_sync *s;
    ipc_operation id, ids[12]; capture v = {0}, blocked[12] = {{0}}, ready = {0}; unsigned i;
    char ready_name[32]; ipc_sync *ready_sync;
    CHECK(ipc_context_create(&options, &c) == IPC_OK);
    CHECK(ipc_sync_create(name, 0, &s) == IPC_OK);
    for (i = 0; i < 100; ++i) {
        CHECK(ipc_sync_wait(c, s, 1000, complete, &v, &id) == IPC_OK);
        CHECK(ipc_sync_post(s) == IPC_OK);
        CHECK(ipc_cancel(c, id) == IPC_OK);
        pump(c, &v, i + 1); CHECK(v.status == IPC_CANCELLED);
        CHECK(ipc_sync_try_wait(s) == IPC_OK); CHECK(ipc_sync_try_wait(s) == IPC_BUSY);
    }
    for (i = 0; i < 12; ++i) CHECK(ipc_sync_wait(c, s, -1, complete, &blocked[i], &ids[i]) == IPC_OK);
    snprintf(ready_name, sizeof(ready_name), "%sr", name);
    CHECK(ipc_sync_create(ready_name, 1, &ready_sync) == IPC_OK);
    CHECK(ipc_sync_wait(c, ready_sync, 1000, complete, &ready, &id) == IPC_OK);
    pump(c, &ready, 1); CHECK(ready.status == IPC_OK);
    CHECK(ipc_sync_destroy(ready_name) == IPC_OK); ipc_sync_close(ready_sync);
    for (i = 0; i < 12; ++i) CHECK(ipc_cancel(c, ids[i]) == IPC_OK);
    for (i = 0; i < 12; ++i) { pump(c, &blocked[i], 1); CHECK(blocked[i].status == IPC_CANCELLED); }
    CHECK(ipc_sync_wait(c, s, 1, complete, &v, &id) == IPC_OK);
    pump(c, &v, 101); CHECK(v.status == IPC_TIMEOUT);
    CHECK(ipc_sync_wait(c, s, -1, complete, &v, &id) == IPC_OK);
    CHECK(ipc_sync_destroy(name) == IPC_OK);
    ipc_sync_close(s); ipc_context_destroy(c);
    CHECK(v.calls == 102 && v.status == IPC_CANCELLED);
}
static void duplex_ordering(const char *name) {
    enum { COUNT = 100 };
    ipc_context *c; ipc_channel *a, *b; ipc_operation id; unsigned i;
    capture *sent_a = calloc(COUNT, sizeof(capture)), *sent_b = calloc(COUNT, sizeof(capture));
    capture *recv_a = calloc(COUNT, sizeof(capture)), *recv_b = calloc(COUNT, sizeof(capture));
    char bytes[16], expected[16];
    CHECK(sent_a && sent_b && recv_a && recv_b);
    CHECK(ipc_context_create(NULL, &c) == IPC_OK); connected_pair(c, name, &a, &b);
    for (i = 0; i < COUNT; ++i) {
        snprintf(bytes, sizeof(bytes), "A%u", i);
        CHECK(ipc_send(a, bytes, strlen(bytes), 2000, complete, &sent_a[i], &id) == IPC_OK);
        snprintf(bytes, sizeof(bytes), "B%u", i);
        CHECK(ipc_send(b, bytes, strlen(bytes), 2000, complete, &sent_b[i], &id) == IPC_OK);
        CHECK(ipc_receive(a, 2000, complete, &recv_a[i], &id) == IPC_OK);
        CHECK(ipc_receive(b, 2000, complete, &recv_b[i], &id) == IPC_OK);
    }
    for (i = 0; i < COUNT; ++i) {
        pump(c, &sent_a[i], 1); pump(c, &sent_b[i], 1); pump(c, &recv_a[i], 1); pump(c, &recv_b[i], 1);
        CHECK(sent_a[i].status == IPC_OK && sent_b[i].status == IPC_OK);
        CHECK(recv_a[i].status == IPC_OK && recv_b[i].status == IPC_OK);
        snprintf(expected, sizeof(expected), "B%u", i);
        CHECK(recv_a[i].size == strlen(expected) && !memcmp(recv_a[i].bytes, expected, strlen(expected)));
        snprintf(expected, sizeof(expected), "A%u", i);
        CHECK(recv_b[i].size == strlen(expected) && !memcmp(recv_b[i].bytes, expected, strlen(expected)));
    }
    ipc_context_destroy(c); free(sent_a); free(sent_b); free(recv_a); free(recv_b);
}
static void fairness_under_new_waits(const char *name) {
    ipc_options options = {0, 0, 1}; ipc_context *context;
    ipc_sync *old_sync, *flood_sync; ipc_operation id;
    capture ready = {0}, cancelled = {0}; char flood_name[32]; unsigned i, submissions = 0;
    snprintf(flood_name, sizeof(flood_name), "%sf", name);
    CHECK(ipc_context_create(&options, &context) == IPC_OK);
    CHECK(ipc_sync_create(name, 0, &old_sync) == IPC_OK);
    CHECK(ipc_sync_create(flood_name, 0, &flood_sync) == IPC_OK);
    CHECK(ipc_sync_wait(context, old_sync, -1, complete, &ready, &id) == IPC_OK);
    for (i = 0; i < 200 && !ready.calls; ++i) {
        CHECK(ipc_sync_wait(context, flood_sync, -1, complete, &cancelled, &id) == IPC_OK);
        CHECK(ipc_sync_wait(context, flood_sync, -1, complete, &cancelled, &id) == IPC_OK);
        submissions += 2;
        if (i == 10) CHECK(ipc_sync_post(old_sync) == IPC_OK);
        CHECK(ipc_run_once(context, 1) >= 0);
    }
    CHECK(ready.calls == 1 && ready.status == IPC_OK);
    CHECK(ipc_sync_destroy(name) == IPC_OK && ipc_sync_destroy(flood_name) == IPC_OK);
    ipc_sync_close(old_sync); ipc_sync_close(flood_sync); ipc_context_destroy(context);
    CHECK(cancelled.calls == submissions && cancelled.status == IPC_CANCELLED);
}
static void lifecycle(const char *name) {
    ipc_context *c; ipc_listener *l; capture v = {0}; ipc_operation id;
    CHECK(ipc_context_create(NULL, &c) == IPC_OK);
    CHECK(ipc_listen(c, name, &l) == IPC_OK);
    CHECK(ipc_accept(l, -1, complete, &v, &id) == IPC_OK);
    ipc_listener_close(l); pump(c, &v, 1); CHECK(v.status == IPC_CLOSED);
    CHECK(ipc_listen(c, name, &l) == IPC_OK);
    CHECK(ipc_accept(l, -1, complete, &v, &id) == IPC_OK);
    ipc_context_destroy(c); CHECK(v.calls == 2 && v.status == IPC_CANCELLED);
}
typedef struct shutdown_state { ipc_context *context; ipc_listener *listener; unsigned calls; } shutdown_state;
static void during_shutdown(const ipc_result *r, void *user) {
    shutdown_state *s = user; ipc_listener *other; ipc_operation id; capture unused = {0};
    CHECK(r->status == IPC_CANCELLED);
    CHECK(ipc_run_once(s->context, 0) == -1);
    CHECK(ipc_listen(s->context, "shutdown", &other) == IPC_INVALID);
    CHECK(ipc_accept(s->listener, -1, complete, &unused, &id) == IPC_INVALID);
    CHECK(ipc_connect(s->context, "shutdown", -1, complete, &unused, &id) == IPC_INVALID);
    ++s->calls;
}
static void invalid_submission(const char *name) {
    ipc_context *c; ipc_listener *listener; ipc_operation id; capture accepted = {0}, unused = {0};
    shutdown_state shutdown;
    CHECK(ipc_context_create(NULL, &c) == IPC_OK);
    CHECK(ipc_listen(c, name, &listener) == IPC_OK);
    CHECK(ipc_accept(listener, 1, complete, &accepted, &id) == IPC_OK);
    CHECK(ipc_connect(c, name, -2, complete, &unused, &id) == IPC_INVALID);
    CHECK(ipc_connect(c, name, 1000, NULL, &unused, &id) == IPC_INVALID);
    CHECK(ipc_connect(c, name, 1000, complete, &unused, NULL) == IPC_INVALID);
    pump(c, &accepted, 1); CHECK(accepted.status == IPC_TIMEOUT && unused.calls == 0);
    shutdown.context = c; shutdown.listener = listener; shutdown.calls = 0;
    CHECK(ipc_accept(listener, -1, during_shutdown, &shutdown, &id) == IPC_OK);
    ipc_context_destroy(c); CHECK(shutdown.calls == 1);
}
int main(void) {
    char name[13];
#ifndef _WIN32
    struct rlimit limit;
    CHECK(getrlimit(RLIMIT_NOFILE, &limit) == 0);
    if (limit.rlim_cur > 64) { limit.rlim_cur = 64; CHECK(setrlimit(RLIMIT_NOFILE, &limit) == 0); }
#endif
    snprintf(name, sizeof(name), "b%lu", (unsigned long)PROCESS_ID);
    framing_limits(name); cancellation_and_pool(name); lifecycle(name); duplex_ordering(name); invalid_submission(name);
    fairness_under_new_waits(name);
    puts("IPC behavior tests passed"); return 0;
}
