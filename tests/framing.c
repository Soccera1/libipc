/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200112L
#include <libipc/ipc.h>
#include "check.h"
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
typedef HANDLE raw_connection;
#define PROCESS_ID GetCurrentProcessId()
static void write_bytes(raw_connection fd, const void *bytes, size_t size) {
    DWORD written = 0;
    CHECK(WriteFile(fd, bytes, (DWORD)size, &written, NULL));
    CHECK(written == size);
}
static void close_peer(raw_connection fd) { CHECK(CloseHandle(fd)); }
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
typedef int raw_connection;
#define PROCESS_ID getpid()
static void write_bytes(raw_connection fd, const void *bytes, size_t size) {
    CHECK(write(fd, bytes, size) == (ssize_t)size);
}
static void close_peer(raw_connection fd) { CHECK(close(fd) == 0); }
#endif
typedef struct result { ipc_channel *channel; ipc_status status; unsigned calls; } result;
static void done(const ipc_result *r, void *user) {
    result *s = user; s->channel = r->channel; s->status = r->status; ++s->calls;
    if (r->message) {
        CHECK(r->message->size == 5 && !memcmp(r->message->data, "hello", 5));
        ipc_message_release(r->message);
    }
}
static void pump(ipc_context *c, result *s, unsigned count) {
    int i;
    for (i = 0; s->calls < count && i < 200; ++i) CHECK(ipc_run_once(c, 10) >= 0);
    CHECK(s->calls == count);
}
static raw_connection raw_peer(ipc_context *c, const char *name, result *accepted) {
#ifdef _WIN32
    ipc_listener *listener; ipc_operation id; HANDLE token, fd;
    DWORD needed = 0; TOKEN_USER *user; char *sid = NULL; char path[256]; int length;
    unsigned target = accepted->calls + 1;
    CHECK(ipc_listen(c, name, &listener) == IPC_OK);
    CHECK(ipc_accept(listener, 1000, done, accepted, &id) == IPC_OK);
    CHECK(ipc_run_once(c, 0) >= 0);
    CHECK(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
    GetTokenInformation(token, TokenUser, NULL, 0, &needed);
    CHECK(needed > 0);
    user = malloc(needed); CHECK(user);
    CHECK(GetTokenInformation(token, TokenUser, user, needed, &needed));
    CHECK(ConvertSidToStringSidA(user->User.Sid, &sid));
    length = snprintf(path, sizeof(path), "\\\\.\\pipe\\libipc_%s_p_%s", sid, name);
    CHECK(length > 0 && (size_t)length < sizeof(path));
    LocalFree(sid); free(user); CHECK(CloseHandle(token));
    fd = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(fd != INVALID_HANDLE_VALUE);
    pump(c, accepted, target); CHECK(accepted->status == IPC_OK);
    ipc_listener_close(listener); return fd;
#else
    ipc_listener *listener; ipc_operation id; struct sockaddr_un address; int fd;
    unsigned target = accepted->calls + 1;
    CHECK(ipc_listen(c, name, &listener) == IPC_OK);
    CHECK(ipc_accept(listener, 1000, done, accepted, &id) == IPC_OK);
    memset(&address, 0, sizeof(address)); address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "/tmp/libipc-%lu/%s", (unsigned long)geteuid(), name);
    fd = socket(AF_UNIX, SOCK_STREAM, 0); CHECK(fd >= 0);
    CHECK(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    pump(c, accepted, target); CHECK(accepted->status == IPC_OK);
    ipc_listener_close(listener); return fd;
#endif
}
int main(void) {
    ipc_context *c; ipc_operation id; result accepted = {0}, received = {0}, sent = {0};
    char name[40]; raw_connection fd; unsigned char first[2] = {0, 0}, rest[7] = {0, 5, 'h', 'e', 'l', 'l', 'o'};
    unsigned char oversized[4] = {0, 0x20, 0, 0};
    unsigned char partial[6] = {0, 0, 0, 5, 'h', 'e'}; unsigned i; unsigned char *large;
    snprintf(name, sizeof(name), "frame%lu", (unsigned long)PROCESS_ID);
    CHECK(ipc_context_create(NULL, &c) == IPC_OK);
    fd = raw_peer(c, name, &accepted);
    CHECK(ipc_receive(accepted.channel, -1, done, &received, &id) == IPC_OK);
    write_bytes(fd, first, sizeof(first));
    CHECK(ipc_run_once(c, 0) >= 0);
    CHECK(received.calls == 0);
    CHECK(ipc_cancel(c, id) == IPC_OK); pump(c, &received, 1);
    CHECK(received.status == IPC_CANCELLED);
    write_bytes(fd, rest, sizeof(rest));
    CHECK(ipc_receive(accepted.channel, 1000, done, &received, &id) == IPC_OK);
    pump(c, &received, 2); CHECK(received.status == IPC_OK);
    write_bytes(fd, partial, sizeof(partial));
    CHECK(ipc_receive(accepted.channel, -1, done, &received, &id) == IPC_OK);
    for (i = 0; i < 4; ++i) CHECK(ipc_run_once(c, 0) >= 0);
    CHECK(received.calls == 2);
    CHECK(ipc_cancel(c, id) == IPC_OK); pump(c, &received, 3);
    CHECK(received.status == IPC_CANCELLED);
    write_bytes(fd, "llo", 3);
    CHECK(ipc_receive(accepted.channel, 1000, done, &received, &id) == IPC_OK);
    pump(c, &received, 4); CHECK(received.status == IPC_OK);
    write_bytes(fd, oversized, sizeof(oversized));
    CHECK(ipc_receive(accepted.channel, 1000, done, &received, &id) == IPC_OK);
    pump(c, &received, 5); CHECK(received.status == IPC_CLOSED);
    close_peer(fd);
    fd = raw_peer(c, name, &accepted);
    CHECK(ipc_receive(accepted.channel, -1, done, &received, &id) == IPC_OK);
    write_bytes(fd, first, sizeof(first));
    for (i = 0; i < 4; ++i) CHECK(ipc_run_once(c, 0) >= 0);
    CHECK(received.calls == 5);
    close_peer(fd);
    pump(c, &received, 6); CHECK(received.status == IPC_CLOSED);
    fd = raw_peer(c, name, &accepted);
    CHECK(ipc_receive(accepted.channel, -1, done, &received, &id) == IPC_OK);
    write_bytes(fd, partial, sizeof(partial));
    for (i = 0; i < 4; ++i) CHECK(ipc_run_once(c, 0) >= 0);
    CHECK(received.calls == 6);
    close_peer(fd);
    pump(c, &received, 7); CHECK(received.status == IPC_CLOSED);
    fd = raw_peer(c, name, &accepted);
    large = malloc(1024 * 1024); CHECK(large);
    memset(large, 'x', 1024 * 1024);
    CHECK(ipc_send(accepted.channel, large, 1024 * 1024, 200, done, &sent, &id) == IPC_OK);
    free(large);
    for (i = 0; i < 4; ++i) CHECK(ipc_run_once(c, 0) >= 0);
    pump(c, &sent, 1);
    if (sent.status != IPC_TIMEOUT) fprintf(stderr, "Backpressured send completed with status %d\n", (int)sent.status);
    CHECK(sent.status == IPC_TIMEOUT);
    CHECK(ipc_receive(accepted.channel, 1000, done, &sent, &id) == IPC_INVALID);
    CHECK(ipc_run_once(c, 0) >= 0 && sent.calls == 1);
    close_peer(fd);
    ipc_context_destroy(c); puts("Adversarial framing passed"); return 0;
}
