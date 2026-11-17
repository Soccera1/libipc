/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200112L
#define _XOPEN_SOURCE 600
#include <libipc/ipc.h>
#include "check.h"
#include <sys/types.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
static int child(const char *name) {
    ipc_context *context; ipc_listener *listener; ipc_region *region; ipc_sync *sync;
    CHECK(ipc_context_create(NULL, &context) == IPC_OK);
    CHECK(ipc_listen(context, name, &listener) == IPC_OK);
    CHECK(ipc_region_create(name, 1024, &region) == IPC_OK);
    CHECK(ipc_sync_create(name, 1, &sync) == IPC_OK);
    CHECK(ipc_sync_try_wait(sync) == IPC_OK);
    memcpy(ipc_region_data(region), "before-crash", 13);
    CHECK(write(STDOUT_FILENO, "R", 1) == 1);
    for (;;) pause();
}
int main(int argc, char **argv) {
    char name[13], path[104], ready; int fds[2], status, signalled;
    pid_t pid; struct pollfd event;
    ipc_context *context; ipc_listener *listener; ipc_region *region, *missing; ipc_sync *sync;
    if (argc == 3 && !strcmp(argv[1], "--child")) return child(argv[2]);
    snprintf(name, sizeof(name), "x%lu", (unsigned long)getpid());
    CHECK(pipe(fds) == 0); pid = fork(); CHECK(pid >= 0);
    if (!pid) {
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(2);
        close(fds[1]); execl(argv[0], argv[0], "--child", name, (char *)NULL); _exit(3);
    }
    close(fds[1]); event.fd = fds[0]; event.events = POLLIN; event.revents = 0;
    signalled = poll(&event, 1, 5000) > 0 && read(fds[0], &ready, 1) == 1 && ready == 'R';
    CHECK(kill(pid, SIGKILL) == 0); CHECK(waitpid(pid, &status, 0) == pid);
    close(fds[0]); CHECK(signalled && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    CHECK(ipc_context_create(NULL, &context) == IPC_OK);
    /* A crash leaves a stale socket; binding must not silently unlink it. */
    CHECK(ipc_listen(context, name, &listener) == IPC_SYSTEM);
    CHECK(ipc_region_open(name, &region) == IPC_OK);
    CHECK(!memcmp(ipc_region_data(region), "before-crash", 13));
    CHECK(ipc_sync_open(name, &sync) == IPC_OK);
    CHECK(ipc_sync_try_wait(sync) == IPC_BUSY);
    CHECK(ipc_region_destroy(name) == IPC_OK);
    CHECK(ipc_sync_destroy(name) == IPC_OK);
    CHECK(ipc_region_open(name, &missing) == IPC_SYSTEM);
    /* Unlinked handles remain usable until explicitly closed. */
    CHECK(!memcmp(ipc_region_data(region), "before-crash", 13));
    CHECK(ipc_sync_post(sync) == IPC_OK && ipc_sync_try_wait(sync) == IPC_OK);
    ipc_region_close(region); ipc_sync_close(sync);
    snprintf(path, sizeof(path), "/tmp/libipc-%lu/%s", (unsigned long)geteuid(), name);
    CHECK(unlink(path) == 0);
    CHECK(ipc_listen(context, name, &listener) == IPC_OK);
    ipc_context_destroy(context);
    puts("POSIX crash cleanup passed"); return 0;
}
