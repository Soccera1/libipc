# libipc

**Portable local IPC for C, C++, and Rust.**

Write one set of code for local messaging, shared memory, and synchronization
across Unix and Windows. libipc provides a C99 core with a C ABI, C++ ownership
helpers, and Rust futures over the same implementation.

- **Messages with boundaries** over Unix-domain sockets or Windows named pipes.
- **Named shared memory** for fixed-size regions shared between processes.
- **Counting semaphores** for notifications and cooperative locks, with optional
  asynchronous waits through a bounded helper pool.
- **Explicit execution:** your application drives completions; callbacks run on
  that thread. Rust futures need no separate async runtime.
- **Native backends:** epoll, kqueue, poll, or IOCP, selected for the target at
  build time. Both shared and static libraries are built.

## Quick start

The C library needs **CMake 3.16+ and a C99 compiler**. C++17 and Rust are optional.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
(cd build && ctest --output-on-failure)
```

Run the echo server in one terminal:

```sh
./build/ipc_echo --server demo
```

When it prints `ready`, run the client in another:

```sh
./build/ipc_echo --client demo hello
# hello
```

The server handles one request and exits. The complete client and server are in
[examples/echo.c](examples/echo.c).

On Windows, build and test from a compiler-enabled PowerShell session:

```powershell
cmake -S . -B build
cmake --build build --config Release
Push-Location build
ctest -C Release --output-on-failure
Pop-Location
```

With a multi-configuration generator, executables are under `build/Release`;
for example, `.\build\Release\ipc_echo.exe --server demo`.

## Choose your interface

| Interface | Entry point | Programming model |
| --- | --- | --- |
| C99 | [`<libipc/ipc.h>`](include/libipc/ipc.h) | Opaque handles, callbacks, explicit resource management |
| C++17 | [`<libipc/ipc.hpp>`](include/libipc/ipc.hpp) | Move-only handles, callback adapters, RAII guards |
| Rust | [`libipc`](rust/libipc/src/lib.rs) | Futures, owned messages, RAII permits |
| Rust FFI | [`libipc-sys`](rust/libipc-sys/src/lib.rs) | Raw C ABI bindings |

In C, create a context, listen or connect, submit operations, then call
`ipc_run` or `ipc_run_once` to dispatch callbacks. The
[echo example](examples/echo.c) shows the complete flow and error handling.

For Rust, add a path dependency on the checkout:

```toml
[dependencies]
libipc = { path = "../libipc/rust/libipc" }
```

Adjust the path to your checkout. This client talks to the same C echo server:

```rust
use libipc::{Context, Error};

fn main() -> Result<(), Error> {
    let context = Context::new(0)?;
    context.block_on(async {
        let connected = context.connect("demo", 5_000)?.await?;
        let channel = connected.channel.expect("successful connection");
        channel.send(b"hello", 5_000)?.await?;
        let reply = channel.receive(5_000)?.await?;
        println!("{}", String::from_utf8_lossy(&reply.message.expect("received message")));
        Ok(())
    })
}
```

Cargo builds the C core with CMake by default. The crates currently require this
checkout. See [Rust build and cross-compilation options](docs/portability.md#rust-cross-builds)
for linking a prebuilt library.

## Add libipc to a project

Install the headers, libraries, and package metadata:

```sh
cmake --install build --prefix /your/prefix
```

For multi-configuration builds, also pass `--config Release`.

### CMake

Configure your application with `-DCMAKE_PREFIX_PATH=/your/prefix`, then use:

```cmake
find_package(libipc 0.1 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE libipc::shared)
# Or use libipc::static for static linkage.
```

The exported targets carry include directories and native system dependencies.
C++ applications include `libipc/ipc.hpp` and link the same library.

### pkg-config

Add the installed `lib/pkgconfig` or `lib64/pkgconfig` directory to
`PKG_CONFIG_PATH`, then query the appropriate module:

```sh
pkg-config --cflags --libs libipc
pkg-config --cflags --libs libipc-static
```

`libipc-static` explicitly selects the static archive and its native dependencies.
For downstream build examples, see [tests/consumer](tests/consumer).

## Shared memory and synchronization

The shared-memory example waits for a notification, prints the written text,
and cleans up its named objects:

```sh
# Terminal 1: wait for "ready".
./build/ipc_shm --owner demo

# Terminal 2:
./build/ipc_shm --write demo hello
```

Applications define the byte layout and synchronize mapped accesses. Regions
have a fixed size; closing a mapping does not destroy its name. Rust exposes
mapped storage through a raw pointer, so dereferencing it requires the usual
aliasing and synchronization guarantees.

Initialize a semaphore to **zero** for notifications or **one** for a cooperative
nonrecursive lock. Balance each successful acquisition with a post. There is no
ownership checking or automatic crash recovery. Set `sync_workers` above zero
to enable asynchronous waits; it is disabled by default. Completions still run
on the driver thread. C++ lock guards and Rust permits post on destruction.
Rust also refunds an acquired token if a successful wait is dropped before
being observed.

## Semantics to know

| Topic | Contract |
| --- | --- |
| Completion | Rejected submissions return an error with no callback. Each accepted operation completes exactly once through the driver. Callbacks never run inline. |
| Threading | Use a context and its handles on one application thread. Only `ipc_stop` and `ipc_wakeup` may be called from another thread. Do not recursively drive or destroy a context from a callback, or reuse it after `fork`. |
| Timeouts | Milliseconds from submission; `-1` means infinite. |
| Buffers | Sends copy the supplied bytes. A successful C receive transfers an `ipc_message` to the caller; release it with `ipc_message_release`. |
| Delivery | Send completion means the frame reached the local OS, without acknowledging peer processing. There is no automatic retry or reconnection. |
| Cancellation | Cancelling a partial send closes the channel. Cancelling a receive preserves its partial frame. Dropping a Rust operation requests cancellation; drive the context to finish it. |
| Limits | Defaults are 1 MiB per message and 8 MiB of queued sends per context. Receive storage is bounded per channel, not globally. |
| Lifetime | Context destruction cancels and drains outstanding operations. C listeners and channels remain context-owned until then; closing releases native resources. Keep the context alive during concurrent stop/wakeup calls. |

Endpoint names accept 1–40 ASCII letters, digits, hyphens, or underscores.
Shared-memory and semaphore names use the same alphabet with a **12-character
limit**. Names are scoped to the current user. Unix sockets live in a private
`/tmp/libipc-UID` directory; stale socket files after a crash require explicit
application cleanup.

POSIX named memory and semaphores survive until unlinked. Windows removes their
names after the final handle/view closes. See the
[platform lifetime differences](docs/portability.md#resource-lifetime-differences)
before designing cleanup across platforms.

## Platform coverage

Unix uses Unix-domain sockets, POSIX shared memory, and named semaphores.
Windows uses named pipes, file mappings, and named semaphores.

| Platform | Default reactor | Verification |
| --- | --- | --- |
| Linux | epoll | C/C++/Rust; GCC, Clang, and sanitizer checks |
| Windows Server 2025 x64 | IOCP | Native execution of MinGW C/C++/Rust binaries |
| FreeBSD, OpenBSD, NetBSD, DragonFly BSD | kqueue | Native C/C++/Rust and installed consumers; kqueue and poll |
| illumos / OmniOS | poll | Native C/C++/Rust and installed consumers |
| Sun OpenSolaris 2009.06 | poll | Native C builds, tests, and installed consumers; 32/64-bit interoperability |
| macOS | kqueue | Probably works; not natively tested |
| AIX 7.2+, HP-UX 11i v3 | poll | Probably work; not natively tested |
| Oracle Solaris 11.4 | poll | Implemented; not natively tested |

The C++ and Rust layers are optional. Their coverage on historical OpenSolaris
remains unverified. Exact OS/toolchain versions, test records, and legacy
build-tool adaptations are in the [portability notes](docs/portability.md).

## Configuration and testing

Each build contains one messaging backend, one shared-memory backend, and one
reactor, selected at configure time. `ipc_build_info()` reports the selection.
Any Unix build can choose the portable poll reactor:

```sh
cmake -S . -B build-poll -DIPC_EVENT_BACKEND=poll
```

| CMake option | Default | Purpose |
| --- | --- | --- |
| `BUILD_TESTING` | `ON` | Build the C regression tests |
| `IPC_BUILD_CPP_TEST` | `OFF` | Add ownership and interoperability tests; requires C++17 |
| `IPC_BUILD_EXAMPLES` | `ON` | Build `ipc_echo` and `ipc_shm` |
| `IPC_BUILD_BENCHMARKS` | `ON` | Build `ipc_benchmark` |
| `IPC_EVENT_BACKEND` | Platform default | Select `epoll`, `kqueue`, `poll`, or `iocp` where available |

Unsupported combinations fail configuration. CTest covers framing, timeouts,
cancellation, queue limits, duplex ordering, process crashes, semaphore fairness,
and shared/static linkage. To include the C++ and Rust tests:

```sh
cmake -S . -B build -DIPC_BUILD_CPP_TEST=ON
cmake --build build
(cd build && ctest --output-on-failure)
cargo test --workspace
```

For latency and throughput measurements:

```sh
./build/ipc_benchmark 10000 64
```

The arguments are the round-trip count and payload size in bytes. Results report
median/p95 latency and effective bidirectional throughput, including framing,
allocation, copying, and event-loop overhead.

## License

[LGPL-2.1-or-later](LICENSE).
