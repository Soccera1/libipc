/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "reactor.h"
#include "ipc_config.h"
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <stdint.h>
#if IPC_REACTOR_EPOLL
#include <sys/epoll.h>
#else
#include <sys/event.h>
#include <time.h>
#endif
int ipc_reactor_init(ipc_reactor *r) {
#if IPC_REACTOR_EPOLL
    r->fd = epoll_create1(EPOLL_CLOEXEC);
#else
    r->fd = kqueue();
    if (r->fd >= 0 && fcntl(r->fd, F_SETFD, FD_CLOEXEC) < 0) {
        close(r->fd); r->fd = -1;
    }
#endif
    r->registered = NULL; r->count = 0;
    return r->fd < 0 ? -1 : 0;
}
void ipc_reactor_destroy(ipc_reactor *r) {
    if (r->fd >= 0) close(r->fd);
    free(r->registered);
}
static int update(ipc_reactor *r, int fd, short before, short after) {
#if IPC_REACTOR_EPOLL
    struct epoll_event event;
    int op = !after ? EPOLL_CTL_DEL : !before ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
    memset(&event, 0, sizeof(event)); event.data.fd = fd;
    if (after & POLLIN) event.events |= EPOLLIN;
    if (after & POLLOUT) event.events |= EPOLLOUT;
    if (epoll_ctl(r->fd, op, fd, &event) == 0) return 0;
    if (!after && (errno == ENOENT || errno == EBADF)) return 0;
    /* A closed descriptor can be reused between turns. */
    if (op == EPOLL_CTL_MOD && errno == ENOENT)
        return epoll_ctl(r->fd, EPOLL_CTL_ADD, fd, &event);
    if (op == EPOLL_CTL_ADD && errno == EEXIST)
        return epoll_ctl(r->fd, EPOLL_CTL_MOD, fd, &event);
    return -1;
#else
    short bits[2] = {POLLIN, POLLOUT}; int filters[2] = {EVFILT_READ, EVFILT_WRITE};
    unsigned i;
    for (i = 0; i < 2; ++i) {
        struct kevent event;
        if (!(before & bits[i]) && !(after & bits[i])) continue;
        EV_SET(&event, fd, filters[i], (after & bits[i]) ? EV_ADD | EV_ENABLE : EV_DELETE, 0, 0, 0);
        if (kevent(r->fd, &event, 1, NULL, 0, NULL) < 0 &&
            !(!(after & bits[i]) && (errno == ENOENT || errno == EBADF))) return -1;
    }
    return 0;
#endif
}
int ipc_reactor_wait(ipc_reactor *r, struct pollfd *fds, size_t n, int timeout) {
    struct pollfd *current; size_t count = 0, i, j; int result;
    if (n > INT_MAX) { errno = EOVERFLOW; return -1; }
    current = calloc(n ? n : 1, sizeof(*current));
    if (!current) return -1;
    for (i = 0; i < n; ++i) {
        fds[i].revents = 0;
        if (fds[i].fd < 0) continue;
        for (j = 0; j < count && current[j].fd != fds[i].fd; ++j) {}
        if (j == count) current[count++].fd = fds[i].fd;
        current[j].events |= fds[i].events;
    }
    for (i = 0; i < r->count; ++i) {
        for (j = 0; j < count && current[j].fd != r->registered[i].fd; ++j) {}
        if (j == count && update(r, r->registered[i].fd, r->registered[i].events, 0)) goto fail;
    }
    for (i = 0; i < count; ++i) {
        short before = 0;
        for (j = 0; j < r->count; ++j)
            if (r->registered[j].fd == current[i].fd) before = r->registered[j].events;
        /* Re-arm even unchanged masks to handle descriptor reuse. */
        if (update(r, current[i].fd, before, current[i].events)) goto fail;
    }
    free(r->registered); r->registered = current; r->count = count;
#if IPC_REACTOR_EPOLL
    {
        struct epoll_event events[64];
        result = epoll_wait(r->fd, events, 64, timeout);
        for (i = 0; result > 0 && i < (size_t)result; ++i)
            for (j = 0; j < n; ++j) if (fds[j].fd == events[i].data.fd) {
                if (events[i].events & EPOLLIN) fds[j].revents |= POLLIN;
                if (events[i].events & EPOLLOUT) fds[j].revents |= POLLOUT;
                if (events[i].events & (EPOLLERR | EPOLLHUP)) fds[j].revents |= POLLERR;
            }
    }
#else
    {
        struct kevent events[64]; struct timespec ts;
        ts.tv_sec = timeout < 0 ? 0 : timeout / 1000;
        ts.tv_nsec = timeout < 0 ? 0 : (timeout % 1000) * 1000000;
        result = kevent(r->fd, NULL, 0, events, 64, timeout < 0 ? NULL : &ts);
        for (i = 0; result > 0 && i < (size_t)result; ++i)
            for (j = 0; j < n; ++j) if ((uintptr_t)fds[j].fd == events[i].ident) {
                fds[j].revents |= events[i].filter == EVFILT_READ ? POLLIN : POLLOUT;
                if (events[i].flags & (EV_ERROR | EV_EOF)) fds[j].revents |= POLLERR;
            }
    }
#endif
    return result;
fail:
    free(current); return -1;
}
