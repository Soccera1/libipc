/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200112L
#include <libipc/ipc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
static double seconds(void) { LARGE_INTEGER t, f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f); return (double)t.QuadPart / f.QuadPart; }
#define PID GetCurrentProcessId()
#else
#include <time.h>
#include <unistd.h>
static double seconds(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
#define PID getpid()
#endif
typedef struct state { ipc_channel *channel; int complete, failed; size_t expected; } state;
static void done(const ipc_result *r, void *user) {
    state *s = user;
    if (r->status != IPC_OK) s->failed = 1;
    if (r->channel) s->channel = r->channel;
    if (r->message) { if (r->message->size != s->expected) s->failed = 1; ipc_message_release(r->message); }
    ++s->complete;
}
static int drive(ipc_context *c, state *s, int count) {
    while (s->complete < count && !s->failed) if (ipc_run_once(c, 5000) < 0) return 0;
    return !s->failed;
}
static int compare(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
int main(int argc, char **argv) {
    unsigned iterations = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 10000, i;
    size_t size = argc > 2 ? (size_t)strtoul(argv[2], NULL, 10) : 64;
    ipc_context *c; ipc_listener *listener; ipc_operation id; state a = {0}, b = {0};
    char name[40]; unsigned char *bytes; double *samples, total = 0;
    if (!iterations || iterations > 1000000 || size > 1024 * 1024) return 2;
    bytes = calloc(size ? size : 1, 1); samples = calloc(iterations, sizeof(*samples));
    if (!bytes || !samples || ipc_context_create(NULL, &c) != IPC_OK) return 1;
    snprintf(name, sizeof(name), "bench%lu", (unsigned long)PID);
    if (ipc_listen(c, name, &listener) || ipc_accept(listener, 5000, done, &a, &id) ||
        ipc_connect(c, name, 5000, done, &b, &id) || !drive(c, &a, 1) || !drive(c, &b, 1)) return 1;
    a.expected = b.expected = size;
    for (i = 0; i < iterations; ++i) {
        double start = seconds(); int target = a.complete + 2;
        if (ipc_send(a.channel, bytes, size, 5000, done, &a, &id) ||
            ipc_receive(b.channel, 5000, done, &b, &id) || !drive(c, &b, target - 1) ||
            ipc_send(b.channel, bytes, size, 5000, done, &b, &id) ||
            ipc_receive(a.channel, 5000, done, &a, &id) || !drive(c, &a, target) || !drive(c, &b, target)) return 1;
        samples[i] = seconds() - start; total += samples[i];
    }
    qsort(samples, iterations, sizeof(*samples), compare);
    printf("%s\nroundtrips=%u bytes=%lu median_us=%.2f p95_us=%.2f MiB_per_s=%.2f\n", ipc_build_info(),
        iterations, (unsigned long)size, samples[iterations / 2] * 1e6,
        samples[(size_t)(iterations - 1) * 95 / 100] * 1e6, iterations * (double)size * 2 / total / (1024 * 1024));
    ipc_context_destroy(c); free(bytes); free(samples); return 0;
}
