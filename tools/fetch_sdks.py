#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.dont_write_bytecode = True

from dependency_fetcher import (
    FetchError,
    SdkArtifactFetcher,
    configure_text_output,
    get_repo_root,
    load_manifest,
    split_names,
)


COMMANDS = {
    "restore": "Restore SDKs/ to match the manifest.",
    "list": "Show whether installed SDK artifacts match the manifest.",
    "upgrade": "Show available upgrades; use --apply to update and install.",
    "install": "Compatibility alias for restore.",
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
        description="Install RadRay SDK artifacts.",
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
        help="Release tag for upgrade --apply or update. When omitted, update uses the latest GitHub release.",
    )
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
    parser.add_argument(
        "--apply",
        action="store_true",
        help="With upgrade, actually update manifest/hash values and install the selected SDK(s).",
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
        fetcher = SdkArtifactFetcher(
            repo_root=repo_root,
            sdk_root=args.sdk_dir,
            only=split_names(args.only),
            force=args.force,
        )
        if args.command in {"restore", "install"}:
            fetcher.fetch_manifest(manifest)
        elif args.command == "list":
            return 1 if fetcher.status_manifest(manifest) else 0
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
