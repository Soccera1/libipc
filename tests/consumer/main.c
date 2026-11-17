/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <libipc/ipc.h>
int main(void) {
    ipc_context *context;
    if (ipc_context_create(0, &context) != IPC_OK) return 1;
    if (ipc_run_once(context, 0) < 0) return 2;
    ipc_context_destroy(context); return 0;
}
