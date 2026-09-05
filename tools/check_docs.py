#!/usr/bin/env python3
"""Check RadRay documentation paths, headers, anchors and local Markdown links.

AGENTS.md and README.md are entry documents; project skills are also checked.
Only docs/architecture and docs/guide hold long-lived subsystem documentation.
Code may carry at most one owning-document banner. Web links and Markdown
section fragments are not validated. Run: python tools/check_docs.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path, PurePosixPath, PureWindowsPath
from urllib.parse import unquote

sys.dont_write_bytecode = True

REPO_ROOT = Path(__file__).resolve().parent.parent
DOCS_ROOT = REPO_ROOT / "docs"
DOC_SECTIONS = {"architecture", "guide"}
SKIP_DIRS = {".git", ".kimix_cache", ".opencode", ".vscode", "SDKs", "third_party", "assets", "__pycache__"}
SOURCE_SUFFIXES = {".h", ".hpp", ".cpp", ".c", ".mm", ".m", ".hlsl", ".hlsli", ".py", ".cmake", ".txt", ".json", ".md", ".yaml", ".yml"}
CODE_SUFFIXES = {".h", ".hpp", ".cpp", ".c", ".mm", ".m"}
DOC_PATH_RE = re.compile(r"(?<![/\w.-])docs/[A-Za-z0-9_/-]+(?:/[A-Za-z0-9_.-]+)*\.md")
ANCHOR_LINE_RE = re.compile(r"^>\s*-\s*锚点:\s*(.+)$", re.MULTILINE)
BACKTICK_RE = re.compile(r"`([^`]+)`")
MD_LINK_RE = re.compile(r"\[[^\]]*\]\(([^)]+)\)")
HEADER_KEYS = ("适用:", "权威:", "锚点:")
NO_ANCHOR_TOKENS = {"无", "—", "-"}


def iter_repo_files() -> list[Path]:
    out: list[Path] = []
    stack = [REPO_ROOT]
    while stack:
        current = stack.pop()
        for entry in current.iterdir():
            if entry.is_symlink():
                continue
            if entry.is_dir():
                if entry.name in SKIP_DIRS or entry.name.startswith("build"):
                    continue
                stack.append(entry)
            elif entry.suffix.lower() in SOURCE_SUFFIXES:
                out.append(entry)
    return sorted(out)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="strict")


def check_doc_references(files: list[Path], errors: list[str]) -> int:
    count = 0
    for path in files:
        for match in DOC_PATH_RE.finditer(read(path)):
            count += 1
            if not (REPO_ROOT / match.group(0)).is_file():
                errors.append(f"{path.relative_to(REPO_ROOT)}: missing doc '{match.group(0)}'")
    return count


def check_link_direction(files: list[Path], errors: list[str]) -> int:
    banner_total = 0
    for path in files:
        if path.suffix.lower() not in CODE_SUFFIXES:
            continue
        banners = [
            lineno
            for lineno, line in enumerate(read(path).splitlines(), 1)
            for _ in DOC_PATH_RE.finditer(line)
        ]
        banner_total += len(banners)
        if len(banners) > 1:
            spots = ", ".join(f"L{n}" for n in banners)
            errors.append(f"{path.relative_to(REPO_ROOT)}: {len(banners)} doc links ({spots}); keep at most one owning-document banner")
    return banner_total


def is_absolute_path(token: str) -> bool:
    return PurePosixPath(token).is_absolute() or bool(PureWindowsPath(token).drive) or bool(PureWindowsPath(token).root)


def anchor_exists(token: str) -> bool:
    if is_absolute_path(token) or ".." in PurePosixPath(token.replace("\\", "/")).parts:
        return False
    if any(ch in token for ch in "*?["):
        return any(REPO_ROOT.glob(token))
    return (REPO_ROOT / token).exists()


def check_anchors_and_headers(errors: list[str]) -> tuple[int, int]:
    for entry in sorted(DOCS_ROOT.iterdir()):
        if not entry.is_dir() or entry.name not in DOC_SECTIONS:
            errors.append(f"{entry.relative_to(REPO_ROOT)}: docs only contains architecture/ and guide/")
    docs = sorted(DOCS_ROOT.rglob("*.md"))
    anchor_count = 0
    for doc in docs:
        rel = doc.relative_to(REPO_ROOT)
        head = "\n".join(read(doc).splitlines()[:8])
        for key in HEADER_KEYS:
            if not re.search(rf"^>\s*-\s*{re.escape(key)}", head, re.MULTILINE):
                errors.append(f"{rel}: header block is missing '{key}'")
        for anchors in ANCHOR_LINE_RE.findall(head):
            if anchors.strip() in NO_ANCHOR_TOKENS:
                continue
            for raw in BACKTICK_RE.findall(anchors):
                for token in (part.strip() for part in raw.split(",")):
                    if not token:
                        continue
                    anchor_count += 1
                    if not anchor_exists(token):
                        errors.append(f"{rel}: anchor must be an existing repository-relative path: '{token}'")
    return len(docs), anchor_count


def check_internal_links(files: list[Path], errors: list[str]) -> int:
    count = 0
    for doc in files:
        if doc.suffix.lower() != ".md":
            continue
        for raw in MD_LINK_RE.findall(read(doc)):
            target = raw.strip()
            if target.startswith("<") and ">" in target:
                target = target[1:target.index(">")]
            else:
                target = target.split(' "', 1)[0].split(" '", 1)[0]
            if re.match(r"^[A-Za-z][A-Za-z0-9+.-]*:", target) or target.startswith("//"):
                continue
            target = unquote(target.split("#", 1)[0])
            if not target:
                continue
            count += 1
            resolved = (doc.parent / target).resolve()
            if is_absolute_path(target) or not resolved.is_relative_to(REPO_ROOT) or not resolved.exists():
                errors.append(f"{doc.relative_to(REPO_ROOT)}: broken or non-portable link '{target}'")
    return count


def main() -> int:
    if not DOCS_ROOT.is_dir():
        print("docs/ not found", file=sys.stderr)
        return 1
    errors: list[str] = []
    for name in ("AGENTS.md", "README.md"):
        if not (REPO_ROOT / name).is_file():
            errors.append(f"missing entry document '{name}'")
    files = iter_repo_files()
    refs = check_doc_references(files, errors)
    docs, anchors = check_anchors_and_headers(errors)
    links = check_internal_links(files, errors)
    banners = check_link_direction(files, errors)
    print(f"scanned {len(files)} files, {docs} docs, {refs} doc refs, {anchors} anchors, {links} links, {banners} code banners")
    if errors:
        print(f"\n{len(errors)} problem(s):", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
