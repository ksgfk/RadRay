#!/usr/bin/env python3
"""Run an already-built CPU scene synchronization benchmark, sequentially."""
from __future__ import annotations

import argparse
import csv
from datetime import datetime
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys


def command_output(args: list[str], cwd: Path) -> str:
    result = subprocess.run(args, cwd=cwd, capture_output=True, text=True, encoding="utf-8", errors="replace", check=True)
    return result.stdout.strip()


def run_logged(args: list[str], env: dict[str, str], log: Path, cwd: Path) -> None:
    with log.open("w", encoding="utf-8") as output:
        process = subprocess.Popen(args, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True, encoding="utf-8", errors="replace")
        for line in process.stdout:
            output.write(line)
            output.flush()
            print(line, end="", flush=True)
        code = process.wait()
    if code:
        sys.exit(f"Benchmark failed ({code}); see {log}")


def fingerprint(root: Path) -> str:
    files = set(command_output(["git", "ls-files", "--cached", "--others", "--exclude-standard", "--", "modules", "cmake", "CMakeLists.txt", "project_manifest.json"], root).splitlines())
    files.update(["modules/runtime/tests/test_scene_sync_performance.cpp", "tools/run_scene_sync_benchmark.py"])
    digest = hashlib.sha256()
    for name in sorted(files):
        path = root / name
        if path.is_file():
            digest.update(name.encode() + b"\0" + path.read_bytes())
    return digest.hexdigest()


def metadata(root: Path, build: Path, executable: Path, args: argparse.Namespace) -> dict:
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
    options = [line for line in cache.splitlines() if not line.startswith(("#", "//")) and
               line.startswith(("CMAKE_CXX_", "CMAKE_GENERATOR", "CMAKE_MSVC_RUNTIME", "CMAKE_BUILD_TYPE", "MI_", "RADRAY_"))]
    compiler_files = sorted((build / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake"))
    compiler = compiler_files[-1].read_text(encoding="utf-8") if compiler_files else "unknown"
    dependencies = {}
    for path in sorted((root / "third_party").iterdir()):
        if (path / ".git").exists():
            dependencies[path.name] = command_output(["git", "rev-parse", "HEAD"], path)
    cpu = platform.processor()
    if sys.platform == "win32":
        cpu = command_output(["powershell", "-NoProfile", "-Command",
                              "Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors | ConvertTo-Json -Compress"], root)
    return {
        "started_at": datetime.now().astimezone().isoformat(),
        "git_head": command_output(["git", "rev-parse", "HEAD"], root),
        "git_status": command_output(["git", "status", "--short"], root),
        "source_sha256": None if args.source_snapshot else fingerprint(root),
        "source_snapshot": str(args.source_snapshot.resolve()) if args.source_snapshot else None, "binary_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
        "executable": str(executable), "platform": platform.platform(), "cpu": cpu,
        "logical_cpus": os.cpu_count(), "affinity": os.environ.get("RADRAY_BENCHMARK_AFFINITY", "OS default; no explicit pinning"),
        "build_options": options, "compiler": compiler, "dependencies": dependencies,
        "frames": args.frames, "warmup": args.warmup, "preflight": 8, "runs": args.runs,
        "cases": args.cases or "all", "allocation_pass": args.allocations,
        "scope": "CPU setters through ConsumeRenderUpdates/Apply; no GPU, command recording, visibility, draw preparation, IO or user gameplay logic",
        "completion": "CPU-only acknowledgement after Consume and scene reader release; GpuWorkCompleted=false",
        "percentiles": "median; P95/P99 nearest rank per frame; no sum of stage percentiles",
        "throughput": "frames / (last Apply end - first mutation start), includes flight reuse waits and intervening completions",
        "payload_bytes": "sum of vector element sizes; excludes capacity, container headers and shared asset data",
        "allocation_scope": "measured interval including harness, both threads merged after drain; -1 means unavailable",
        "status": "running",
    }


def summarize(prefix: Path, frame_count: int, requested: set[str], run: int) -> list[dict]:
    with Path(f"{prefix}.cases.csv").open(newline="", encoding="utf-8") as stream:
        cases = {(r["scenario"], r["mode"], r["flights"]): r for r in csv.DictReader(stream)}
    summaries = []
    payloads = {}
    configurations = {}
    with Path(f"{prefix}.frames.csv").open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        key = lambda row: (row["scenario"], row["mode"], row["flights"])
        for identity, values in itertools.groupby(reader, key=key):
            rows = list(values)
            if len(rows) != frame_count or identity not in cases:
                sys.exit(f"Missing/incomplete samples for {identity}")
            for frame, row in enumerate(rows):
                if int(row["frame"]) != frame or int(row["sequence"]) != int(rows[0]["sequence"]) + frame:
                    sys.exit(f"Frame/sequence mismatch for {identity}, frame {frame}")
                if int(row["end_ns"]) < int(row["start_ns"]) or float(row["queue_us"]) < 0:
                    sys.exit(f"Invalid timestamps for {identity}")
            payload_fields = ("transforms", "mesh_states", "creates", "removes", "light_records", "payload_bytes",
                              "local_transforms", "transform_parents", "transform_creates", "transform_removes")
            payload = tuple(tuple(r.get(k, "0") for k in payload_fields) for r in rows)
            previous = payloads.setdefault(identity[0], payload)
            if previous != payload:
                sys.exit(f"Single/multithread workloads differ: {identity}")
            configurations.setdefault(identity[0], set()).add(identity[1:])
            summary = {"run": run, **cases[identity], "samples": frame_count}
            summary["throughput_us_per_frame"] = float(summary["span_us"]) / frame_count
            for field in rows[0]:
                if not field.endswith("_us"):
                    continue
                numbers = sorted(float(r[field]) for r in rows)
                summary[f"{field}_mean"] = statistics.fmean(numbers)
                summary[f"{field}_median"] = statistics.median(numbers)
                summary[f"{field}_p95"] = numbers[math.ceil(len(numbers) * .95) - 1]
                summary[f"{field}_p99"] = numbers[math.ceil(len(numbers) * .99) - 1]
                summary[f"{field}_max"] = numbers[-1]
            for field in payload_fields:
                summary[f"{field}_mean"] = statistics.fmean(int(r.get(field, "0")) for r in rows)
                summary[f"{field}_max"] = max(int(r.get(field, "0")) for r in rows)
            summaries.append(summary)
    expected = {(mode, str(flight)) for mode in ("single", "threaded") for flight in (1, 2, 3)}
    if not configurations or any(value != expected for value in configurations.values()):
        sys.exit("Incomplete single/threaded x F=1/2/3 matrix")
    if requested and set(configurations) != requested:
        sys.exit(f"Unknown/missing requested scenarios: {requested - set(configurations)}")
    if len(summaries) != len(cases):
        sys.exit("Case and frame files disagree")
    return summaries


def write_csv(path: Path, rows: list[dict]) -> None:
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build_scene_sync_perf"))
    parser.add_argument("--source-snapshot", type=Path, help="Archived source directory for a preserved baseline binary; never fingerprint the current tree as its source")
    parser.add_argument("--config", choices=["Debug", "Release", "RelWithDebInfo"], default="Release")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--frames", type=int, default=512)
    parser.add_argument("--warmup", type=int, default=64)
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--cases", default="", help="Comma-separated exact scenario names; default: all")
    parser.add_argument("--allocations", action="store_true", help="Separate allocation run; do not mix its timing with latency runs")
    args = parser.parse_args()
    if not 1 <= args.frames <= 1000000 or not 64 <= args.warmup <= 1000000 or args.runs < 1:
        parser.error("frames must be 1..1000000; warmup 64..1000000; runs >= 1")
    root = Path(__file__).resolve().parents[1]
    build = args.build_dir.resolve()
    executable = build / "_build" / args.config / ("test_scene_sync_performance.exe" if os.name == "nt" else "test_scene_sync_performance")
    if not executable.is_file():
        parser.error(f"Build test_scene_sync_performance first: {executable}")
    output = (args.output or build / "results" / datetime.now().strftime("%Y%m%d-%H%M%S")).resolve()
    if output.exists() and any(output.iterdir()):
        parser.error(f"Output directory must be empty: {output}")
    output.mkdir(parents=True, exist_ok=True)
    info = metadata(root, build, executable, args)
    metadata_path = output / "metadata.json"
    metadata_path.write_text(json.dumps(info, ensure_ascii=False, indent=2), encoding="utf-8")
    diff = subprocess.run(["git", "diff", "HEAD", "--binary"], cwd=root, capture_output=True, check=True).stdout
    if args.source_snapshot:
        snapshot = args.source_snapshot.resolve()
        (output / "tracked.patch").write_bytes((snapshot / "benchmark-baseline.patch").read_bytes())
        (output / "source-metadata.json").write_bytes((snapshot / "metadata.json").read_bytes())
        info["source_snapshot_patch_sha256"] = hashlib.sha256((output / "tracked.patch").read_bytes()).hexdigest()
    else:
        (output / "tracked.patch").write_bytes(diff)
        for name in command_output(["git", "ls-files", "--others", "--exclude-standard", "--", "modules"], root).splitlines():
            destination = output / "untracked-source" / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes((root / name).read_bytes())
    for name in ("modules/runtime/tests/test_scene_sync_performance.cpp", "tools/run_scene_sync_benchmark.py"):
        source = args.source_snapshot / Path(name).name if args.source_snapshot and name.endswith(".cpp") else root / name
        (output / Path(name).name).write_bytes(source.read_bytes())
    env = {key: value for key, value in os.environ.items() if not key.startswith(("RADRAY_SCENE_SYNC_", "RADRAY_RUN_SCENE_SYNC_"))}
    run_logged([str(executable), "--gtest_filter=SceneSyncCorrectness.*"], env, output / "correctness.log", root)
    env.update(RADRAY_RUN_SCENE_SYNC_BENCHMARK="1", RADRAY_SCENE_SYNC_FRAMES=str(args.frames),
               RADRAY_SCENE_SYNC_WARMUP=str(args.warmup), RADRAY_SCENE_SYNC_CASES=args.cases)
    if args.allocations:
        env["RADRAY_SCENE_SYNC_ALLOCATIONS"] = "1"
        run_logged([str(executable), "--gtest_filter=SceneSyncAllocation.*"], env, output / "allocation-calibration.log", root)
    summaries = []
    for run in range(1, args.runs + 1):
        prefix = output / f"run-{run}"
        env["RADRAY_SCENE_SYNC_OUTPUT"] = str(prefix)
        run_logged([str(executable), "--gtest_filter=SceneSyncPerformance.Matrix"], env, output / f"run-{run}.log", root)
        summaries.extend(summarize(prefix, args.frames, set(args.cases.split(",")) if args.cases else set(), run))
    write_csv(output / "summary.csv", summaries)
    comparisons = []
    indexed = {(int(r["run"]), r["scenario"], r["mode"], r["flights"]): r for r in summaries}
    for row in summaries:
        if row["mode"] != "threaded":
            continue
        single = indexed[int(row["run"]), row["scenario"], "single", row["flights"]]
        comparisons.append({"run": row["run"], "scenario": row["scenario"], "flights": row["flights"],
                            "throughput_speedup": float(row["frames_per_second"]) / float(single["frames_per_second"]),
                            "single_us_per_frame": single["throughput_us_per_frame"], "threaded_us_per_frame": row["throughput_us_per_frame"],
                            "single_e2e_p50_us": single["e2e_us_median"], "threaded_e2e_p50_us": row["e2e_us_median"],
                            "single_e2e_p99_us": single["e2e_us_p99"], "threaded_e2e_p99_us": row["e2e_us_p99"]})
    write_csv(output / "comparison.csv", comparisons)
    info.update(status="passed", completed_at=datetime.now().astimezone().isoformat(), configurations=len(summaries))
    metadata_path.write_text(json.dumps(info, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"Validated {len(summaries)} configurations. Results: {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
