#!/usr/bin/env python3
"""Run separate GTest checks, then forward sampling to Google Benchmark's native runner."""
from __future__ import annotations

import argparse
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def run_logged(command: list[str], root: Path, log: Path) -> int:
    with log.open("w", encoding="utf-8") as output:
        process = subprocess.Popen(command, cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True, encoding="utf-8", errors="replace")
        for line in process.stdout:
            output.write(line)
            print(line, end="", flush=True)
        return process.wait()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, epilog="Unrecognized --benchmark_* options are forwarded unchanged.")
    parser.add_argument("--build-dir", type=Path, default=Path("build_scene_sync_perf"))
    parser.add_argument("--config", choices=["Release", "RelWithDebInfo"], default="Release")
    parser.add_argument("--output", type=Path, required=True)
    args, forwarded = parser.parse_known_args()
    if forwarded and forwarded[0] == "--":
        forwarded.pop(0)
    if any(not value.startswith("--benchmark_") for value in forwarded):
        parser.error("Use --benchmark_option=value for native Google Benchmark options")
    if any(value.split("=", 1)[0] in ("--benchmark_out", "--benchmark_out_format", "--benchmark_list_tests") for value in forwarded):
        parser.error("This runner owns JSON output; use the executable directly to list benchmarks")

    root = Path(__file__).resolve().parents[1]
    build = args.build_dir.resolve()
    output = args.output.resolve()
    suffix = ".exe" if os.name == "nt" else ""
    executable = build / "_build" / args.config / f"bench_scene_sync{suffix}"
    correctness = build / "_build" / args.config / f"test_scene_sync{suffix}"
    if not executable.is_file() or not correctness.is_file():
        parser.error("Build bench_scene_sync and test_scene_sync before running")
    if output.exists() and any(output.iterdir()):
        parser.error(f"Output directory must be empty: {output}")
    output.mkdir(parents=True, exist_ok=True)
    command = [str(executable), *forwarded, f"--benchmark_out={output / 'results.json'}", "--benchmark_out_format=json"]
    patch = subprocess.run(["git", "diff", "HEAD", "--binary"], cwd=root, capture_output=True, check=True).stdout
    (output / "tracked.patch").write_bytes(patch)
    sources = subprocess.check_output(["git", "ls-files", "--cached", "--others", "--exclude-standard", "--",
                                       "modules", "benchmarks", "cmake", "CMakeLists.txt", "tools/run_scene_sync_benchmark.py"], cwd=root, text=True).splitlines()
    digest = hashlib.sha256()
    for name in sorted(set(sources)):
        source = root / name
        if source.is_file():
            digest.update(name.encode() + b"\0" + source.read_bytes())
    for name in subprocess.check_output(["git", "ls-files", "--others", "--exclude-standard", "--", "modules", "benchmarks"], cwd=root, text=True).splitlines():
        target = output / "untracked-source" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes((root / name).read_bytes())
    (output / "CMakeCache.txt").write_bytes((build / "CMakeCache.txt").read_bytes())
    metadata = {
        "started_at": datetime.now().astimezone().isoformat(),
        "git_head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
        "source_sha256": digest.hexdigest(),
        "binary_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
        "correctness_binary_sha256": hashlib.sha256(correctness.read_bytes()).hexdigest(),
        "command": command,
        "scope": "CPU World mutation through RT Apply and CPU retirement; no GPU work",
        "timing": "Google Benchmark real time and process CPU time; one iteration is one frame; batches of 256 drain all flights",
        "status": "running",
    }
    metadata_path = output / "metadata.json"
    metadata_path.write_text(json.dumps(metadata, ensure_ascii=False, indent=2), encoding="utf-8")
    code = run_logged([str(correctness), "--gtest_filter=SceneSyncCorrectness.*"], root, output / "correctness.log")
    if code == 0:
        code = run_logged(command, root, output / "benchmark.log")
    if code == 0:
        results_path = output / "results.json"
        if not results_path.is_file() or results_path.stat().st_size == 0:
            code = 1
        else:
            results = json.loads(results_path.read_text(encoding="utf-8"))
            rows = results.get("benchmarks", [])
            if not rows or any(row.get("error_occurred") for row in rows):
                code = 1
    metadata.update(status="passed" if code == 0 else "failed", completed_at=datetime.now().astimezone().isoformat())
    metadata_path.write_text(json.dumps(metadata, ensure_ascii=False, indent=2), encoding="utf-8")
    return code


if __name__ == "__main__":
    sys.exit(main())
