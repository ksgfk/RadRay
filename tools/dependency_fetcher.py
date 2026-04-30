#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import json
import os
import platform
import shutil
import stat
import subprocess
import sys
import time
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path
from typing import Any


LOCKFILE_NAME = "project_manifest.lock.json"


class FetchError(RuntimeError):
    pass


def get_repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def load_manifest(path: Path) -> dict[str, Any]:
    if not path.exists():
        raise FetchError(f"manifest not found: {path}")
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise FetchError(f"failed to parse manifest {path}: {exc}") from exc


def split_names(values: list[str] | None) -> set[str]:
    names: set[str] = set()
    for value in values or []:
        for part in value.split(","):
            part = part.strip()
            if part:
                names.add(part)
    return names


def note(message: str) -> None:
    print(f"[fetch] {message}")


def warn(message: str) -> None:
    print(f"[fetch] warning: {message}", file=sys.stderr)


def utc_now() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def run(args: list[str], cwd: Path | None = None, capture: bool = True) -> str:
    result = subprocess.run(
        args,
        cwd=str(cwd) if cwd else None,
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
    )
    if result.returncode != 0:
        command = " ".join(args)
        details = ""
        if capture:
            stdout = result.stdout.strip()
            stderr = result.stderr.strip()
            if stdout:
                details += f"\nstdout:\n{stdout}"
            if stderr:
                details += f"\nstderr:\n{stderr}"
        raise FetchError(f"command failed ({result.returncode}): {command}{details}")
    return result.stdout.strip() if capture else ""


def run_ok(args: list[str], cwd: Path | None = None) -> bool:
    result = subprocess.run(
        args,
        cwd=str(cwd) if cwd else None,
        text=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return result.returncode == 0


def git(cwd: Path, *args: str, capture: bool = True) -> str:
    return run(["git", *args], cwd=cwd, capture=capture)


def git_ok(cwd: Path, *args: str) -> bool:
    return run_ok(["git", *args], cwd=cwd)


def require_git() -> None:
    if shutil.which("git") is None:
        raise FetchError("git executable was not found in PATH")


def hash_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_json_if_changed(path: Path, value: dict[str, Any]) -> None:
    content = json.dumps(value, indent=2, sort_keys=True) + "\n"
    if path.exists() and path.read_text(encoding="utf-8") == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8", newline="\n")


def read_json(path: Path) -> dict[str, Any] | None:
    if not path.exists():
        return None
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return None
    return value if isinstance(value, dict) else None


def read_lockfile(path: Path) -> dict[str, Any] | None:
    lockfile = read_json(path)
    if lockfile is None:
        return None
    entries = lockfile.get("third_parties")
    if not isinstance(entries, list):
        raise FetchError(f"invalid lockfile, missing third_parties array: {path}")
    return lockfile


def lock_entries_by_name(lockfile: dict[str, Any] | None) -> dict[str, dict[str, Any]]:
    if lockfile is None:
        return {}
    entries: dict[str, dict[str, Any]] = {}
    for entry in lockfile.get("third_parties", []):
        if not isinstance(entry, dict):
            continue
        name = str(entry.get("name", "")).strip()
        if name:
            entries[name] = entry
    return entries


def safe_rmtree(path: Path, allowed_root: Path) -> None:
    resolved = path.resolve()
    root = allowed_root.resolve()
    if resolved == root or not resolved.is_relative_to(root):
        raise FetchError(f"refusing to remove path outside managed root: {resolved}")

    def remove_readonly(function: Any, child: str, _exc_info: Any) -> None:
        os.chmod(child, stat.S_IWRITE)
        function(child)

    try:
        shutil.rmtree(resolved, onexc=remove_readonly)
    except TypeError:
        shutil.rmtree(resolved, onerror=remove_readonly)


def normalize_git_url(url: str) -> str:
    normalized = url.strip().replace("\\", "/").rstrip("/")
    if normalized.lower().endswith(".git"):
        normalized = normalized[:-4]
    return normalized.lower()


def ref_from_source(source: dict[str, Any]) -> tuple[str, str]:
    for key, kind in (("Commit", "commit"), ("Tag", "tag"), ("Branch", "branch")):
        value = source.get(key)
        if value is not None and str(value).strip():
            return kind, str(value).strip()
    return "default", ""


def ref_label(source: dict[str, Any]) -> str:
    kind, value = ref_from_source(source)
    return "default" if kind == "default" else f"{kind} {value}"


def short_commit(commit: str) -> str:
    return commit[:12]


def is_full_sha(value: str) -> bool:
    return len(value) == 40 and all(char in "0123456789abcdefABCDEF" for char in value)


def manifest_git_sources(manifest: dict[str, Any], only: set[str] | None = None) -> list[dict[str, Any]]:
    parties = manifest.get("ThirdParties") or []
    if not isinstance(parties, list):
        raise FetchError("manifest field ThirdParties must be an array")

    sources: list[dict[str, Any]] = []
    seen: set[str] = set()
    for source in parties:
        if not isinstance(source, dict):
            raise FetchError("ThirdParties entries must be objects")
        name = str(source.get("Name", "")).strip()
        if not name:
            raise FetchError("ThirdParties entry is missing Name")
        if name in seen:
            raise FetchError(f"duplicate third party name: {name}")
        seen.add(name)
        if only and name not in only:
            continue

        source_type = str(source.get("Type", "")).strip()
        if source_type != "git":
            warn(f"{name}: unsupported third party type: {source_type}")
            continue
        sources.append(source)
    return sources


class GitDependencyFetcher:
    def __init__(
        self,
        repo_root: Path,
        target_root: Path,
        only: set[str] | None = None,
        update: bool = False,
        force: bool = False,
        lockfile: Path | None = None,
        use_lock: bool = True,
    ) -> None:
        self.repo_root = repo_root.resolve()
        self.target_root = target_root.resolve()
        self.only = only or set()
        self.update = update
        self.force = force
        self.lockfile = (lockfile or self.repo_root / LOCKFILE_NAME).resolve()
        self.use_lock = use_lock
        self.lock_data = read_lockfile(self.lockfile) if self.use_lock and self.lockfile.exists() else None
        self.lock_entries = lock_entries_by_name(self.lock_data)

    def fetch_manifest(self, manifest: dict[str, Any]) -> None:
        require_git()
        for source in manifest_git_sources(manifest, self.only):
            self.fetch_one(self.source_for_install(source))

    def lock_manifest(self, manifest: dict[str, Any]) -> None:
        require_git()
        sources = manifest_git_sources(manifest)
        selected_names = self.only or {str(source["Name"]) for source in sources}
        old_entries = lock_entries_by_name(read_lockfile(self.lockfile) if self.lockfile.exists() else None)

        entries: list[dict[str, Any]] = []
        for source in sources:
            name = str(source["Name"])
            if name in selected_names or name not in old_entries:
                note(f"{name}: locking {ref_label(source)}")
                entries.append(self.lock_entry_for_source(source))
            else:
                entries.append(old_entries[name])

        write_json_if_changed(
            self.lockfile,
            {
                "schema_version": 1,
                "manager": "radray-fetch",
                "generated_at_utc": utc_now(),
                "third_parties": entries,
            },
        )
        note(f"lockfile written: {self.lockfile}")

    def status_manifest(self, manifest: dict[str, Any]) -> int:
        problem_count = 0
        sources = manifest_git_sources(manifest, self.only)
        if not sources:
            note("no git third-party dependencies matched")
            return 0

        for source in sources:
            status, detail, problem = self.status_one(source)
            if problem:
                problem_count += 1
            print(f"{str(source['Name']):24} {status:14} {detail}")
        return problem_count

    def prune_manifest(self, manifest: dict[str, Any]) -> None:
        manifest_names = {str(source["Name"]) for source in manifest_git_sources(manifest)}
        if not self.target_root.exists():
            note(f"third-party directory does not exist: {self.target_root}")
            return

        for child in sorted(self.target_root.iterdir(), key=lambda path: path.name.lower()):
            if child.name == ".radray-state":
                continue
            if child.name in manifest_names:
                continue
            if self.only and child.name not in self.only:
                continue
            self.prune_path(child)

        state_dir = self.target_root / ".radray-state"
        if state_dir.exists():
            for state_file in sorted(state_dir.glob("*.json")):
                if state_file.stem not in manifest_names:
                    state_file.unlink()
                    note(f"pruned stale state: {state_file.name}")

    def refetch_manifest(self, manifest: dict[str, Any]) -> None:
        require_git()
        for source in manifest_git_sources(manifest, self.only):
            name = str(source["Name"])
            target = self.target_root / name
            if target.exists():
                note(f"{name}: removing existing checkout")
                safe_rmtree(target, self.target_root)
            self.fetch_one(self.source_for_install(source))

    def fetch_one(self, source: dict[str, Any]) -> None:
        name = str(source["Name"])
        repo_url = str(source.get("Git", "")).strip()
        if not repo_url:
            raise FetchError(f"{name}: missing Git URL")

        self.target_root.mkdir(parents=True, exist_ok=True)
        target = self.target_root / name
        note(f"{name}: resolving {ref_label(source)}")

        if target.exists():
            self.validate_existing_repo(target, repo_url, name)
            satisfied = self.is_satisfied(target, source)
            if satisfied and (not self.update or not self.is_mutable_ref(source)):
                self.write_state(target, source)
                note(f"{name}: already installed at requested version")
                return
            self.ensure_can_change(target, name)
            self.checkout_ref(target, source, existing=True)
        else:
            note(f"{name}: cloning {repo_url}")
            run(["git", "clone", repo_url, str(target)], capture=False)
            self.checkout_ref(target, source, existing=False)

        self.apply_patch_if_needed(target, source)
        self.write_state(target, source)
        note(f"{name}: installed {ref_label(source)}")

    def source_for_install(self, source: dict[str, Any]) -> dict[str, Any]:
        if not self.use_lock or not self.lock_data:
            return source
        name = str(source["Name"])
        entry = self.lock_entries.get(name)
        if entry is None:
            raise FetchError(f"{name}: missing from lockfile, run the lock command or use --no-lock")

        manifest_git = normalize_git_url(str(source.get("Git", "")))
        locked_git = normalize_git_url(str(entry.get("git", "")))
        if manifest_git != locked_git:
            raise FetchError(
                f"{name}: lockfile remote does not match manifest\n"
                f"  manifest: {source.get('Git', '')}\n"
                f"  lockfile: {entry.get('git', '')}"
            )

        commit = str(entry.get("commit", "")).strip()
        if not commit:
            raise FetchError(f"{name}: lockfile entry is missing commit")

        locked_source = dict(source)
        locked_source.pop("Tag", None)
        locked_source.pop("Branch", None)
        locked_source["Commit"] = commit
        return locked_source

    def lock_entry_for_source(self, source: dict[str, Any]) -> dict[str, Any]:
        spec = self.desired_state_spec(source)
        spec["commit"] = self.resolve_source_commit(source)
        return spec

    def resolve_source_commit(self, source: dict[str, Any]) -> str:
        name = str(source["Name"])
        repo_url = str(source.get("Git", "")).strip()
        if not repo_url:
            raise FetchError(f"{name}: missing Git URL")

        kind, value = ref_from_source(source)
        if kind == "commit":
            if is_full_sha(value):
                return value.lower()
            target = self.target_root / name
            if target.exists() and git_ok(target, "rev-parse", "--is-inside-work-tree"):
                self.validate_existing_repo(target, repo_url, name)
                if not git_ok(target, "rev-parse", "--verify", f"{value}^{{commit}}"):
                    self.fetch_all(target)
                return git(target, "rev-parse", f"{value}^{{commit}}")
            raise FetchError(f"{name}: commit lock requires a full 40-character SHA or an installed repo")
        if kind == "tag":
            return self.resolve_remote_tag(repo_url, value, name)
        if kind == "branch":
            return self.resolve_remote_branch(repo_url, value, name)
        return self.resolve_remote_head(repo_url, name)

    def status_one(self, source: dict[str, Any]) -> tuple[str, str, bool]:
        name = str(source["Name"])
        target = self.target_root / name
        if not target.exists():
            return "missing", f"{ref_label(source)} is not installed", True
        if not git_ok(target, "rev-parse", "--is-inside-work-tree"):
            return "invalid", f"target is not a git repository: {target}", True

        try:
            self.validate_existing_repo(target, str(source.get("Git", "")), name)
        except FetchError as exc:
            return "remote-mismatch", str(exc).replace("\n", "; "), True

        install_source = self.source_for_install(source)
        current = self.current_head(target)
        kind, value = ref_from_source(install_source)
        detail = f"{ref_label(source)} at {short_commit(current)}"
        problem = False
        status = "ok"

        if kind == "commit":
            expected = value.lower()
            if not current.lower().startswith(expected) and not expected.startswith(current.lower()):
                status = "mismatch"
                detail += f", expected {short_commit(expected)}"
                problem = True
        elif kind == "tag":
            tag_commit = self.try_tag_commit(target, value)
            if tag_commit is None:
                status = "tag-missing"
                detail += f", local tag {value} is missing"
                problem = True
            elif tag_commit != current:
                status = "mismatch"
                detail += f", expected {short_commit(tag_commit)}"
                problem = True
        elif kind == "branch":
            branch = git(target, "branch", "--show-current")
            if branch != value:
                status = "mismatch"
                detail += f", expected branch {value}"
                problem = True

        state = read_json(self.state_path(target))
        desired = self.desired_state_spec(install_source)
        if not self.patch_is_satisfied(target, install_source, state, current, desired):
            status = "patch-missing"
            detail += ", patch is not applied"
            problem = True

        dirty = self.tracked_status(target)
        if dirty:
            detail += ", tracked files modified"
            if not problem:
                status = "dirty"
            problem = True
        return status, detail, problem

    def prune_path(self, path: Path) -> None:
        if not path.is_dir():
            if self.force:
                path.unlink()
                note(f"pruned file: {path.name}")
            else:
                warn(f"{path.name}: not a directory, use --force to remove")
            return

        if git_ok(path, "rev-parse", "--is-inside-work-tree"):
            status = self.tracked_status(path)
            if status and not self.force:
                raise FetchError(f"{path.name}: local tracked changes prevent pruning, rerun with --force")
        elif not self.force:
            raise FetchError(f"{path.name}: unmanaged directory, rerun prune with --force to remove it")

        safe_rmtree(path, self.target_root)
        note(f"pruned dependency: {path.name}")

    def validate_existing_repo(self, target: Path, repo_url: str, name: str) -> None:
        if not git_ok(target, "rev-parse", "--is-inside-work-tree"):
            raise FetchError(f"{name}: target exists but is not a git repository: {target}")
        current_url = git(target, "config", "--get", "remote.origin.url")
        if normalize_git_url(current_url) != normalize_git_url(repo_url):
            raise FetchError(
                f"{name}: remote URL mismatch\n"
                f"  manifest: {repo_url}\n"
                f"  existing: {current_url}"
            )

    def is_satisfied(self, target: Path, source: dict[str, Any]) -> bool:
        state = read_json(self.state_path(target))
        desired = self.desired_state_spec(source)
        head = self.current_head(target)
        if state and state.get("spec") == desired and state.get("commit") == head:
            return True

        kind, value = ref_from_source(source)
        if kind == "commit":
            expected = value.lower()
            same_ref = head.lower().startswith(expected) or expected.startswith(head.lower())
        elif kind == "tag":
            tag_commit = self.try_tag_commit(target, value)
            same_ref = tag_commit is not None and tag_commit == head
        elif kind == "branch":
            same_ref = git(target, "branch", "--show-current") == value
        else:
            same_ref = True

        if not same_ref:
            return False
        if not self.patch_is_satisfied(target, source, state, head, desired):
            return False

        status = self.tracked_status(target)
        if status and not source.get("Patch"):
            warn(f"{source['Name']}: requested version is present but tracked files are modified")
        return True

    def patch_is_satisfied(
        self,
        target: Path,
        source: dict[str, Any],
        state: dict[str, Any] | None,
        head: str,
        desired: dict[str, Any],
    ) -> bool:
        patch_value = source.get("Patch")
        if not patch_value:
            return True
        if state and state.get("spec") == desired and state.get("commit") == head:
            return True
        patch_path = self.repo_root / str(patch_value)
        if not patch_path.exists():
            raise FetchError(f"{source['Name']}: patch file not found: {patch_path}")
        return git_ok(target, "apply", "--reverse", "--check", str(patch_path))

    def ensure_can_change(self, target: Path, name: str) -> None:
        status = self.tracked_status(target)
        if not status:
            return
        if not self.force:
            raise FetchError(
                f"{name}: local tracked changes would be overwritten. "
                "Commit/remove them or rerun with --force."
            )
        git(target, "reset", "--hard", capture=False)

    def fetch_all(self, target: Path) -> None:
        git(target, "fetch", "--all", "--tags", "--prune", capture=False)

    def ls_remote(self, repo_url: str, *patterns: str) -> str:
        return run(["git", "ls-remote", repo_url, *patterns])

    def resolve_remote_tag(self, repo_url: str, tag: str, name: str) -> str:
        output = self.ls_remote(repo_url, f"refs/tags/{tag}", f"refs/tags/{tag}^{{}}")
        direct: str | None = None
        peeled: str | None = None
        for line in output.splitlines():
            commit, ref = line.split("\t", 1)
            if ref == f"refs/tags/{tag}^{{}}":
                peeled = commit
            elif ref == f"refs/tags/{tag}":
                direct = commit
        commit = peeled or direct
        if commit is None:
            raise FetchError(f"{name}: remote tag not found: {tag}")
        return commit

    def resolve_remote_branch(self, repo_url: str, branch: str, name: str) -> str:
        output = self.ls_remote(repo_url, f"refs/heads/{branch}")
        for line in output.splitlines():
            commit, ref = line.split("\t", 1)
            if ref == f"refs/heads/{branch}":
                return commit
        raise FetchError(f"{name}: remote branch not found: {branch}")

    def resolve_remote_head(self, repo_url: str, name: str) -> str:
        output = self.ls_remote(repo_url, "HEAD")
        for line in output.splitlines():
            commit, ref = line.split("\t", 1)
            if ref == "HEAD":
                return commit
        raise FetchError(f"{name}: remote HEAD not found")

    def checkout_ref(self, target: Path, source: dict[str, Any], existing: bool) -> None:
        kind, value = ref_from_source(source)
        if kind == "commit":
            if existing:
                self.fetch_all(target)
            git(target, "checkout", "--detach", value, capture=False)
        elif kind == "tag":
            if existing:
                self.fetch_all(target)
            git(target, "checkout", "--detach", f"tags/{value}", capture=False)
        elif kind == "branch":
            self.fetch_all(target)
            remote_ref = f"origin/{value}"
            if git_ok(target, "rev-parse", "--verify", remote_ref):
                git(target, "checkout", "-B", value, remote_ref, capture=False)
            else:
                git(target, "checkout", value, capture=False)
        elif existing and self.update:
            git(target, "pull", "--ff-only", capture=False)

    def apply_patch_if_needed(self, target: Path, source: dict[str, Any]) -> None:
        patch_value = source.get("Patch")
        if not patch_value:
            return
        patch_path = self.repo_root / str(patch_value)
        if not patch_path.exists():
            raise FetchError(f"{source['Name']}: patch file not found: {patch_path}")
        git(target, "apply", str(patch_path), capture=False)

    def desired_state_spec(self, source: dict[str, Any]) -> dict[str, Any]:
        kind, value = ref_from_source(source)
        spec: dict[str, Any] = {
            "name": str(source["Name"]),
            "type": "git",
            "git": normalize_git_url(str(source["Git"])),
            "ref": {"kind": kind, "value": value},
        }
        patch_value = source.get("Patch")
        if patch_value:
            patch_path = self.repo_root / str(patch_value)
            spec["patch"] = {
                "path": str(patch_value).replace("\\", "/"),
                "sha256": hash_file(patch_path) if patch_path.exists() else None,
            }
        return spec

    def write_state(self, target: Path, source: dict[str, Any]) -> None:
        state_path = self.state_path(target)
        old_state = read_json(state_path)
        spec = self.desired_state_spec(source)
        commit = self.current_head(target)
        if old_state and old_state.get("spec") == spec and old_state.get("commit") == commit:
            return
        write_json_if_changed(
            state_path,
            {
                "schema_version": 1,
                "manager": "radray-fetch",
                "spec": spec,
                "commit": commit,
                "installed_at_utc": utc_now(),
            },
        )

    def state_path(self, target: Path) -> Path:
        return self.target_root / ".radray-state" / f"{target.name}.json"

    def current_head(self, target: Path) -> str:
        return git(target, "rev-parse", "HEAD")

    def tracked_status(self, target: Path) -> str:
        return git(target, "status", "--porcelain", "--untracked-files=no")

    def try_tag_commit(self, target: Path, tag: str) -> str | None:
        ref = f"refs/tags/{tag}^{{commit}}"
        if not git_ok(target, "rev-parse", "--verify", ref):
            return None
        return git(target, "rev-parse", ref)

    def is_mutable_ref(self, source: dict[str, Any]) -> bool:
        kind, _ = ref_from_source(source)
        return kind in {"branch", "default"}


class SdkArtifactFetcher:
    def __init__(
        self,
        repo_root: Path,
        sdk_root: Path,
        only: set[str] | None = None,
        force: bool = False,
    ) -> None:
        self.repo_root = repo_root.resolve()
        self.sdk_root = sdk_root.resolve()
        self.only = only or set()
        self.force = force

    def fetch_manifest(self, manifest: dict[str, Any]) -> None:
        artifacts = manifest.get("Artifacts") or []
        if not isinstance(artifacts, list):
            raise FetchError("manifest field Artifacts must be an array")

        current_platform, current_arch = current_triplet()
        note(f"host triplet: {current_platform}-{current_arch}")
        seen: set[str] = set()

        for artifact in artifacts:
            if not isinstance(artifact, dict):
                raise FetchError("Artifacts entries must be objects")
            name = str(artifact.get("Name", "")).strip()
            if not name:
                raise FetchError("Artifacts entry is missing Name")
            if self.only and name not in self.only:
                continue

            info = self.match_artifact(artifact, current_platform, current_arch)
            if info is None:
                note(f"{name}: no artifact for {current_platform}-{current_arch}, skipping")
                continue
            if name in seen:
                raise FetchError(f"duplicate SDK artifact name for current triplet: {name}")
            seen.add(name)
            self.fetch_one(info)

    def match_artifact(
        self,
        artifact: dict[str, Any],
        current_platform: str,
        current_arch: str,
    ) -> dict[str, Any] | None:
        triplets = artifact.get("Triplets")
        if not isinstance(triplets, list) or not triplets:
            raise FetchError(f"{artifact.get('Name', '<unknown>')}: missing Triplets")

        outer = {key: value for key, value in artifact.items() if key != "Triplets"}
        for triplet in triplets:
            if not isinstance(triplet, dict):
                continue
            if triplet.get("Platform") == current_platform and triplet.get("Arch") == current_arch:
                merged = dict(outer)
                merged.update(triplet)
                return merged
        return None

    def fetch_one(self, info: dict[str, Any]) -> None:
        name = str(info["Name"])
        version = str(info.get("Version", "")).strip()
        if not version:
            raise FetchError(f"{name}: missing Version")

        url = expand_url(str(info.get("Url", "")), info)
        if not url:
            raise FetchError(f"{name}: missing Url")
        archive_name = str(info.get("ArchiveName") or archive_name_from_url(url))
        expected_hash = str(info.get("Hash") or "").strip().lower()
        if info.get("EnforceHash") and not expected_hash:
            raise FetchError(f"{name}: EnforceHash is true but Hash is missing")

        version_root = self.sdk_root / name / f"v{version}"
        archive_path = version_root / archive_name
        extract_dir = version_root / "extracted"
        stamp_file = version_root / ".done"

        note(f"{name}: resolving v{version}")
        if not self.force and stamp_file.exists() and extract_dir.exists():
            if archive_path.exists() and expected_hash:
                self.verify_hash(archive_path, expected_hash)
            self.validate_files(extract_dir, info)
            self.write_state(version_root, info, archive_name, url)
            note(f"{name}: already installed at requested version")
            return

        version_root.mkdir(parents=True, exist_ok=True)
        if archive_path.exists():
            if expected_hash:
                self.verify_hash(archive_path, expected_hash)
            note(f"{name}: archive already downloaded")
        else:
            note(f"{name}: downloading {url}")
            download_file(url, archive_path)
            if expected_hash:
                self.verify_hash(archive_path, expected_hash)

        if extract_dir.exists():
            safe_rmtree(extract_dir, version_root)
        extract_dir.mkdir(parents=True, exist_ok=True)
        extract_archive(archive_path, extract_dir)
        self.validate_files(extract_dir, info)
        stamp_file.touch()
        self.write_state(version_root, info, archive_name, url)
        note(f"{name}: installed v{version}")

    def verify_hash(self, path: Path, expected_hash: str) -> None:
        actual_hash = hash_file(path).lower()
        if actual_hash != expected_hash.lower():
            raise FetchError(
                f"hash mismatch for {path}\n"
                f"  expected: {expected_hash}\n"
                f"  actual:   {actual_hash}"
            )

    def validate_files(self, extract_dir: Path, info: dict[str, Any]) -> None:
        validation_files = info.get("ValidationFiles") or []
        for value in validation_files:
            path = extract_dir / str(value)
            if not path.exists():
                raise FetchError(f"{info['Name']}: extracted package is missing {value}")

    def write_state(self, version_root: Path, info: dict[str, Any], archive_name: str, url: str) -> None:
        state = {
            "schema_version": 1,
            "manager": "radray-fetch",
            "name": str(info["Name"]),
            "version": str(info["Version"]),
            "platform": str(info.get("Platform", "")),
            "arch": str(info.get("Arch", "")),
            "url": url,
            "archive_name": archive_name,
            "hash": str(info.get("Hash") or "").lower(),
        }
        write_json_if_changed(version_root / ".radray-sdk.json", state)


def current_triplet() -> tuple[str, str]:
    if sys.platform.startswith("win"):
        host_platform = "windows"
    elif sys.platform == "darwin":
        host_platform = "macos"
    elif sys.platform.startswith("linux"):
        host_platform = "linux"
    else:
        host_platform = "unknown"

    machine = platform.machine().lower()
    arch_map = {
        "amd64": "x64",
        "x86_64": "x64",
        "arm64": "arm64",
        "aarch64": "arm64",
        "x86": "x86",
        "i386": "x86",
        "i686": "x86",
    }
    return host_platform, arch_map.get(machine, "unknown")


def expand_url(template: str, values: dict[str, Any]) -> str:
    url = template
    for key, value in values.items():
        url = url.replace(f"{{{key}}}", str(value))
    return url


def archive_name_from_url(url: str) -> str:
    parsed = urllib.parse.urlparse(url)
    name = Path(urllib.parse.unquote(parsed.path)).name
    if not name:
        raise FetchError(f"cannot infer archive name from URL: {url}")
    return name


def download_file(url: str, output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    with urllib.request.urlopen(url) as response:
        with output.open("wb") as file:
            shutil.copyfileobj(response, file)


def extract_archive(archive_path: Path, extract_dir: Path) -> None:
    if archive_path.suffix.lower() != ".zip":
        raise FetchError(f"unsupported archive type: {archive_path}")
    with zipfile.ZipFile(archive_path) as archive:
        archive.extractall(extract_dir)
