/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef IPC_TEST_CHECK_H
#define IPC_TEST_CHECK_H
#include <stdio.h>
#include <stdlib.h>
#define CHECK(expression) do { if (!(expression)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); abort(); \
} } while (0)
#endif
