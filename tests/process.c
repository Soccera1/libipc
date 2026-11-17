/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200112L
#define _XOPEN_SOURCE 600
#include "process.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
unsigned long test_process_id(void) { return (unsigned long)GetCurrentProcessId(); }
static void terminate(test_process *child) {
    if (!child->native) return;
    TerminateProcess((HANDLE)child->native, 99);
    WaitForSingleObject((HANDLE)child->native, 5000);
    CloseHandle((HANDLE)child->native); child->native = 0;
}
int test_start_echo(const char *program, const char *endpoint, test_process *child) {
    SECURITY_ATTRIBUTES attributes = {sizeof(attributes), NULL, TRUE};
    STARTUPINFOA startup; PROCESS_INFORMATION process; HANDLE input, output;
    char command[4096], line[16]; size_t used = 0; ULONGLONG deadline;
    child->native = 0;
    if (snprintf(command, sizeof(command), "\"%s\" --server %s", program, endpoint) >= (int)sizeof(command)) return 0;
    if (!CreatePipe(&input, &output, &attributes, 0)) return 0;
    if (!SetHandleInformation(input, HANDLE_FLAG_INHERIT, 0)) { CloseHandle(input); CloseHandle(output); return 0; }
    memset(&startup, 0, sizeof(startup)); startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = output; startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    if (!CreateProcessA(program, command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process)) {
        CloseHandle(input); CloseHandle(output); return 0;
    }
    CloseHandle(output); CloseHandle(process.hThread); child->native = (intptr_t)process.hProcess;
    deadline = GetTickCount64() + 5000;
    while (used < sizeof(line) - 1 && GetTickCount64() < deadline) {
        DWORD available = 0, count = 0; char byte;
        if (!PeekNamedPipe(input, NULL, 0, NULL, &available, NULL)) break;
        if (!available) {
            if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) break;
            Sleep(1); continue;
        }
        if (!ReadFile(input, &byte, 1, &count, NULL) || !count) break;
        if (byte == '\n') { line[used] = 0; CloseHandle(input); if (!strcmp(line, "ready")) return 1; terminate(child); return 0; }
        if (byte != '\r') line[used++] = byte;
    }
    CloseHandle(input); terminate(child); return 0;
}
int test_wait_process(test_process *child) {
    DWORD code; int ok;
    if (!child->native) return 0;
    if (WaitForSingleObject((HANDLE)child->native, 5000) != WAIT_OBJECT_0) { terminate(child); return 0; }
    ok = GetExitCodeProcess((HANDLE)child->native, &code) && code == 0;
    CloseHandle((HANDLE)child->native); child->native = 0; return ok;
}
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
unsigned long test_process_id(void) { return (unsigned long)getpid(); }
static int64_t milliseconds(void) {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time)) return -1;
    return (int64_t)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}
static void terminate(test_process *child) {
    if (!child->native) return;
    kill((pid_t)child->native, SIGKILL);
    while (waitpid((pid_t)child->native, NULL, 0) < 0 && errno == EINTR) {}
    child->native = 0;
}
int test_start_echo(const char *program, const char *endpoint, test_process *child) {
    int fds[2]; pid_t pid; char line[16]; size_t used = 0; int64_t deadline;
    child->native = 0;
    if (pipe(fds)) return 0;
    pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return 0; }
    if (!pid) {
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(2);
        if (fds[1] != STDOUT_FILENO) close(fds[1]);
        execl(program, program, "--server", endpoint, (char *)NULL); _exit(3);
    }
    close(fds[1]); child->native = (intptr_t)pid; deadline = milliseconds() + 5000;
    while (used < sizeof(line) - 1) {
        int64_t remaining = deadline - milliseconds(); char byte;
        struct pollfd event = {fds[0], POLLIN, 0};
        if (remaining <= 0) break;
        if (poll(&event, 1, (int)remaining) < 0 && errno == EINTR) continue;
        if (!event.revents || read(fds[0], &byte, 1) != 1) break;
        if (byte == '\n') { line[used] = 0; close(fds[0]); if (!strcmp(line, "ready")) return 1; terminate(child); return 0; }
        if (byte != '\r') line[used++] = byte;
    }
    close(fds[0]); terminate(child); return 0;
}
int test_wait_process(test_process *child) {
    int status; int64_t deadline = milliseconds() + 5000;
    if (!child->native) return 0;
    while (milliseconds() < deadline) {
        pid_t result = waitpid((pid_t)child->native, &status, WNOHANG);
        struct timespec delay = {0, 1000000};
        if (result > 0) { child->native = 0; return WIFEXITED(status) && WEXITSTATUS(status) == 0; }
        if (result < 0 && errno != EINTR) break;
        nanosleep(&delay, NULL);
    }
    terminate(child); return 0;
}
#endif
