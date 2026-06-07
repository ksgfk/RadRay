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
    configure_text_output,
    get_repo_root,
    load_manifest,
    split_names,
)


COMMANDS = {
    "restore": "Restore third_party/ to match the manifest and lockfile.",
    "list": "Show whether installed dependencies match the manifest.",
    "upgrade": "Show available upgrades; use --apply to update and install.",
    "lock": "Regenerate project_manifest.lock.json.",
    "prune": "Remove dependencies that are no longer in the manifest.",
    "refetch": "Remove and fetch selected dependencies again.",
    "install": "Compatibility alias for restore.",
    "status": "Compatibility alias for list.",
    "check-updates": "Compatibility alias for upgrade.",
    "update": "Compatibility alias for upgrade --apply.",
}


def command_help() -> str:
    width = max(len(command) for command in COMMANDS)
    lines = ["commands:"]
    for command, description in COMMANDS.items():
        lines.append(f"  {command:<{width}}  {description}")
    return "\n".join(lines)


def main() -> int:
    configure_text_output()
    repo_root = get_repo_root()
    parser = argparse.ArgumentParser(
        description="Manage RadRay git third-party dependencies.",
        usage="%(prog)s [options] command [tag]",
        epilog=command_help(),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "command",
        nargs="?",
        help="Command to run.",
    )
    parser.add_argument(
        "tag",
        nargs="?",
        help="Release tag for upgrade --apply or update. When omitted, update uses the latest available version.",
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
        "--apply",
        action="store_true",
        help="With upgrade, actually update manifest/lockfile and install the selected package(s).",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Allow resetting local tracked changes when reinstalling or updating a dependency.",
    )
    args = parser.parse_args()
    if args.command is None:
        parser.print_help(sys.stderr)
        return 2
    if args.command not in COMMANDS:
        parser.print_usage(sys.stderr)
        print(f"error: unknown command: {args.command}", file=sys.stderr)
        print(command_help(), file=sys.stderr)
        return 2

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
        if args.command in {"restore", "install"}:
            fetcher.fetch_manifest(manifest)
        elif args.command == "lock":
            fetcher.lock_manifest(manifest)
        elif args.command in {"list", "status"}:
            return 1 if fetcher.status_manifest(manifest) else 0
        elif args.command == "prune":
            fetcher.prune_manifest(manifest)
        elif args.command == "refetch":
            fetcher.refetch_manifest(manifest)
        elif args.command == "check-updates":
            fetcher.check_release_updates_manifest(manifest)
        elif args.command == "upgrade":
            if args.apply:
                fetcher.update_release_manifest(args.manifest, manifest, args.tag)
            elif args.tag:
                raise FetchError("upgrade TAG requires --apply")
            else:
                fetcher.check_release_updates_manifest(manifest)
        elif args.command == "update":
            fetcher.update_release_manifest(args.manifest, manifest, args.tag)
    except FetchError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
