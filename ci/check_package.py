#!/usr/bin/env python3
"""Install, relocate, build and run native CMake and pkg-config consumers."""
# SPDX-License-Identifier: LGPL-2.1-or-later
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("--config")
    parser.add_argument("--generator", help="CMake generator for downstream consumers")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    build = args.build.resolve()
    if args.config is None:
        args.config = ""
        for line in (build / "CMakeCache.txt").read_text().splitlines():
            if line.startswith("CMAKE_BUILD_TYPE:"):
                args.config = line.split("=", 1)[1]
            elif line.startswith("CMAKE_CONFIGURATION_TYPES:"):
                args.config = "Release"

    def run(*command, env=None):
        subprocess.run([str(arg) for arg in command], env=env, check=True)

    def verify_linkage(executable, linkage, environment):
        if os.name == "nt":
            return
        inspector = ["otool", "-L"] if sys.platform == "darwin" else ["ldd"]
        result = subprocess.run([*inspector, str(executable)], env=environment,
                                capture_output=True, text=True, check=False)
        dependencies = result.stdout + result.stderr
        dynamic = re.search(r"libipc(?:\.[0-9]+)*\.(?:so|sl|dylib|a)\b", dependencies)
        if bool(dynamic) != (linkage == "shared") or (linkage == "shared" and result.returncode):
            raise RuntimeError(f"Expected {linkage} libipc linkage: {dependencies}")

    with tempfile.TemporaryDirectory(prefix="package-check-", dir=build) as directory:
        temporary = Path(directory)
        prefix = temporary / "original"
        run("cmake", "--install", build, "--config", args.config, "--prefix", prefix)
        relocated = temporary / "relocated prefix"
        prefix.rename(relocated)
        packages = list(relocated.rglob("libipc.pc"))
        if len(packages) != 1:
            raise RuntimeError("Expected exactly one installed pkg-config directory")
        environment = os.environ.copy()
        # Use only this installation's metadata, independent of the host search path.
        environment["PKG_CONFIG_LIBDIR"] = str(packages[0].parent)
        environment["PKG_CONFIG_PATH"] = ""
        for metadata in ("cmake", "pkg-config"):
            for linkage in ("shared", "static"):
                consumer = temporary / (metadata + "-" + linkage)
                generator = ["-G", args.generator] if args.generator else []
                run("cmake", "-S", root / "tests/consumer", "-B", consumer,
                    *generator,
                    "-DCMAKE_BUILD_TYPE=" + args.config,
                    "-DCMAKE_PREFIX_PATH=" + str(relocated),
                    "-DUSE_STATIC=" + ("ON" if linkage == "static" else "OFF"),
                    "-DUSE_PKG_CONFIG=" + ("ON" if metadata == "pkg-config" else "OFF"),
                    env=environment)
                run("cmake", "--build", consumer, "--config", args.config, env=environment)
                executable = consumer / "consumer"
                if os.name == "nt":
                    executable = consumer / "consumer.exe"
                    if not executable.exists():
                        executable = consumer / args.config / "consumer.exe"
                    environment["PATH"] = str(relocated / "bin") + os.pathsep + environment["PATH"]
                run(executable, env=environment)
                verify_linkage(executable, linkage, environment)
                print(f"Relocated {metadata} {linkage} consumer passed", flush=True)


if __name__ == "__main__":
    main()
