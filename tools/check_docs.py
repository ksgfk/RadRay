#!/usr/bin/env python3
"""Validate that documentation links and code anchors have not rotted.

Checks:
  1. Every ``docs/<name>.md`` path mentioned in source files, AGENTS.md or README.md exists.
  2. Every repository-local path listed on a doc's ``> - 锚点:`` line exists (glob
     patterns must match at least one file). Absolute anchors in ``docs/research/``
     identify external source checkouts and are not portable, so they are not checked.
  3. Every relative markdown link inside ``docs/`` resolves.
  4. Every durable doc outside ``docs/adr/`` carries the three-line header block; every
     ADR carries the 状态/日期/影响 block. ``docs/handoff/`` contains transient session
     snapshots and is exempt from durable-document headers.
  5. Link direction: docs reference code, not the reverse. A ``.h``/``.cpp`` file may point
     at ``docs/architecture`` or ``docs/guide`` at most once -- the banner naming its owning
     subsystem. A second one means rationale is leaking back into comments; state the
     constraint inline instead, or point at an ADR. ADR links are guardrails and are
     unrestricted. See the "Link direction" rule in AGENTS.md.

Exit code is non-zero when any check fails. Run from anywhere:

    python tools/check_docs.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path, PurePosixPath, PureWindowsPath

sys.dont_write_bytecode = True

REPO_ROOT = Path(__file__).resolve().parent.parent
DOCS_ROOT = REPO_ROOT / "docs"
HANDOFF_ROOT = DOCS_ROOT / "handoff"
RESEARCH_ROOT = DOCS_ROOT / "research"

SKIP_DIRS = {
    ".git",
    ".kimix_cache",
    ".opencode",
    ".vscode",
    "SDKs",
    "third_party",
}
SOURCE_SUFFIXES = {".h", ".hpp", ".cpp", ".c", ".mm", ".m", ".hlsl", ".hlsli", ".py", ".cmake", ".txt", ".json"}

DOC_PATH_RE = re.compile(r"docs/[A-Za-z0-9_/-]+(?:/[A-Za-z0-9_.-]+)*\.md")
CODE_SUFFIXES = {".h", ".hpp", ".cpp", ".c", ".mm", ".m"}
ANCHOR_LINE_RE = re.compile(r"^>\s*-\s*锚点:\s*(.+)$", re.MULTILINE)
BACKTICK_RE = re.compile(r"`([^`]+)`")
MD_LINK_RE = re.compile(r"\[[^\]]*\]\(([^)#]+?)(?:#[^)]*)?\)")
HEADER_KEYS = ("适用:", "权威:", "锚点:")
ADR_KEYS = ("状态:", "日期:", "影响:")
NO_ANCHOR_TOKENS = {"无", "—", "-"}


def iter_repo_files() -> list[Path]:
    out: list[Path] = []
    stack = [REPO_ROOT]
    while stack:
        current = stack.pop()
        for entry in current.iterdir():
            if entry.is_dir():
                if entry.name in SKIP_DIRS or entry.name.startswith("build_"):
                    continue
                stack.append(entry)
            elif entry.suffix.lower() in SOURCE_SUFFIXES or entry.name in {"AGENTS.md", "README.md"}:
                out.append(entry)
    return out


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def check_doc_references(files: list[Path], errors: list[str]) -> int:
    count = 0
    for path in files:
        for match in DOC_PATH_RE.finditer(read(path)):
            count += 1
            if not (REPO_ROOT / match.group(0)).exists():
                errors.append(f"{path.relative_to(REPO_ROOT)}: missing doc '{match.group(0)}'")
    return count


def check_link_direction(files: list[Path], errors: list[str]) -> int:
    """Enforce the "Link direction" rule from AGENTS.md."""
    banner_total = 0
    for path in files:
        if path.suffix.lower() not in CODE_SUFFIXES:
            continue
        banners: list[int] = []
        for lineno, line in enumerate(read(path).splitlines(), 1):
            for match in DOC_PATH_RE.finditer(line):
                if "/adr/" not in match.group(0):  # ADR guardrails are unrestricted
                    banners.append(lineno)
        banner_total += len(banners)
        if len(banners) > 1:
            spots = ", ".join(f"L{n}" for n in banners)
            errors.append(
                f"{path.relative_to(REPO_ROOT)}: {len(banners)} doc links ({spots}); keep at most one "
                f"banner -- state the constraint inline instead, or point at an ADR"
            )
    return banner_total


def anchor_exists(token: str) -> bool:
    if any(ch in token for ch in "*?["):
        return any(REPO_ROOT.glob(token))
    return (REPO_ROOT / token).exists()


def is_absolute_anchor(token: str) -> bool:
    return (
        Path(token).is_absolute()
        or PurePosixPath(token).is_absolute()
        or PureWindowsPath(token).is_absolute()
    )


def check_anchors_and_headers(errors: list[str]) -> tuple[int, int]:
    docs = sorted(DOCS_ROOT.rglob("*.md"))
    anchor_count = 0
    for doc in docs:
        text = read(doc)
        rel = doc.relative_to(REPO_ROOT)
        head = "\n".join(text.splitlines()[:8])
        is_adr_entry = doc.parent.name == "adr" and doc.name != "README.md"
        is_handoff = HANDOFF_ROOT in doc.parents
        is_research = RESEARCH_ROOT in doc.parents
        if not is_handoff:
            for key in ADR_KEYS if is_adr_entry else HEADER_KEYS:
                if key not in head:
                    errors.append(f"{rel}: header block is missing '{key}'")
        for anchors in ANCHOR_LINE_RE.findall(text):
            if anchors.strip() in NO_ANCHOR_TOKENS or anchors.startswith("无"):
                continue
            for raw in BACKTICK_RE.findall(anchors):
                for token in (part.strip() for part in raw.split(",")):
                    if not token:
                        continue
                    anchor_count += 1
                    if is_research and is_absolute_anchor(token):
                        continue
                    if not anchor_exists(token):
                        errors.append(f"{rel}: anchor path does not exist: '{token}'")
    return len(docs), anchor_count


def check_internal_links(errors: list[str]) -> int:
    count = 0
    for doc in sorted(DOCS_ROOT.rglob("*.md")):
        for target in MD_LINK_RE.findall(read(doc)):
            if "://" in target or target.startswith("mailto:"):
                continue
            count += 1
            if not (doc.parent / target).resolve().exists():
                errors.append(f"{doc.relative_to(REPO_ROOT)}: broken link '{target}'")
    return count


def main() -> int:
    if not DOCS_ROOT.is_dir():
        print("docs/ not found", file=sys.stderr)
        return 1

    errors: list[str] = []
    files = iter_repo_files()
    refs = check_doc_references(files, errors)
    docs, anchors = check_anchors_and_headers(errors)
    links = check_internal_links(errors)
    banners = check_link_direction(files, errors)

    print(
        f"scanned {len(files)} files, {docs} docs, {refs} doc refs, "
        f"{anchors} anchors, {links} links, {banners} code banners"
    )
    if errors:
        print(f"\n{len(errors)} problem(s):", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
