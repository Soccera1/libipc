/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "libipc/ipc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdlib.h>
static ipc_channel *accepted, *connected;
static int calls;
static ipc_status expected = IPC_OK;
static void result(const ipc_result *r, void *u) {
    assert(r->status == expected);
    if (u) *(ipc_channel **)u = r->channel;
    if (r->message) {
        assert(r->message->size == 5);
        assert(memcmp(r->message->data, "hello", 5) == 0);
        ipc_message_release(r->message);
    }
    ++calls;
}
static void pump(ipc_context *c, int target) {
    int i;
    for (i = 0; calls < target && i < 200; ++i) assert(ipc_run_once(c, 10) >= 0);
    assert(calls == target);
}
int main(int argc, char **argv) {
    ipc_context *c; ipc_listener *l; ipc_operation op; ipc_region *r; ipc_sync *s;
    char name[13]; pid_t pid; int status;
    ipc_options options = {1024, 4096, 2};
    if (argc == 2) {
        ipc_region *other; ipc_sync *notify;
        if (ipc_region_open(argv[1], &other) != IPC_OK || ipc_sync_open(argv[1], &notify) != IPC_OK) return 2;
        memcpy(ipc_region_data(other), "shared", 7);
        if (ipc_sync_post(notify) != IPC_OK) return 3;
        ipc_sync_close(notify); ipc_region_close(other); return 0;
    }
    snprintf(name, sizeof(name), "t%lu", (unsigned long)getpid());
    assert(ipc_context_create(&options, &c) == IPC_OK);
    assert(ipc_listen(c, name, &l) == IPC_OK);
    assert(ipc_accept(l, 1000, result, &accepted, &op) == IPC_OK);
    assert(ipc_connect(c, name, 1000, result, &connected, &op) == IPC_OK);
    assert(calls == 0); pump(c, 2);
    assert(ipc_send(connected, "hello", 5, 1000, result, NULL, &op) == IPC_OK);
    assert(ipc_receive(accepted, 1000, result, NULL, &op) == IPC_OK);
    pump(c, 4);
    assert(ipc_receive(accepted, -1, result, NULL, &op) == IPC_OK);
    assert(ipc_cancel(c, op) == IPC_OK); expected = IPC_CANCELLED; pump(c, 5);
    expected = IPC_TIMEOUT;
    assert(ipc_receive(accepted, 1, result, NULL, &op) == IPC_OK); pump(c, 6);
    expected = IPC_OK;
    assert(ipc_region_create(name, 1024, &r) == IPC_OK);
    assert(ipc_sync_create(name, 0, &s) == IPC_OK);
    assert(ipc_sync_wait(c, s, 2000, result, NULL, &op) == IPC_OK);
    pid = fork(); assert(pid >= 0);
    if (!pid) {
        execl(argv[0], argv[0], name, (char *)NULL);
        _exit(4);
    }
    pump(c, 7); assert(strcmp(ipc_region_data(r), "shared") == 0);
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(ipc_sync_wait(c, s, -1, result, NULL, &op) == IPC_OK);
    assert(ipc_cancel(c, op) == IPC_OK); expected = IPC_CANCELLED; pump(c, 8);
    ipc_sync_close(s); ipc_region_close(r);
    assert(ipc_sync_destroy(name) == IPC_OK); assert(ipc_region_destroy(name) == IPC_OK);
    ipc_context_destroy(c);
    puts("IPC integration passed"); return 0;
}
