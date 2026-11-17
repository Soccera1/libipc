#!/usr/bin/env python3
"""Fetch pinned native-test fixtures; never executes an upstream launch script.

The SSH keys are deliberately published VM test fixtures, not user credentials.
Downloaded archives and keys stay in the ignored build-vm directory.
"""
# SPDX-License-Identifier: LGPL-2.1-or-later
import argparse
import hashlib
import json
from pathlib import Path
import time
import urllib.error
import urllib.request


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def fetch(url, path, size, expected):
    if path.exists():
        if path.stat().st_size != size or sha256(path) != expected:
            raise RuntimeError(f"Existing artifact differs from pinned digest: {path}")
        if path.name.endswith("id_rsa"):
            path.chmod(0o600)
        print(f"{path.name}: already verified", flush=True)
        return
    partial = path.with_suffix(path.suffix + ".partial")
    for attempt in range(4):
        start = partial.stat().st_size if partial.exists() else 0
        if start == size:
            break
        if start > size:
            raise RuntimeError(f"Oversized partial artifact: {partial}")
        print(f"{path.name}: fetching from byte {start}", flush=True)
        request = urllib.request.Request(
            url + "?libipc_resume=" + str(time.time_ns()),
            headers={"Range": f"bytes={start}-"},
        )
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                if start and (
                    response.status != 206
                    or not response.headers.get("Content-Range", "").startswith(f"bytes {start}-")
                ):
                    raise RuntimeError("Server did not honor the resume range")
                with partial.open("ab") as output:
                    milestone = start // (64 * 1024 * 1024)
                    while block := response.read(1024 * 1024):
                        output.write(block)
                        if output.tell() > size:
                            raise RuntimeError("Artifact exceeds pinned size")
                        current = output.tell() // (64 * 1024 * 1024)
                        if current > milestone:
                            milestone = current
                            print(f"{path.name}: {output.tell() // (1024 * 1024)} MiB", flush=True)
            if partial.stat().st_size == size:
                break
        except (OSError, urllib.error.URLError) as error:
            print(f"Transfer attempt {attempt + 1}: {error}", flush=True)
            time.sleep(1)
    if not partial.exists() or partial.stat().st_size != size or sha256(partial) != expected:
        raise RuntimeError(f"Artifact incomplete or digest mismatch: {partial}")
    partial.replace(path)
    if path.name.endswith("id_rsa"):
        path.chmod(0o600)
    print(f"{path.name}: SHA256 verified", flush=True)


def main():
    repository = Path(__file__).resolve().parent.parent
    images = json.loads((repository / "ci/vm-images.json").read_text())
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", choices=images)
    arguments = parser.parse_args()
    image = images[arguments.image]
    root = repository / "build-vm" / arguments.image
    root.mkdir(parents=True, mode=0o700, exist_ok=True)
    base = f"https://github.com/{image['repository']}/releases/download/{image['tag']}/"
    filename = arguments.image + ".qcow2.zst"
    fetch(base + filename, root / filename, image["bytes"], image["sha256"])
    filename = arguments.image + "-host.id_rsa"
    fetch(base + filename, root / filename, 411, image["key_sha256"])


if __name__ == "__main__":
    main()
