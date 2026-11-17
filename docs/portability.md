# Portability and verification

Every build contains one messaging backend, one shared-memory backend, and one
reactor. Configuration probes compile and link rather than running target code.
`ipc_build_info()` reports the selected configuration.

| Platform | Messaging / memory | Default reactor | Evidence in this workspace |
| --- | --- | --- | --- |
| Linux | Unix sockets / POSIX | epoll | GCC, Clang, ASan/UBSan tests |
| Windows Server 2025 x64 | Named pipes / file mappings | IOCP | KVM: MinGW C/Rust static/shared, C++ ownership and C interoperability; also tested under Wine |
| macOS | Unix sockets / POSIX | kqueue | Expected to work; not natively tested |
| FreeBSD 14.3 amd64 | Unix sockets / POSIX | kqueue | KVM: C/C++/Rust and installed consumers, kqueue + poll |
| OpenBSD 8.0 amd64 | Unix sockets / POSIX | kqueue | KVM: C/C++/Rust and installed consumers, kqueue + poll |
| NetBSD 10.1 amd64 | Unix sockets / POSIX | kqueue | KVM: C/C++/Rust and installed consumers, kqueue + poll |
| DragonFly 6.4.2 x86_64 | Unix sockets / POSIX | kqueue | KVM: C/C++/Rust and installed consumers, kqueue + poll |
| illumos (OmniOS r151050 amd64) | Unix sockets / POSIX | poll | KVM: C/C++/Rust; relocated CMake/pkg-config consumers |
| Sun OpenSolaris 2009.06 x86 | Unix sockets / POSIX | poll | KVM: CMake/C tests, benchmark, installed consumers; 32-bit + 64-bit and cross-ABI examples |
| Oracle Solaris 11.4 | Unix sockets / POSIX | poll | Implemented; native tests pending |
| AIX 7.2+ | Unix sockets / POSIX | poll | Expected to work; not natively tested |
| HP-UX 11i v3 | Unix sockets / POSIX | poll | Expected to work with filesystem object-name adaptation; not natively tested |

Solaris-family testing covers Sun's original OpenSolaris 2009.06 as well as
illumos. The [Sun OpenSolaris record](verification/opensolaris-2009.06.json)
covers six CMake/CTest cases, five direct C test invocations, and the benchmark
per ABI, using native GCC 4.3.2 in 32-bit and 64-bit modes on the original
`snv_111b` kernel. Messaging, shared memory, and semaphore notification also
interoperate between 32-bit and 64-bit processes in both directions. The
[C-only recipe](../ci/sun_opensolaris_c.sh) builds directly with the archived
compiler. Optional C++17 helpers and Rust remain unverified on this historical OS.
The benchmark uses the POSIX.1-2001 namespace recognized by its C99 headers.

The saved [CMake harness](verification/opensolaris-cmake-harness.sh) tests shared
and static consumers of both CMake exports and pkg-config metadata after moving
the installation to a path containing spaces. CMake 3.16.9 was built using GCC
4.8.5 with its `SUNOS_NO_IFADDRS` fallback and this
[timestamp compatibility patch](verification/opensolaris-cmake-3.16.patch).
pkg-config 0.29.2 uses a small
[GLib header-order patch](verification/opensolaris-pkg-config-0.29.2.patch).
Original pkg-config 0.23 passes normal-prefix consumers but truncates paths
containing spaces. CMake 3.16's FindPkgConfig parser also splits escaped spaces;
the relocated pkg-config checks use a
[direct compiler consumer](verification/opensolaris-pkgconfig-consumer.py) that
parses the emitted arguments with Python 2.4's `shlex`. CMake export consumers
are tested separately. These build-tool adaptations are independent of the C
library's runtime dependencies.

Native OmniOS r151050 tests cover eight C/C++ tests, six Rust tests, and four
relocated installed consumers in each of the default and explicit-poll builds.
The verification record below identifies the exact kernel and toolchains.
Oracle Solaris 11.4 remains separately unverified.

The six Rust tests and documentation example pass on 32-bit Linux/glibc,
including rejection of message limits exceeding Rust's `isize::MAX` slice bound.
The 32-bit Linux/musl static behavior suite passes through qemu-i386. The
big-endian PowerPC64 Linux/musl static behavior suite and benchmark pass through
qemu-ppc64, including framing, duplex ordering, and semaphore cancellation/refund
tests. These architecture checks do not establish support for vendor Unix
kernels. The [FreeBSD](verification/freebsd-14.3.json),
[OpenBSD](verification/openbsd-8.0.json),
[NetBSD](verification/netbsd-10.1.json),
[DragonFly](verification/dragonflybsd-6.4.2.json), and
[illumos](verification/omnios-r151050.json) verification records contain
compiler/runtime versions and hashes checked against the guest source.

The [native Windows record](verification/windows-native.json) covers 11 harness
commands on Windows Server 2025 Datacenter Evaluation, build 26100.1742, in a
network-isolated KVM guest. It includes C integration, crash cleanup, shared and
static behavior, adversarial framing, C++ ownership and C interoperability, and
six Rust tests against each of the static library and DLL. The binaries were
cross-compiled with MinGW; native MSVC builds and installed Windows consumers
remain unverified by this gate.
The [saved harness](verification/windows-native-harness.ps1) is the exact script
used with the immutable test ISO. Its `Z:` drive mapping matches the C peer path
compiled into those Rust test binaries. It writes results to COM2 (or COM1) and
the guest disk, then shuts down the disposable guest.

Wine is a separate compatibility check. A checked-in CI recipe is not evidence
that a job passed. Keep unexecuted ports explicitly unverified. C++ and Rust
coverage is independent of C: neither toolchain is required for the C library.
The [Windows Rust record](verification/windows-rust-wine.json) covers six tests
against each of the static library and DLL. Its Windows standard library was
built from the installed Rust sources; compiler/runtime details are recorded.
The [Windows framing record](verification/windows-framing-wine.json) covers a
raw named-pipe peer: fragmented header/payload cancellation, oversized lengths,
peer EOF during either header or payload, and backpressured-send timeouts while
native I/O drains. The same cases run on Unix.

## Native testing

With CMake 3.16+, a C99 compiler, and optional C++17 installed, run
`sh ci/unix.sh`. Without C++17, configure without `IPC_BUILD_CPP_TEST` and run the
C tests first. Select vendor compilers using CC/CXX or CMAKE_TOOLCHAIN_FILE.
Record OS version, compiler version, word size, byte order, configuration,
CTest output, and Cargo output when Rust is available. Execute both the default
reactor and poll. Use separate build directories for 32/64-bit configurations.

On Windows:

```powershell
cmake -S . -B build -DIPC_BUILD_CPP_TEST=ON
cmake --build build --config Release
Push-Location build
ctest -C Release --output-on-failure
Pop-Location
cargo test --workspace
```

With MinGW and Wine on Unix, configure with
`-DCMAKE_TOOLCHAIN_FILE=cmake/mingw.cmake` and run the resulting ipc_test.exe,
ipc_behavior_shared.exe, and ipc_behavior_static.exe using Wine.

Solaris/illumos links socket and nsl. The rt library is linked when required.
HP-UX uses names under the owner-private runtime directory for POSIX objects.
Native archive/DLL naming follows CMake; static libraries have a distinct name
where AIX shared archives or Windows import libraries would otherwise collide.
OpenBSD shared libraries use its two-component ABI filename convention
(`libipc.so.MAJOR.MINOR`) so native linker and pkg-config discovery agree.

## Resource lifetime differences

POSIX unlink removes the name while existing handles and mappings remain valid.
Windows cannot unlink a named mapping or semaphore with other handles alive:
`destroy` checks the object and the final handle/view close removes the name.
Keep the owning process alive while peers open objects. Windows object names
include the process user SID and use an owner-restricted DACL; pipes reject
remote clients. Regions have private fixed-width metadata before the payload.

## Rust cross builds

Set CMAKE_TOOLCHAIN_FILE for a bundled cross build. To link a prebuilt target
library instead, set LIBIPC_LIB_DIR, LIBIPC_LINK_KIND (static or dylib), and
LIBIPC_SYSTEM_LIBS (space-separated native dependencies). Bundled builds read
the native dependency list generated by CMake. LIBIPC_TEST_PEER may point to a
target ipc_echo executable for interoperability testing with prebuilt libraries.

The Rust crates currently require this checkout; preparing standalone crate
publication is outside the initial release. macOS, AIX, and HP-UX are accepted
as expected to work without native verification; additional native coverage can
be added when runners are available. Crash cleanup
passes on Linux, native FreeBSD/OpenBSD/NetBSD/DragonFly/illumos, and native
Windows Server 2025 (also under Wine). Relocated CMake
static/shared consumers, pkg-config metadata, and rejected invalid backend
selections have been exercised locally.

With Python 3 and pkg-config installed, `python3 ci/check_package.py build-native`
installs the configured artifacts into a temporary directory, relocates them to
a path containing spaces, and compiles and runs four downstream consumers:
CMake and pkg-config, each with shared and static linkage. The explicit
`libipc-static` pkg-config module selects a distinct static archive name and
includes the configured platform's native dependencies.
On Unix, the check also inspects executable dependencies to verify that the
selected shared/static linkage was actually used.
Use `--generator Ninja` with Ninja installed when the system's make cannot
represent dependencies containing spaces (including BSD make).
