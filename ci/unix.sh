#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eu
uname -a
cmake -S . -B build-native -DCMAKE_BUILD_TYPE=Release -DIPC_BUILD_CPP_TEST=ON
cmake --build build-native
(cd build-native && ctest --output-on-failure)
cmake -S . -B build-portable -DCMAKE_BUILD_TYPE=Release -DIPC_EVENT_BACKEND=poll -DIPC_BUILD_CPP_TEST=ON
cmake --build build-portable
(cd build-portable && ctest --output-on-failure)
build-native/ipc_benchmark 100 64
