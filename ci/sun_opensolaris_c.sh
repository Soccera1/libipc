#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Native C-only verification for Sun OpenSolaris 2009.06 with archived GCC 4.3.2.
set -eu
PATH=/usr/gcc/4.3/bin:/usr/gnu/bin:/usr/ccs/bin:/usr/bin:/usr/sbin
export PATH
uname -a
gcc-4.3.2 --version | head -1
for bits in 32 64; do
    output=build-sun-$bits
    mkdir -p "$output"
    cat > "$output/ipc_config.h" <<'CONFIG'
#define IPC_REACTOR_NAME "poll"
#define IPC_REACTOR_EPOLL 0
#define IPC_HAVE_SEM_CLOCKWAIT 0
CONFIG
    gcc-4.3.2 -m$bits -std=c99 -Wall -Wextra -Werror -Wno-missing-field-initializers -fPIC -D_POSIX_C_SOURCE=200112L -D_REENTRANT -D__EXTENSIONS__ -Iinclude -I"$output" -c src/ipc.c -o "$output/ipc.o"
    gcc-4.3.2 -m$bits -std=c99 -Wall -Wextra -Werror -Wno-missing-field-initializers -fPIC -D_POSIX_C_SOURCE=200112L -D_REENTRANT -D__EXTENSIONS__ -Iinclude -I"$output" -c src/reactor_poll.c -o "$output/reactor.o"
    ar cr "$output/libipc.a" "$output/ipc.o" "$output/reactor.o"
    gcc-4.3.2 -m$bits -shared -Wl,-h,libipc.so.0 -o "$output/libipc.so.0" "$output/ipc.o" "$output/reactor.o" -lpthread -lrt -lsocket -lnsl
    ln -sf libipc.so.0 "$output/libipc.so"
    for test in test crash framing behavior; do
        gcc-4.3.2 -m$bits -std=c99 -Wall -Wextra -Werror -Wno-missing-field-initializers -D_REENTRANT -D__EXTENSIONS__ -Iinclude "tests/$test.c" -L"$output" '-Wl,-R,$ORIGIN' -lipc -lpthread -lrt -lsocket -lnsl -o "$output/$test"
        "$output/$test"
        echo "NATIVE_C_PASS bits=$bits test=$test linkage=shared"
    done
    gcc-4.3.2 -m$bits -std=c99 -Wall -Wextra -Werror -Wno-missing-field-initializers -D_REENTRANT -D__EXTENSIONS__ -Iinclude tests/behavior.c "$output/libipc.a" -lpthread -lrt -lsocket -lnsl -o "$output/behavior-static"
    "$output/behavior-static"
    echo "NATIVE_C_PASS bits=$bits test=behavior linkage=static"
    gcc-4.3.2 -m$bits -std=c99 -Wall -Wextra -Werror -Wno-missing-field-initializers -Iinclude benchmarks/roundtrip.c "$output/libipc.a" -lpthread -lrt -lsocket -lnsl -o "$output/benchmark"
    "$output/benchmark" 100 64
    for example in echo shared_memory; do
        gcc-4.3.2 -m$bits -std=c99 -Wall -Wextra -Werror -Wno-missing-field-initializers -Iinclude "examples/$example.c" "$output/libipc.a" -lpthread -lrt -lsocket -lnsl -o "$output/$example"
    done
    file "$output/test" "$output/libipc.so.0"
    ldd "$output/test"
done

wait_ready() {
    attempts=0
    until grep '^ready$' "$1" >/dev/null; do
        kill -0 "$2"
        attempts=`expr "$attempts" + 1`
        if [ "$attempts" -ge 10 ]; then
            echo 'Example did not become ready' >&2
            return 1
        fi
        sleep 1
    done
}

for owner in 32 64; do
    peer=64
    if [ "$owner" = 64 ]; then peer=32; fi
    name="e$owner-$$"
    log="build-sun-$owner/cross-echo.log"
    "build-sun-$owner/echo" --server "$name" > "$log" &
    process=$!
    wait_ready "$log" "$process"
    response=`"build-sun-$peer/echo" --client "$name" cross-abi-message`
    test "$response" = cross-abi-message
    wait "$process"
    echo "NATIVE_ABI_PASS owner=$owner peer=$peer mechanism=messaging"

    name="m$owner-$$"
    log="build-sun-$owner/cross-memory.log"
    "build-sun-$owner/shared_memory" --owner "$name" > "$log" &
    process=$!
    wait_ready "$log" "$process"
    "build-sun-$peer/shared_memory" --write "$name" cross-abi-memory
    wait "$process"
    grep '^cross-abi-memory$' "$log"
    echo "NATIVE_ABI_PASS owner=$owner peer=$peer mechanism=shared_memory"
done
