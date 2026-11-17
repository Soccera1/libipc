/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef IPC_TEST_PROCESS_H
#define IPC_TEST_PROCESS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct test_process { intptr_t native; } test_process;
unsigned long test_process_id(void);
int test_start_echo(const char *program, const char *endpoint, test_process *);
int test_wait_process(test_process *);
#ifdef __cplusplus
}
#endif
#endif
