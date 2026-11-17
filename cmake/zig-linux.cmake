# Optional cross-check toolchain; e.g. -DIPC_ZIG_TARGET=x86-linux-musl.
set(CMAKE_SYSTEM_NAME Linux)
set(IPC_ZIG_TARGET x86-linux-musl CACHE STRING "Zig target triple")
set(CMAKE_C_COMPILER zig)
set(CMAKE_C_COMPILER_ARG1 "cc -target ${IPC_ZIG_TARGET}")
# Zig's linker driver does not accept newer CMake linker depfile arguments.
set(CMAKE_LINK_DEPENDS_USE_LINKER FALSE)
