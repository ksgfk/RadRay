#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.dont_write_bytecode = True

from dependency_fetcher import (
    LOCKFILE_NAME,
    FetchError,
    GitDependencyFetcher,
    get_repo_root,
    load_manifest,
    split_names,
)


def main() -> int:
    repo_root = get_repo_root()
    parser = argparse.ArgumentParser(description="Manage RadRay git third-party dependencies.")
    parser.add_argument(
        "command",
        nargs="?",
        choices=["install", "lock", "status", "prune", "refetch"],
        default="install",
        help="Command to run. Defaults to install.",
    )
    parser.add_argument(
        "--manifest",
        type=Path,
        default=repo_root / "project_manifest.json",
        help="Path to project_manifest.json.",
    )
    parser.add_argument(
        "--third-party-dir",
        type=Path,
        default=repo_root / "third_party",
        help="Directory where git dependencies are installed.",
    )
    parser.add_argument(
        "--lockfile",
        type=Path,
        default=repo_root / LOCKFILE_NAME,
        help="Path to the generated dependency lockfile.",
    )
    parser.add_argument(
        "--no-lock",
        action="store_true",
        help="Install and check status directly from the manifest instead of the lockfile.",
    )
    parser.add_argument(
        "--only",
        action="append",
        help="Operate only on the named dependency. Can be passed multiple times or as comma-separated names.",
    )
    parser.add_argument(
        "--update",
        action="store_true",
        help="Refresh mutable refs, such as Branch entries and dependencies without an explicit ref.",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Allow resetting local tracked changes when reinstalling or updating a dependency.",
    )
    args = parser.parse_args()

    try:
        manifest = load_manifest(args.manifest)
        fetcher = GitDependencyFetcher(
            repo_root=repo_root,
            target_root=args.third_party_dir,
            only=split_names(args.only),
            update=args.update,
            force=args.force,
            lockfile=args.lockfile,
            use_lock=not args.no_lock,
        )
        if args.command == "install":
            fetcher.fetch_manifest(manifest)
        elif args.command == "lock":
            fetcher.lock_manifest(manifest)
        elif args.command == "status":
            return 1 if fetcher.status_manifest(manifest) else 0
        elif args.command == "prune":
            fetcher.prune_manifest(manifest)
        elif args.command == "refetch":
            fetcher.refetch_manifest(manifest)
    except FetchError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
