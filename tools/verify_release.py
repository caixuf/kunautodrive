#!/usr/bin/env python3
"""Verify a FlowEngine release manifest before activation."""

import hashlib
import json
import platform
import sys
from pathlib import Path


def normalized_arch(value: str) -> str:
    return {"amd64": "x86_64", "aarch64": "arm64"}.get(value, value)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <release_dir>", file=sys.stderr)
        return 2
    root = Path(sys.argv[1]).resolve()
    try:
        manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
        expected_arch = normalized_arch(manifest["architecture"])
        actual_arch = normalized_arch(platform.machine())
        if expected_arch != actual_arch:
            raise ValueError(f"architecture mismatch: package={expected_arch} host={actual_arch}")
        for relative, expected in manifest["files"].items():
            path = root / relative
            if not path.is_file():
                raise ValueError(f"missing file: {relative}")
            actual = hashlib.sha256(path.read_bytes()).hexdigest()
            if actual != expected:
                raise ValueError(f"checksum mismatch: {relative}")
        print(f'OK version={manifest["version"]} files={len(manifest["files"])}')
    except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
        print(f"verify_release: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
