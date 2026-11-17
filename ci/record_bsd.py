#!/usr/bin/env python3
"""Record native Unix test results only after checking the tested source hashes."""
# SPDX-License-Identifier: LGPL-2.1-or-later
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import shlex
import subprocess


def main():
    root = Path(__file__).resolve().parent.parent
    images = json.loads((root / "ci/vm-images.json").read_text())
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", choices=images)
    parser.add_argument("port", type=int)
    parser.add_argument("--rust-tests", type=int, default=6)
    parser.add_argument("--package-tests", action="store_true")
    args = parser.parse_args()
    directory = root / "build-vm" / args.image
    ssh = ["ssh", "-i", str(directory / (args.image + "-host.id_rsa")),
           "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes",
           "-o", "StrictHostKeyChecking=yes", "-o", "UserKnownHostsFile=" + str(directory / "known_hosts"),
           "-p", str(args.port), "root@127.0.0.1"]

    def guest(command):
        command = "export PATH=/opt/ooce/bin:/opt/gcc-13/bin:/usr/pkg/bin:/usr/local/bin:$PATH; " + command
        result = subprocess.run(ssh + ["sh -c " + shlex.quote(command)],
                                capture_output=True, text=True, check=True)
        return result.stdout

    files = ["CMakeLists.txt", "LICENSE", "Cargo.toml", "Cargo.lock", "ci/unix.sh", "ci/check_package.py"]
    for folder in ("src", "include", "tests", "cmake", "examples", "benchmarks", "rust"):
        files.extend(str(p.relative_to(root)) for p in (root / folder).rglob("*")
                     if p.is_file() and p.suffix in (".c", ".h", ".cpp", ".hpp", ".rs", ".toml", ".cmake", ".in"))
    files = sorted(set(files))
    illumos = args.image.startswith("omnios-")
    if illumos:
        hash_command = "for file in " + shlex.join(files) + '; do digest -a sha256 "$file" || exit; done'
    else:
        hash_command = "sha256 -q " + shlex.join(files)
    actual = guest("cd /root/libipc && " + hash_command).splitlines()
    if len(actual) != len(files):
        raise RuntimeError("Guest source hash output is incomplete")
    hashes = {}
    for name, digest in zip(files, actual):
        expected = hashlib.sha256((root / name).read_bytes()).hexdigest()
        if expected != digest:
            raise RuntimeError("Guest tested different source: " + name)
        hashes[name] = digest

    required = ["ipc_reject_invalid_configuration", "ipc_cpp_ownership", "ipc_cpp_c_interop",
                "ipc_adversarial_framing", "ipc_crash_cleanup", "ipc_integration",
                "ipc_behavior_shared", "ipc_behavior_static"]
    counts = {}
    configurations = {}
    for build in ("build-native", "build-portable"):
        log = guest("cat /root/libipc/" + build + "/Testing/Temporary/LastTest.log")
        if log.count("Test Passed.") != len(required) or any("Test: " + name + "\n" not in log for name in required):
            raise RuntimeError("Complete passing CTest results missing: " + build)
        counts[build] = len(required)
        configurations[build] = guest("cat /root/libipc/" + build + "/ipc_config.h").strip()
    rust_counts = None
    if args.rust_tests:
        log = (directory / "rust-tests.log").read_text()
        summary = f"test result: ok. {args.rust_tests} passed; 0 failed"
        if log.count(summary) != 2:
            raise RuntimeError("Both passing Rust reactor runs are required")
        rust_counts = {"default": args.rust_tests, "explicit_poll": args.rust_tests}
    package_counts = None
    if args.package_tests:
        log = (directory / "package-tests.log").read_text()
        for metadata in ("cmake", "pkg-config"):
            for linkage in ("shared", "static"):
                message = f"Relocated {metadata} {linkage} consumer passed"
                if log.count(message) != 2:
                    raise RuntimeError("Both passing installed consumer runs are required: " + message)
        package_counts = {"build-native": 4, "build-portable": 4}
    if illumos:
        metadata_command = ("uname -a; gcc --version | head -n 1; cmake --version | head -n 1; "
                            "isainfo -kv; file /root/libipc/build-native/ipc_echo")
    else:
        metadata_command = ("uname -a; cc --version | head -n 1; cmake --version | head -n 1; "
                            "getconf LONG_BIT; sysctl -n hw.byteorder")
    metadata = guest(metadata_command).splitlines()
    if args.rust_tests:
        metadata.append(guest("rustc --version").strip())
    report = {"tested_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "platform": metadata, "ctest_pass_counts": counts,
              "configurations": configurations,
              "package_pass_counts": package_counts,
              "rust_pass_counts": rust_counts, "source_sha256": hashes}
    output = directory / "verification.json"
    output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Verified {len(hashes)} source files; CTest {counts}; Rust {rust_counts}")
    print(output)


if __name__ == "__main__":
    main()
