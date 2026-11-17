/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "reactor.h"
#include <stdlib.h>
int ipc_reactor_init(ipc_reactor *r) { r->fd = -1; return 0; }
void ipc_reactor_destroy(ipc_reactor *r) { (void)r; }
int ipc_reactor_wait(ipc_reactor *r, struct pollfd *fds, size_t n, int timeout) {
    struct pollfd *unique;
    size_t i, j, count = 0;
    int result;
    (void)r;
    unique = calloc(n ? n : 1, sizeof(*unique));
    if (!unique) return -1;
    /* poll limits nfds to the descriptor limit on several Unix kernels.
       Requests can greatly outnumber their distinct underlying descriptors. */
    for (i = 0; i < n; ++i) {
        fds[i].revents = 0;
        if (fds[i].fd < 0) continue;
        for (j = 0; j < count && unique[j].fd != fds[i].fd; ++j) {}
        if (j == count) unique[count++].fd = fds[i].fd;
        unique[j].events |= fds[i].events;
    }
    result = poll(unique, (nfds_t)count, timeout);
    if (result >= 0) {
        for (i = 0; i < n; ++i) {
            if (fds[i].fd < 0) continue;
            for (j = 0; j < count; ++j) if (unique[j].fd == fds[i].fd) {
                fds[i].revents = unique[j].revents & (fds[i].events | POLLERR | POLLHUP | POLLNVAL);
                break;
            }
        }
    }
    free(unique); return result;
}
