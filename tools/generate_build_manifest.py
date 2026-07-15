#!/usr/bin/env python3
"""Generate metadata for a reproducible CI build artifact."""

from __future__ import annotations

import argparse
import json
from datetime import datetime, timezone
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--mod-version", required=True)
    parser.add_argument("--git-commit", required=True)
    parser.add_argument("--build-type", required=True)
    parser.add_argument("--ndk-version", required=True)
    parser.add_argument("--cmake-version", required=True)
    parser.add_argument("--android-api", required=True, type=int)
    parser.add_argument("--preloader-version", required=True)
    parser.add_argument("--run-id", required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    manifest = {
        "project": "Bedrock Aeronautics",
        "mod_version": args.mod_version,
        "git_commit": args.git_commit,
        "minecraft_version": "1.21.0.03",
        "minecraft_abi": "arm64-v8a",
        "android_api": args.android_api,
        "ndk_version": args.ndk_version,
        "cmake_version": args.cmake_version,
        "levilauncher_preloader_version": args.preloader_version,
        "loader_package_type": "preload-native",
        "build_type": args.build_type,
        "github_run_id": args.run_id,
        "build_timestamp_utc": datetime.now(timezone.utc).isoformat(),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
