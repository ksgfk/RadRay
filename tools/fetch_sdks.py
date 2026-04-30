#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.dont_write_bytecode = True

from dependency_fetcher import FetchError, SdkArtifactFetcher, get_repo_root, load_manifest, split_names


def main() -> int:
    repo_root = get_repo_root()
    parser = argparse.ArgumentParser(description="Install RadRay SDK artifacts.")
    parser.add_argument(
        "--manifest",
        type=Path,
        default=repo_root / "project_manifest.json",
        help="Path to project_manifest.json.",
    )
    parser.add_argument(
        "--sdk-dir",
        type=Path,
        default=repo_root / "SDKs",
        help="Directory where SDK artifacts are installed.",
    )
    parser.add_argument(
        "--only",
        action="append",
        help="Install only the named SDK. Can be passed multiple times or as comma-separated names.",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Re-extract an SDK even when the requested version is already installed.",
    )
    args = parser.parse_args()

    try:
        manifest = load_manifest(args.manifest)
        fetcher = SdkArtifactFetcher(
            repo_root=repo_root,
            sdk_root=args.sdk_dir,
            only=split_names(args.only),
            force=args.force,
        )
        fetcher.fetch_manifest(manifest)
    except FetchError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
