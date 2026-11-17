/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "libipc/ipc.h"
#include <windows.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
static int calls;
static ipc_status expected = IPC_OK;
static void completed(const ipc_result *r, void *user) {
    assert(r->status == expected);
    if (user) *(ipc_channel **)user = r->channel;
    if (r->message) {
        assert(r->message->size == 5 && !memcmp(r->message->data, "hello", 5));
        ipc_message_release(r->message);
    }
    ++calls;
}
static void pump(ipc_context *c, int target) {
    int i;
    for (i = 0; i < 200 && calls < target; ++i) assert(ipc_run_once(c, 10) >= 0);
    assert(calls == target);
}
static int peer(const char *name) {
    ipc_context *c; ipc_channel *channel = NULL; ipc_region *r; ipc_sync *s; ipc_operation id;
    assert(ipc_context_create(NULL, &c) == IPC_OK);
    assert(ipc_region_open(name, &r) == IPC_OK);
    assert(ipc_sync_open(name, &s) == IPC_OK);
    assert(ipc_connect(c, name, 1000, completed, &channel, &id) == IPC_OK); pump(c, 1);
    memcpy(ipc_region_data(r), "process", 8); assert(ipc_sync_post(s) == IPC_OK);
    assert(ipc_send(channel, "hello", 5, 1000, completed, NULL, &id) == IPC_OK);
    assert(ipc_receive(channel, 1000, completed, NULL, &id) == IPC_OK); pump(c, 3);
    ipc_sync_close(s); ipc_region_close(r); ipc_context_destroy(c); return 0;
}
static void separate_process(void) {
    ipc_context *c; ipc_channel *channel = NULL; ipc_listener *listener;
    ipc_region *r; ipc_sync *s; ipc_operation id; ipc_options options = {0, 0, 1};
    char executable[MAX_PATH], command[MAX_PATH + 64], name[13];
    STARTUPINFOA startup; PROCESS_INFORMATION process; DWORD code; int base = calls;
    snprintf(name, sizeof(name), "p%lu", (unsigned long)GetCurrentProcessId());
    assert(ipc_context_create(&options, &c) == IPC_OK);
    assert(ipc_region_create(name, 1024, &r) == IPC_OK);
    assert(ipc_sync_create(name, 0, &s) == IPC_OK);
    assert(ipc_listen(c, name, &listener) == IPC_OK);
    assert(ipc_accept(listener, 2000, completed, &channel, &id) == IPC_OK);
    assert(ipc_sync_wait(c, s, 2000, completed, NULL, &id) == IPC_OK);
    assert(GetModuleFileNameA(NULL, executable, sizeof(executable)) > 0);
    snprintf(command, sizeof(command), "\"%s\" --peer %s", executable, name);
    memset(&startup, 0, sizeof(startup)); startup.cb = sizeof(startup);
    assert(CreateProcessA(executable, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process));
    pump(c, base + 2); assert(!strcmp(ipc_region_data(r), "process"));
    assert(ipc_receive(channel, 2000, completed, NULL, &id) == IPC_OK); pump(c, base + 3);
    assert(ipc_send(channel, "hello", 5, 2000, completed, NULL, &id) == IPC_OK); pump(c, base + 4);
    assert(WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0);
    assert(GetExitCodeProcess(process.hProcess, &code) && code == 0);
    CloseHandle(process.hProcess); CloseHandle(process.hThread);
    assert(ipc_sync_destroy(name) == IPC_OK && ipc_region_destroy(name) == IPC_OK);
    ipc_sync_close(s); ipc_region_close(r); ipc_context_destroy(c);
}
int main(int argc, char **argv) {
    ipc_context *c; ipc_listener *l; ipc_channel *client = NULL, *server = NULL;
    ipc_operation op; ipc_region *r, *other; ipc_sync *s;
    ipc_options options = {1024, 4096, 2}; char name[13];
    if (argc == 3 && !strcmp(argv[1], "--peer")) return peer(argv[2]);
    snprintf(name, sizeof(name), "w%lu", (unsigned long)GetCurrentProcessId());
    assert(ipc_context_create(&options, &c) == IPC_OK);
    assert(ipc_listen(c, name, &l) == IPC_OK);
    assert(ipc_accept(l, 1000, completed, &server, &op) == IPC_OK);
    assert(ipc_connect(c, name, 1000, completed, &client, &op) == IPC_OK);
    assert(calls == 0); pump(c, 2);
    assert(ipc_send(client, "hello", 5, 1000, completed, NULL, &op) == IPC_OK);
    assert(ipc_receive(server, 1000, completed, NULL, &op) == IPC_OK); pump(c, 4);
    assert(ipc_receive(server, -1, completed, NULL, &op) == IPC_OK);
    ipc_run_once(c, 0);
    assert(ipc_cancel(c, op) == IPC_OK); expected = IPC_CANCELLED; pump(c, 5);
    expected = IPC_OK;
    assert(ipc_send(client, "hello", 5, 1000, completed, NULL, &op) == IPC_OK);
    assert(ipc_receive(server, 1000, completed, NULL, &op) == IPC_OK); pump(c, 7);
    assert(ipc_region_create(name, 1024, &r) == IPC_OK);
    assert(ipc_region_open(name, &other) == IPC_OK);
    assert(ipc_region_size(other) == 1024);
    memcpy(ipc_region_data(r), "shared", 7);
    assert(!memcmp(ipc_region_data(other), "shared", 7));
    assert(ipc_sync_create(name, 0, &s) == IPC_OK);
    assert(ipc_sync_wait(c, s, 1000, completed, NULL, &op) == IPC_OK);
    assert(ipc_sync_post(s) == IPC_OK); pump(c, 8);
    expected = IPC_TIMEOUT;
    assert(ipc_receive(server, 1, completed, NULL, &op) == IPC_OK); pump(c, 9);
    expected = IPC_CANCELLED;
    assert(ipc_sync_wait(c, s, -1, completed, NULL, &op) == IPC_OK);
    ipc_sync_close(s); ipc_region_close(other); ipc_region_close(r);
    ipc_context_destroy(c); assert(calls == 10);
    expected = IPC_OK; separate_process();
    puts("Windows IOCP integration passed"); return 0;
}
