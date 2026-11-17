/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <libipc/ipc.h>
#include "check.h"
#include <windows.h>
#include <string.h>
static int child(const char *name, const char *event_name) {
    ipc_context *context; ipc_listener *listener; ipc_region *region; ipc_sync *sync;
    HANDLE event;
    CHECK(ipc_context_create(NULL, &context) == IPC_OK);
    CHECK(ipc_listen(context, name, &listener) == IPC_OK);
    CHECK(ipc_region_create(name, 1024, &region) == IPC_OK);
    CHECK(ipc_sync_create(name, 1, &sync) == IPC_OK);
    CHECK(ipc_sync_try_wait(sync) == IPC_OK);
    event = OpenEventA(EVENT_MODIFY_STATE, FALSE, event_name); CHECK(event != NULL);
    CHECK(SetEvent(event)); CloseHandle(event);
    Sleep(INFINITE); return 0;
}
int main(int argc, char **argv) {
    char name[13], event_name[80], executable[MAX_PATH], command[MAX_PATH + 160];
    HANDLE event, waits[2]; PROCESS_INFORMATION process; STARTUPINFOA startup; DWORD result;
    ipc_sync *sync, *missing_sync; ipc_region *missing_region;
    ipc_context *context; ipc_listener *listener;
    if (argc == 4 && !strcmp(argv[1], "--child")) return child(argv[2], argv[3]);
    snprintf(name, sizeof(name), "x%lu", (unsigned long)GetCurrentProcessId());
    snprintf(event_name, sizeof(event_name), "Local\\libipc_crash_test_%lu", (unsigned long)GetCurrentProcessId());
    event = CreateEventA(NULL, TRUE, FALSE, event_name); CHECK(event != NULL);
    CHECK(GetModuleFileNameA(NULL, executable, sizeof(executable)) > 0);
    snprintf(command, sizeof(command), "\"%s\" --child %s %s", executable, name, event_name);
    memset(&startup, 0, sizeof(startup)); startup.cb = sizeof(startup);
    CHECK(CreateProcessA(executable, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process));
    waits[0] = event; waits[1] = process.hProcess;
    result = WaitForMultipleObjects(2, waits, FALSE, 5000);
    if (result == WAIT_OBJECT_0) CHECK(ipc_sync_open(name, &sync) == IPC_OK);
    CHECK(TerminateProcess(process.hProcess, 99));
    CHECK(WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0);
    CloseHandle(process.hProcess); CloseHandle(process.hThread); CloseHandle(event);
    CHECK(result == WAIT_OBJECT_0);
    /* The held semaphore does not recover automatically while another handle lives. */
    CHECK(ipc_sync_try_wait(sync) == IPC_BUSY);
    CHECK(ipc_sync_post(sync) == IPC_OK && ipc_sync_try_wait(sync) == IPC_OK);
    ipc_sync_close(sync);
    CHECK(ipc_sync_open(name, &missing_sync) == IPC_SYSTEM);
    CHECK(ipc_region_open(name, &missing_region) == IPC_SYSTEM);
    CHECK(ipc_context_create(NULL, &context) == IPC_OK);
    CHECK(ipc_listen(context, name, &listener) == IPC_OK);
    ipc_context_destroy(context);
    puts("Windows crash cleanup passed"); return 0;
}
