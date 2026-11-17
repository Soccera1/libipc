/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <libipc/ipc.h>
#include <stdio.h>
#include <string.h>
typedef struct state { ipc_context *context; ipc_region *region; int failed; } state;
static void notified(const ipc_result *r, void *user) {
    state *s = user;
    if (r->status == IPC_OK) printf("%.4095s\n", (char *)ipc_region_data(s->region));
    else s->failed = 1;
    ipc_stop(s->context);
}
int main(int argc, char **argv) {
    ipc_region *region = NULL; ipc_sync *sync = NULL; ipc_context *context = NULL;
    ipc_status status; int owner, failed = 0;
    if (argc < 3 || (strcmp(argv[1], "--owner") && strcmp(argv[1], "--write"))) {
        fprintf(stderr, "usage: ipc_shm --owner NAME | --write NAME TEXT\n"); return 2;
    }
    owner = !strcmp(argv[1], "--owner");
    if (!owner && (argc != 4 || strlen(argv[3]) >= 4096)) return 2;
    status = owner ? ipc_region_create(argv[2], 4096, &region) : ipc_region_open(argv[2], &region);
    if (status != IPC_OK) return 1;
    status = owner ? ipc_sync_create(argv[2], 0, &sync) : ipc_sync_open(argv[2], &sync);
    if (status != IPC_OK) {
        if (owner) ipc_region_destroy(argv[2]);
        ipc_region_close(region); return 1;
    }
    if (owner) {
        ipc_options options = {0, 0, 1}; ipc_operation id; state s;
        if (ipc_context_create(&options, &context) != IPC_OK) failed = 1;
        else {
            s.context = context; s.region = region; s.failed = 0;
            if (ipc_sync_wait(context, sync, 30000, notified, &s, &id) != IPC_OK) failed = 1;
            else { puts("ready"); fflush(stdout); ipc_run(context); failed = s.failed; }
            ipc_context_destroy(context);
        }
        ipc_sync_destroy(argv[2]); ipc_region_destroy(argv[2]);
    } else {
        if (ipc_region_size(region) != 4096) failed = 1;
        else { memcpy(ipc_region_data(region), argv[3], strlen(argv[3]) + 1); failed = ipc_sync_post(sync) != IPC_OK; }
    }
    ipc_sync_close(sync); ipc_region_close(region); return failed;
}
