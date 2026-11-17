/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef IPC_REACTOR_H
#define IPC_REACTOR_H
#include <poll.h>
#include <stddef.h>
typedef struct ipc_reactor {
    int fd;
    struct pollfd *registered;
    size_t count;
} ipc_reactor;
int ipc_reactor_init(ipc_reactor *);
void ipc_reactor_destroy(ipc_reactor *);
int ipc_reactor_wait(ipc_reactor *, struct pollfd *, size_t, int);
#endif
