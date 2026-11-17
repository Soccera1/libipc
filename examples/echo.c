/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <libipc/ipc.h>
#include <stdio.h>
#include <string.h>
typedef struct app {
    ipc_context *context;
    ipc_channel *channel;
    const char *text;
    int server, failed;
} app;
static void fail(app *a, ipc_status status) {
    fprintf(stderr, "IPC operation failed: %d\n", (int)status);
    a->failed = 1; ipc_stop(a->context);
}
static void finished(const ipc_result *r, void *user) {
    app *a = user;
    if (r->status != IPC_OK) fail(a, r->status);
    else ipc_stop(a->context);
}
static void received(const ipc_result *r, void *user) {
    app *a = user; ipc_operation id;
    if (r->status != IPC_OK) { fail(a, r->status); return; }
    if (a->server) {
        ipc_status s = ipc_send(a->channel, r->message->data, r->message->size, 5000, finished, a, &id);
        if (s != IPC_OK) fail(a, s);
    } else {
        fwrite(r->message->data, 1, r->message->size, stdout); putchar('\n'); ipc_stop(a->context);
    }
    ipc_message_release(r->message);
}
static void sent(const ipc_result *r, void *user) {
    app *a = user; ipc_operation id; ipc_status s;
    if (r->status != IPC_OK) { fail(a, r->status); return; }
    s = ipc_receive(a->channel, 5000, received, a, &id);
    if (s != IPC_OK) fail(a, s);
}
static void connected(const ipc_result *r, void *user) {
    app *a = user; ipc_operation id; ipc_status s;
    if (r->status != IPC_OK) { fail(a, r->status); return; }
    a->channel = r->channel;
    s = a->server ? ipc_receive(a->channel, 5000, received, a, &id) :
        ipc_send(a->channel, a->text, strlen(a->text), 5000, sent, a, &id);
    if (s != IPC_OK) fail(a, s);
}
int main(int argc, char **argv) {
    app a = {0}; ipc_operation id; ipc_listener *listener; ipc_status s;
    if (argc < 3 || (strcmp(argv[1], "--server") && strcmp(argv[1], "--client"))) {
        fprintf(stderr, "usage: ipc_echo --server NAME | --client NAME [TEXT]\n"); return 2;
    }
    a.server = !strcmp(argv[1], "--server"); a.text = argc > 3 ? argv[3] : "hello";
    if (ipc_context_create(NULL, &a.context) != IPC_OK) return 1;
    if (a.server) {
        s = ipc_listen(a.context, argv[2], &listener);
        if (s == IPC_OK) {
            puts("ready"); fflush(stdout);
            s = ipc_accept(listener, 10000, connected, &a, &id);
        }
    } else s = ipc_connect(a.context, argv[2], 5000, connected, &a, &id);
    if (s != IPC_OK) fail(&a, s); else ipc_run(a.context);
    ipc_context_destroy(a.context); return a.failed ? 1 : 0;
}
