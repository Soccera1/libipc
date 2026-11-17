// SPDX-License-Identifier: LGPL-2.1-or-later
use std::{
    collections::hash_map::DefaultHasher,
    env,
    hash::{Hash, Hasher},
    path::PathBuf,
    process::Command,
};
fn main() {
    let root = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap()).join("../..");
    for name in ["src", "include", "CMakeLists.txt", "cmake", "examples"] {
        println!("cargo:rerun-if-changed={}", root.join(name).display());
    }
    for key in [
        "LIBIPC_LIB_DIR",
        "LIBIPC_LINK_KIND",
        "LIBIPC_SYSTEM_LIBS",
        "CMAKE_TOOLCHAIN_FILE",
        "IPC_EVENT_BACKEND",
        "IPC_MESSAGE_BACKEND",
        "IPC_SHM_BACKEND",
        "CC",
        "CFLAGS",
        "CMAKE_GENERATOR",
    ] {
        println!("cargo:rerun-if-env-changed={key}");
    }
    if let Some(directory) = env::var_os("LIBIPC_LIB_DIR") {
        let kind = env::var("LIBIPC_LINK_KIND").unwrap_or_else(|_| "static".into());
        assert!(
            kind == "static" || kind == "dylib",
            "LIBIPC_LINK_KIND must be static or dylib"
        );
        let os = env::var("CARGO_CFG_TARGET_OS").unwrap();
        let library = if kind == "static" && matches!(os.as_str(), "windows" | "aix") {
            "ipc_static"
        } else {
            "ipc"
        };
        println!(
            "cargo:rustc-link-search=native={}",
            PathBuf::from(directory).display()
        );
        println!("cargo:rustc-link-lib={kind}={library}");
        for library in env::var("LIBIPC_SYSTEM_LIBS")
            .unwrap_or_default()
            .split_whitespace()
        {
            println!("cargo:rustc-link-lib={library}");
        }
        return;
    }
    // Cargo owns this cache. Separate configurations must not inherit CMake's
    // previous compiler or backend choice when an environment override changes.
    let mut signature = DefaultHasher::new();
    for key in [
        "HOST",
        "TARGET",
        "CC",
        "CFLAGS",
        "CMAKE_GENERATOR",
        "CMAKE_TOOLCHAIN_FILE",
        "IPC_EVENT_BACKEND",
        "IPC_MESSAGE_BACKEND",
        "IPC_SHM_BACKEND",
    ] {
        key.hash(&mut signature);
        env::var_os(key).hash(&mut signature);
    }
    std::fs::read(root.join("CMakeLists.txt"))
        .unwrap()
        .hash(&mut signature);
    if let Some(toolchain) = env::var_os("CMAKE_TOOLCHAIN_FILE") {
        let path = PathBuf::from(toolchain);
        let path = if path.is_absolute() {
            path
        } else {
            root.join(path)
        };
        println!("cargo:rerun-if-changed={}", path.display());
        std::fs::read(path).ok().hash(&mut signature);
    }
    let out = PathBuf::from(env::var_os("OUT_DIR").unwrap())
        .join(format!("native-{:016x}", signature.finish()));
    let mut configure = Command::new("cmake");
    configure
        .arg("-S")
        .arg(&root)
        .arg("-B")
        .arg(&out)
        .arg("-DBUILD_TESTING=OFF");
    if let Some(toolchain) = env::var_os("CMAKE_TOOLCHAIN_FILE") {
        configure.arg(format!(
            "-DCMAKE_TOOLCHAIN_FILE={}",
            PathBuf::from(toolchain).display()
        ));
    } else if env::var("HOST").unwrap() != env::var("TARGET").unwrap() {
        panic!("set CMAKE_TOOLCHAIN_FILE for the target, or LIBIPC_LIB_DIR for a prebuilt target library");
    }
    for key in [
        "IPC_EVENT_BACKEND",
        "IPC_MESSAGE_BACKEND",
        "IPC_SHM_BACKEND",
    ] {
        if let Ok(value) = env::var(key) {
            configure.arg(format!("-D{key}={value}"));
        }
    }
    assert!(configure.status().unwrap().success());
    assert!(Command::new("cmake")
        .arg("--build")
        .arg(&out)
        .arg("--config")
        .arg("Release")
        .arg("--target")
        .args(["ipc_static", "ipc_echo"])
        .status()
        .unwrap()
        .success());
    println!("cargo:rustc-link-search=native={}", out.display());
    println!(
        "cargo:rustc-link-search=native={}",
        out.join("Release").display()
    );
    let manifest = std::fs::read_to_string(out.join("ipc-rust-link.txt")).unwrap();
    let mut libraries = manifest.lines();
    println!("cargo:rustc-link-lib=static={}", libraries.next().unwrap());
    for library in libraries.filter(|s| !s.is_empty()) {
        println!("cargo:rustc-link-lib={library}");
    }
    let executable = if env::var("CARGO_CFG_TARGET_OS").unwrap() == "windows" {
        "ipc_echo.exe"
    } else {
        "ipc_echo"
    };
    let peer = if out.join("Release").join(executable).exists() {
        out.join("Release").join(executable)
    } else {
        out.join(executable)
    };
    println!("cargo:echo_binary={}", peer.display());
}
