/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "libipc/ipc.h"
#include "ipc_config.h"
#include "reactor.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <limits.h>

enum kind { ACCEPT, CONNECT, SEND, RECEIVE, WAIT };
struct ipc_listener {
    ipc_context *ctx; int fd;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    struct ipc_listener *next;
};
struct ipc_channel {
    ipc_context *ctx; int fd; unsigned char header[4]; size_t head, used, length;
    unsigned char *payload; struct ipc_channel *next;
};
struct ipc_sync { sem_t *sem; unsigned references; };
struct ipc_region { void *data; size_t size; };
typedef struct operation {
    ipc_operation id; enum kind kind; int fd, cancelled, working, done;
    int64_t deadline; ipc_callback callback; void *user;
    ipc_channel *channel; ipc_sync *sync; unsigned char *bytes; size_t size, offset;
    ipc_status status; struct operation *next;
} operation;
struct ipc_context {
    ipc_reactor reactor;
    int wake[2], stopped; size_t maximum, capacity, queued;
    ipc_operation serial, wait_cursor, wait_limit; operation *first, *last;
    ipc_listener *listeners; ipc_channel *channels;
    pthread_mutex_t mutex; pthread_cond_t condition; pthread_t *workers; unsigned worker_count; int quitting, driving;
};
static int64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int nonblock(int fd) {
    int f = fcntl(fd, F_GETFL, 0);
    return f < 0 || fcntl(fd, F_SETFL, f | O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ? -1 : 0;
}
static int valid_name(const char *name) {
    size_t i;
    if (!name || !*name || strlen(name) > 40) return 0;
    for (i = 0; name[i]; ++i) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return 1;
}
static int socket_path(const char *name, char *path, size_t size) {
    char dir[64]; struct stat st;
    if (!valid_name(name)) return -1;
    snprintf(dir, sizeof(dir), "/tmp/libipc-%lu", (unsigned long)geteuid());
    if (mkdir(dir, 0700) < 0 && errno != EEXIST) return -1;
    if (lstat(dir, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077)) return -1;
    return snprintf(path, size, "%s/%s", dir, name) >= (int)size ? -1 : 0;
}
static int object_name(const char *name, char type, char *out, size_t size) {
    if (!valid_name(name)) return -1;
    /* Keep names within restrictive POSIX semaphore implementations. */
    if (strlen(name) > 12) return -1;
#if defined(__hpux)
    /* HP-UX interprets POSIX object names as filesystem paths. */
    {
        char path[104];
        if (socket_path(name, path, sizeof(path))) return -1;
        return snprintf(out, size, "%s-%c", path, type) >= (int)size ? -1 : 0;
    }
#else
    return snprintf(out, size, "/i%c%lu_%s", type, (unsigned long)geteuid(), name) >= (int)size ? -1 : 0;
#endif
}
const char *ipc_build_info(void) { return "libipc 0.1.0; messaging=unix; shm=posix; reactor=" IPC_REACTOR_NAME; }
void ipc_wakeup(ipc_context *c) {
    char b = 0;
    if (c) while (write(c->wake[1], &b, 1) < 0 && errno == EINTR) {}
}
void ipc_stop(ipc_context *c) {
    if (!c) return;
    pthread_mutex_lock(&c->mutex); c->stopped = 1; pthread_mutex_unlock(&c->mutex);
    ipc_wakeup(c);
}
static void *worker(void *arg) {
    ipc_context *c = arg;
    for (;;) {
        operation *o, *chosen = NULL; int acquired = 0, error = 0, pass;
        pthread_mutex_lock(&c->mutex);
        if (c->quitting) { pthread_mutex_unlock(&c->mutex); break; }
        /* New arrivals must not extend a sweep indefinitely. */
        for (pass = 0; pass < 2 && !chosen; ++pass) {
            for (o = c->first; o; o = o->next) {
                if (o->kind == WAIT && o->sync && !o->done && !o->working &&
                    o->id > c->wait_cursor && o->id <= c->wait_limit) {
                    chosen = o; break;
                }
            }
            if (!chosen) { c->wait_cursor = 0; c->wait_limit = c->serial; }
        }
        if (!chosen) {
            pthread_cond_wait(&c->condition, &c->mutex);
            pthread_mutex_unlock(&c->mutex);
            continue;
        }
        o = chosen; o->working = 1; c->wait_cursor = o->id;
        pthread_mutex_unlock(&c->mutex);
#if IPC_HAVE_SEM_CLOCKWAIT
        {
            struct timespec until;
            clock_gettime(CLOCK_MONOTONIC, &until);
            until.tv_nsec += 1000000;
            if (until.tv_nsec >= 1000000000) { ++until.tv_sec; until.tv_nsec -= 1000000000; }
            acquired = sem_clockwait(o->sync->sem, CLOCK_MONOTONIC, &until) == 0;
            if (!acquired) error = errno;
        }
#else
        acquired = sem_trywait(o->sync->sem) == 0;
        if (!acquired) {
            struct timespec delay = {0, 1000000};
            error = errno; nanosleep(&delay, NULL);
        }
#endif
        pthread_mutex_lock(&c->mutex);
        o->working = 0;
        if (o->cancelled || c->quitting) { o->status = IPC_CANCELLED; o->done = 1; }
        else if (o->deadline >= 0 && now_ms() >= o->deadline) { o->status = IPC_TIMEOUT; o->done = 1; }
        else if (acquired) { o->status = IPC_OK; o->done = 1; }
        else if (error != EAGAIN && error != EINTR && error != ETIMEDOUT) { o->status = IPC_SYSTEM; o->done = 1; }
        if (acquired && o->status != IPC_OK) sem_post(o->sync->sem);
        if (o->done) ipc_wakeup(c);
        pthread_cond_broadcast(&c->condition);
        pthread_mutex_unlock(&c->mutex);
    }
    return NULL;
}
ipc_status ipc_context_create(const ipc_options *opt, ipc_context **out) {
    ipc_context *c; unsigned i; struct timespec probe;
    if (!out || (opt && opt->sync_workers > 64)) return IPC_INVALID;
    if (clock_gettime(CLOCK_MONOTONIC, &probe)) return IPC_SYSTEM;
    c = calloc(1, sizeof(*c)); if (!c) return IPC_SYSTEM;
    c->reactor.fd = -1;
    c->maximum = opt && opt->max_message_bytes ? opt->max_message_bytes : 1024 * 1024;
    c->capacity = opt && opt->max_queued_bytes ? opt->max_queued_bytes : 8 * 1024 * 1024;
    if (c->maximum > UINT32_MAX || pipe(c->wake) < 0) { free(c); return IPC_SYSTEM; }
    if (nonblock(c->wake[0]) || nonblock(c->wake[1]) || pthread_mutex_init(&c->mutex, NULL)) {
        close(c->wake[0]); close(c->wake[1]); free(c); return IPC_SYSTEM;
    }
    c->worker_count = opt ? opt->sync_workers : 0;
    if (pthread_cond_init(&c->condition, NULL)) {
        pthread_mutex_destroy(&c->mutex); close(c->wake[0]); close(c->wake[1]); free(c); return IPC_SYSTEM;
    }
    if (ipc_reactor_init(&c->reactor)) {
        pthread_cond_destroy(&c->condition); pthread_mutex_destroy(&c->mutex);
        close(c->wake[0]); close(c->wake[1]); free(c); return IPC_SYSTEM;
    }
    c->workers = calloc(c->worker_count ? c->worker_count : 1, sizeof(pthread_t));
    if (!c->workers) { c->worker_count = 0; ipc_context_destroy(c); return IPC_SYSTEM; }
    for (i = 0; i < c->worker_count; ++i) {
        if (pthread_create(&c->workers[i], NULL, worker, c)) {
            c->worker_count = i; ipc_context_destroy(c); return IPC_SYSTEM;
        }
    }
    *out = c; return IPC_OK;
}
static ipc_status submit(ipc_context *c, enum kind kind, int fd, ipc_channel *ch,
                         int timeout, ipc_callback cb, void *user, ipc_operation *id, operation **result) {
    operation *o;
    if (!c || !cb || !id || timeout < -1 || c->quitting) return IPC_INVALID;
    o = calloc(1, sizeof(*o)); if (!o) return IPC_SYSTEM;
    o->kind = kind; o->fd = fd; o->channel = ch; o->callback = cb; o->user = user;
    o->deadline = timeout < 0 ? -1 : now_ms() + timeout;
    pthread_mutex_lock(&c->mutex);
    o->id = ++c->serial;
    if (c->last) c->last->next = o; else c->first = o;
    c->last = o;
    pthread_mutex_unlock(&c->mutex);
    *id = o->id; *result = o; return IPC_OK;
}
ipc_status ipc_listen(ipc_context *c, const char *name, ipc_listener **out) {
    struct sockaddr_un a; ipc_listener *l;
    if (!c || c->quitting || !out) return IPC_INVALID;
    memset(&a, 0, sizeof(a)); a.sun_family = AF_UNIX;
    if (socket_path(name, a.sun_path, sizeof(a.sun_path))) return IPC_INVALID;
    l = calloc(1, sizeof(*l)); if (!l) return IPC_SYSTEM;
    l->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (l->fd < 0 || nonblock(l->fd) || bind(l->fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        if (l->fd >= 0) close(l->fd);
        free(l); return IPC_SYSTEM;
    }
    if (listen(l->fd, 64) < 0) { close(l->fd); unlink(a.sun_path); free(l); return IPC_SYSTEM; }
    l->ctx = c; memcpy(l->path, a.sun_path, sizeof(l->path)); l->next = c->listeners; c->listeners = l;
    *out = l; return IPC_OK;
}
void ipc_listener_close(ipc_listener *l) {
    if (l && l->fd >= 0) {
        operation *o;
        pthread_mutex_lock(&l->ctx->mutex);
        for (o = l->ctx->first; o; o = o->next)
            if (o->kind == ACCEPT && o->fd == l->fd) o->fd = -1;
        pthread_mutex_unlock(&l->ctx->mutex);
        close(l->fd); l->fd = -1; unlink(l->path);
    }
}
void ipc_channel_close(ipc_channel *ch) {
    if (ch && ch->fd >= 0) {
        close(ch->fd); ch->fd = -1;
        free(ch->payload); ch->payload = NULL;
        ch->head = ch->used = ch->length = 0;
    }
}
ipc_status ipc_accept(ipc_listener *l, int t, ipc_callback cb, void *u, ipc_operation *id) {
    operation *o;
    if (!l || l->fd < 0) return IPC_INVALID;
    return submit(l->ctx, ACCEPT, l->fd, NULL, t, cb, u, id, &o);
}
ipc_status ipc_connect(ipc_context *c, const char *name, int t, ipc_callback cb, void *u, ipc_operation *id) {
    struct sockaddr_un a; int fd, rc; operation *o; ipc_status s;
    /* Reject invalid submissions before creating a connection at the peer. */
    if (!c || c->quitting || t < -1 || !cb || !id) return IPC_INVALID;
    memset(&a, 0, sizeof(a)); a.sun_family = AF_UNIX;
    if (socket_path(name, a.sun_path, sizeof(a.sun_path))) return IPC_INVALID;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return IPC_SYSTEM;
    if (nonblock(fd)) { close(fd); return IPC_SYSTEM; }
    rc = connect(fd, (struct sockaddr *)&a, sizeof(a));
    if (rc < 0 && errno != EINPROGRESS) { close(fd); return IPC_SYSTEM; }
    s = submit(c, CONNECT, fd, NULL, t, cb, u, id, &o);
    if (s != IPC_OK) close(fd);
    return s;
}
ipc_status ipc_send(ipc_channel *ch, const void *data, size_t n, int t, ipc_callback cb, void *u, ipc_operation *id) {
    unsigned char *bytes; operation *o; ipc_status s; ipc_context *c;
    if (!ch || ch->fd < 0 || (n && !data)) return IPC_INVALID;
    c = ch->ctx;
    if (n > c->maximum || n > SIZE_MAX - 4 || n + 4 > c->capacity - c->queued) return IPC_LIMIT;
    bytes = malloc(n + 4); if (!bytes) return IPC_SYSTEM;
    bytes[0] = (unsigned char)(n >> 24); bytes[1] = (unsigned char)(n >> 16);
    bytes[2] = (unsigned char)(n >> 8); bytes[3] = (unsigned char)n;
    if (n) memcpy(bytes + 4, data, n);
    s = submit(c, SEND, ch->fd, ch, t, cb, u, id, &o);
    if (s != IPC_OK) { free(bytes); return s; }
    o->bytes = bytes; o->size = n + 4; c->queued += o->size; return IPC_OK;
}
ipc_status ipc_receive(ipc_channel *ch, int t, ipc_callback cb, void *u, ipc_operation *id) {
    operation *o;
    if (!ch || ch->fd < 0) return IPC_INVALID;
    return submit(ch->ctx, RECEIVE, ch->fd, ch, t, cb, u, id, &o);
}
ipc_status ipc_cancel(ipc_context *c, ipc_operation id) {
    operation *o; ipc_status s = IPC_INVALID;
    if (!c) return s;
    pthread_mutex_lock(&c->mutex);
    for (o = c->first; o; o = o->next) if (o->id == id) { o->cancelled = 1; s = IPC_OK; break; }
    pthread_mutex_unlock(&c->mutex); return s;
}
static ssize_t safe_send(int fd, const void *p, size_t n) {
#if defined(MSG_NOSIGNAL)
    return send(fd, p, n, MSG_NOSIGNAL);
#elif defined(SO_NOSIGPIPE)
    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled))) return -1;
    return send(fd, p, n, 0);
#else
    sigset_t set, old, pending; int had, saved; ssize_t rc; struct timespec zero = {0, 0};
    sigemptyset(&set); sigaddset(&set, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &set, &old)) { errno = EIO; return -1; }
    sigpending(&pending); had = sigismember(&pending, SIGPIPE);
    rc = send(fd, p, n, 0); saved = errno;
    if (rc < 0 && saved == EPIPE && !had) (void)sigtimedwait(&set, NULL, &zero);
    pthread_sigmask(SIG_SETMASK, &old, NULL); errno = saved; return rc;
#endif
}
static ipc_channel *channel_new(ipc_context *c, int fd) {
    ipc_channel *ch = calloc(1, sizeof(*ch));
    if (!ch) { close(fd); return NULL; }
    ch->ctx = c; ch->fd = fd; ch->next = c->channels; c->channels = ch; return ch;
}
static int receive_step(operation *o, ipc_message **out) {
    ipc_channel *ch = o->channel; ssize_t n;
    if (ch->head < 4) {
        n = recv(ch->fd, ch->header + ch->head, 4 - ch->head, 0);
        if (n <= 0) return n == 0 ? -1 : (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1);
        ch->head += (size_t)n;
        if (ch->head < 4) return 0;
        ch->length = ((uint32_t)ch->header[0] << 24) | ((uint32_t)ch->header[1] << 16) |
                     ((uint32_t)ch->header[2] << 8) | ch->header[3];
        if (ch->length > ch->ctx->maximum) return -1;
        ch->payload = malloc(ch->length ? ch->length : 1);
        if (!ch->payload) return -1;
    }
    if (ch->used < ch->length) {
        n = recv(ch->fd, ch->payload + ch->used, ch->length - ch->used, 0);
        if (n <= 0) return n == 0 ? -1 : (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1);
        ch->used += (size_t)n;
        if (ch->used < ch->length) return 0;
    }
    *out = malloc(sizeof(**out)); if (!*out) return -1;
    (*out)->data = ch->payload; (*out)->size = ch->length;
    ch->payload = NULL; ch->head = ch->used = ch->length = 0; return 1;
}
void ipc_message_release(ipc_message *m) { if (m) { free(m->data); free(m); } }
int ipc_run_once(ipc_context *c, int timeout) {
    operation *o, *prev; struct pollfd *fds; size_t count = 1, i; int callbacks = 0;
    if (!c || timeout < -1 || c->driving) return -1;
    c->driving = 1;
    pthread_mutex_lock(&c->mutex);
    if (c->quitting) goto dispatch;
    for (o = c->first; o; o = o->next) ++count;
    fds = calloc(count, sizeof(*fds));
    if (!fds) { pthread_mutex_unlock(&c->mutex); c->driving = 0; return -1; }
    fds[0].fd = c->wake[0]; fds[0].events = POLLIN;
    for (o = c->first, i = 1; o; o = o->next, ++i) {
        int64_t left = o->deadline < 0 ? -1 : o->deadline - now_ms();
        fds[i].fd = o->kind == WAIT ? -1 : o->fd;
        fds[i].events = o->kind == SEND || o->kind == CONNECT ? POLLOUT : POLLIN;
        if (!o->working && (o->cancelled || o->done || c->quitting)) timeout = 0;
        if ((o->channel && o->channel->fd < 0) || (o->kind == ACCEPT && o->fd < 0)) {
            fds[i].fd = -1; timeout = 0;
        }
        if (!o->working && left >= 0 && left <= INT_MAX && (timeout < 0 || left < timeout)) timeout = (int)left;
        if (!o->working && o->deadline >= 0 && left < 0) timeout = 0;
    }
    pthread_mutex_unlock(&c->mutex);
    if (ipc_reactor_wait(&c->reactor, fds, count, timeout) < 0 && errno != EINTR) { free(fds); c->driving = 0; return -1; }
    { char b[128]; while (read(c->wake[0], b, sizeof(b)) > 0) {} }
    free(fds);
    pthread_mutex_lock(&c->mutex);
dispatch:
    prev = NULL; o = c->first;
    /* Snapshot the tail: callbacks may submit operations for the next turn. */
    { operation *tail = c->last;
    while (o) {
        ipc_result r; int completed = 0, last = o == tail;
        memset(&r, 0, sizeof(r)); r.operation = o->id; r.status = IPC_OK;
        if (o->working) { prev = o; o = o->next; if (last) break; continue; }
        if (o->cancelled || c->quitting) { r.status = IPC_CANCELLED; completed = 1; }
        else if (o->kind == WAIT && o->done) { r.status = o->status; completed = 1; }
        else if (o->deadline >= 0 && now_ms() >= o->deadline) { r.status = IPC_TIMEOUT; completed = 1; }
        else if (o->kind != WAIT) {
            operation *p; int blocked = 0;
            for (p = c->first; p != o; p = p->next)
                if (p->kind == o->kind && p->fd == o->fd) blocked = 1;
            if ((o->channel && o->channel->fd < 0) || (o->kind == ACCEPT && o->fd < 0)) { r.status = IPC_CLOSED; completed = 1; }
            else if (!blocked && o->kind == ACCEPT) {
                int fd = accept(o->fd, NULL, NULL);
                if (fd >= 0) {
                    if (nonblock(fd)) { close(fd); r.status = IPC_SYSTEM; }
                    else if (!(r.channel = channel_new(c, fd))) r.status = IPC_SYSTEM;
                    completed = 1;
                } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) { completed = 1; r.status = IPC_CLOSED; }
            } else if (!blocked && o->kind == CONNECT) {
                struct pollfd f = {o->fd, POLLOUT, 0};
                if (poll(&f, 1, 0) > 0) {
                    int err = 0; socklen_t len = sizeof(err);
                    if (getsockopt(o->fd, SOL_SOCKET, SO_ERROR, &err, &len) || err) r.status = IPC_SYSTEM;
                    else { r.channel = channel_new(c, o->fd); o->fd = -1; if (!r.channel) r.status = IPC_SYSTEM; }
                    completed = 1;
                }
            } else if (!blocked && o->kind == SEND) {
                ssize_t n = safe_send(o->fd, o->bytes + o->offset, o->size - o->offset);
                if (n > 0) { o->offset += (size_t)n; completed = o->offset == o->size; }
                else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) { r.status = IPC_CLOSED; completed = 1; }
            } else if (!blocked && o->kind == RECEIVE) {
                int rc = receive_step(o, &r.message);
                if (rc) { completed = 1; if (rc < 0) r.status = IPC_CLOSED; }
            }
        }
        if (!completed) { prev = o; o = o->next; if (last) break; continue; }
        if (o->kind == WAIT && o->done && o->status == IPC_OK && r.status != IPC_OK) sem_post(o->sync->sem);
        if (o->channel && r.status != IPC_OK &&
            ((o->kind == SEND && o->offset) || r.status == IPC_CLOSED)) ipc_channel_close(o->channel);
        if (o->kind == CONNECT && o->fd >= 0) close(o->fd);
        if (prev) prev->next = o->next; else c->first = o->next;
        if (c->last == o) c->last = prev;
        if (o->kind == SEND) c->queued -= o->size;
        if (o->sync && --o->sync->references == 0) { sem_close(o->sync->sem); free(o->sync); }
        { ipc_callback cb = o->callback; void *u = o->user;
          free(o->bytes); free(o);
          pthread_mutex_unlock(&c->mutex); cb(&r, u); ++callbacks; pthread_mutex_lock(&c->mutex); }
        o = prev ? prev->next : c->first;
        if (last) break;
    }}
    pthread_mutex_unlock(&c->mutex); c->driving = 0; return callbacks;
}
void ipc_run(ipc_context *c) {
    if (!c) return;
    pthread_mutex_lock(&c->mutex); c->stopped = 0; pthread_mutex_unlock(&c->mutex);
    for (;;) {
        int stop;
        pthread_mutex_lock(&c->mutex); stop = c->stopped; pthread_mutex_unlock(&c->mutex);
        if (stop || ipc_run_once(c, -1) < 0) break;
    }
}
void ipc_context_destroy(ipc_context *c) {
    unsigned i; ipc_channel *ch; ipc_listener *l;
    if (!c || c->driving) return;
    pthread_mutex_lock(&c->mutex); c->quitting = 1; pthread_cond_broadcast(&c->condition); pthread_mutex_unlock(&c->mutex);
    for (i = 0; i < c->worker_count; ++i) pthread_join(c->workers[i], NULL);
    while (c->first) ipc_run_once(c, 0);
    while ((ch = c->channels)) { c->channels = ch->next; ipc_channel_close(ch); free(ch->payload); free(ch); }
    while ((l = c->listeners)) { c->listeners = l->next; ipc_listener_close(l); free(l); }
    ipc_reactor_destroy(&c->reactor);
    close(c->wake[0]); close(c->wake[1]); pthread_cond_destroy(&c->condition); pthread_mutex_destroy(&c->mutex); free(c->workers); free(c);
}
static ipc_status region(const char *name, size_t size, int create, ipc_region **out) {
    char path[104]; int fd; struct stat st; ipc_region *r;
    if (!out || object_name(name, 'm', path, sizeof(path)) ||
        (create && (!size || (off_t)size < 0 || (uintmax_t)(off_t)size != (uintmax_t)size))) return IPC_INVALID;
    fd = shm_open(path, O_RDWR | (create ? O_CREAT | O_EXCL : 0), 0600);
    if (fd < 0) return IPC_SYSTEM;
    if ((create && ftruncate(fd, (off_t)size)) || fstat(fd, &st) || st.st_size <= 0 || (uintmax_t)st.st_size > SIZE_MAX) {
        close(fd); if (create) shm_unlink(path); return IPC_SYSTEM;
    }
    r = malloc(sizeof(*r)); if (!r) { close(fd); if (create) shm_unlink(path); return IPC_SYSTEM; }
    r->size = (size_t)st.st_size; r->data = mmap(NULL, r->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); close(fd);
    if (r->data == MAP_FAILED) { free(r); if (create) shm_unlink(path); return IPC_SYSTEM; }
    *out = r; return IPC_OK;
}
ipc_status ipc_region_create(const char *n, size_t s, ipc_region **r) { return region(n, s, 1, r); }
ipc_status ipc_region_open(const char *n, ipc_region **r) { return region(n, 0, 0, r); }
void *ipc_region_data(ipc_region *r) { return r ? r->data : NULL; }
size_t ipc_region_size(const ipc_region *r) { return r ? r->size : 0; }
void ipc_region_close(ipc_region *r) { if (r) { munmap(r->data, r->size); free(r); } }
ipc_status ipc_region_destroy(const char *n) { char p[104]; if (object_name(n, 'm', p, sizeof(p))) return IPC_INVALID; return shm_unlink(p) ? IPC_SYSTEM : IPC_OK; }
static ipc_status sync_open(const char *name, unsigned value, int create, ipc_sync **out) {
    char p[104]; ipc_sync *s;
    if (!out || object_name(name, 's', p, sizeof(p))) return IPC_INVALID;
    s = malloc(sizeof(*s)); if (!s) return IPC_SYSTEM;
    s->sem = sem_open(p, create ? O_CREAT | O_EXCL : 0, 0600, value); s->references = 1;
    if (s->sem == SEM_FAILED) { free(s); return IPC_SYSTEM; }
    *out = s; return IPC_OK;
}
ipc_status ipc_sync_create(const char *n, unsigned v, ipc_sync **s) { return sync_open(n, v, 1, s); }
ipc_status ipc_sync_open(const char *n, ipc_sync **s) { return sync_open(n, 0, 0, s); }
void ipc_sync_close(ipc_sync *s) { if (s && --s->references == 0) { sem_close(s->sem); free(s); } }
ipc_status ipc_sync_destroy(const char *n) { char p[104]; if (object_name(n, 's', p, sizeof(p))) return IPC_INVALID; return sem_unlink(p) ? IPC_SYSTEM : IPC_OK; }
ipc_status ipc_sync_post(ipc_sync *s) { return !s ? IPC_INVALID : sem_post(s->sem) ? IPC_SYSTEM : IPC_OK; }
ipc_status ipc_sync_try_wait(ipc_sync *s) { if (!s) return IPC_INVALID; return sem_trywait(s->sem) == 0 ? IPC_OK : errno == EAGAIN ? IPC_BUSY : IPC_SYSTEM; }
ipc_status ipc_sync_wait(ipc_context *c, ipc_sync *s, int t, ipc_callback cb, void *u, ipc_operation *id) {
    operation *o; ipc_status status;
    if (!c || !s) return IPC_INVALID;
    if (!c->worker_count) return IPC_DISABLED;
    /* Workers see only fully initialized WAIT requests. */
    status = submit(c, WAIT, -1, NULL, t, cb, u, id, &o);
    if (status == IPC_OK) {
        pthread_mutex_lock(&c->mutex); o->sync = s; ++s->references; pthread_cond_broadcast(&c->condition); pthread_mutex_unlock(&c->mutex);
    }
    return status;
}
