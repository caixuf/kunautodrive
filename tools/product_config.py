#!/usr/bin/env python3
"""Read and validate deployment product metadata."""

import argparse
import json
import re
import sys
from pathlib import Path

REQUIRED = ("product_id", "display_name", "env_prefix", "install_dir",
            "package_prefix", "plugin_dir", "default_vehicle")


def load(path: Path) -> dict:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError("root must be an object")
    missing = [key for key in REQUIRED if not value.get(key)]
    if missing:
        raise ValueError(f"missing fields: {', '.join(missing)}")
    for key in ("product_id", "install_dir", "package_prefix", "default_vehicle"):
        if not re.fullmatch(r"[a-z0-9][a-z0-9_-]*", value[key]):
            raise ValueError(f"invalid {key}: {value[key]}")
    if value["plugin_dir"].startswith("/") or ".." in Path(value["plugin_dir"]).parts:
        raise ValueError(f'invalid plugin_dir: {value["plugin_dir"]}')
    if not re.fullmatch(r"[A-Z][A-Z0-9_]*", value["env_prefix"]):
        raise ValueError(f'invalid env_prefix: {value["env_prefix"]}')
    return value


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("config", type=Path)
    parser.add_argument("key", choices=REQUIRED)
    args = parser.parse_args()
    try:
        print(load(args.config)[args.key])
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"product_config: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
