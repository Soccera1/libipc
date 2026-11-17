#!/bin/sh
set -eu
PATH=/export/home/ipctest/cmake/bin:/export/home/ipctest/pkgconfig-modern/bin:/export/home/ipctest/pkgconfig/bin:/usr/gnu/bin:/usr/bin:/usr/ccs/bin:/usr/sbin:/sbin
LD_LIBRARY_PATH=/export/home/ipctest/toolchain/lib
export PATH LD_LIBRARY_PATH
cd /export/home/ipctest/libipc
root=`pwd`
cmake --version
pkg-config --version
for bits in 32 64; do
    build="$root/build-cmake-sun-$bits"
    cmake -S . -B "$build" -DCMAKE_C_COMPILER=/usr/bin/gcc-4.3.2 -DCMAKE_C_FLAGS="-m$bits" -DCMAKE_BUILD_TYPE=Release -DIPC_BUILD_CPP_TEST=OFF
    cmake --build "$build" --parallel 2
    (cd "$build" && ctest --output-on-failure)
    "$build/ipc_benchmark" 100 64
    package="$build/package-$$"
    mkdir "$package"
    cmake --install "$build" --prefix "$package/original"
    relocated="$package/relocated prefix"
    mv "$package/original" "$relocated"
    legacy="$package/legacy"
    cmake --install "$build" --prefix "$legacy"
    for linkage in shared static; do
        static=OFF
        if [ "$linkage" = static ]; then static=ON; fi
        consumer="$package/pkg-config-0.23-$linkage"
        PKG_CONFIG_LIBDIR="$legacy/lib/pkgconfig" PKG_CONFIG_PATH= cmake -S tests/consumer -B "$consumer" -DCMAKE_C_COMPILER=/usr/bin/gcc-4.3.2 -DCMAKE_C_FLAGS="-m$bits" -DCMAKE_BUILD_TYPE=Release -DUSE_STATIC="$static" -DUSE_PKG_CONFIG=ON -DPKG_CONFIG_EXECUTABLE=/export/home/ipctest/pkgconfig/bin/pkg-config
        cmake --build "$consumer"
        "$consumer/consumer"
        echo "NATIVE_LEGACY_PACKAGE_PASS bits=$bits linkage=$linkage"
    done
    PKG_CONFIG_LIBDIR="$relocated/lib/pkgconfig"
    PKG_CONFIG_PATH=
    export PKG_CONFIG_LIBDIR PKG_CONFIG_PATH
    for metadata in cmake pkg-config; do
        for linkage in shared static; do
            static=OFF
            if [ "$linkage" = static ]; then static=ON; fi
            pkg=OFF
            if [ "$metadata" = pkg-config ]; then pkg=ON; fi
            consumer="$package/$metadata-$linkage"
            if [ "$metadata" = pkg-config ]; then
                mkdir "$consumer"
                module=libipc
                if [ "$linkage" = static ]; then module=libipc-static; fi
                CC=/usr/bin/gcc-4.3.2 /usr/bin/python /var/tmp/consume-pkgconfig.py tests/consumer/main.c "$consumer/consumer" "$module" "$bits"
            else
                cmake -S tests/consumer -B "$consumer" -DCMAKE_C_COMPILER=/usr/bin/gcc-4.3.2 -DCMAKE_C_FLAGS="-m$bits" -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$relocated" -DUSE_STATIC="$static" -DUSE_PKG_CONFIG=OFF
                cmake --build "$consumer"
            fi
            "$consumer/consumer"
            ldd "$consumer/consumer" > "$consumer/dependencies.txt"
            if [ "$linkage" = shared ]; then
                grep 'libipc\.so' "$consumer/dependencies.txt"
            elif grep 'libipc\.so' "$consumer/dependencies.txt"; then
                echo 'Unexpected shared dependency in static consumer' >&2
                exit 1
            fi
            echo "NATIVE_PACKAGE_PASS bits=$bits metadata=$metadata linkage=$linkage"
        done
    done
done
