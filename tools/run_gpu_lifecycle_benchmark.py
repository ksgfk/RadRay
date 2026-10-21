"""Serial paired comparison or complete case coverage of the lifecycle benchmarks."""
import argparse
import csv
import hashlib
import json
import math
import pathlib
import platform
import statistics
import subprocess
import time


def workloads():
    return {
        "bench_material_scene": r"^MaterialScene/(Churn|UniqueAssetChurn)/|^MaterialScene/(d3d12|vulkan)/(PrepareRecord|FullFrame)/N(1000|10000|100000)/M(1|100)/V3/dirty(0|1)/real_time$|^MaterialScene/(Direct|Manual|ManualTint)/(d3d12|vulkan)/FullFrame/N10000/M1/V3/dirty1/real_time$",
        "bench_scene_gpu": r"^SceneGpu/(d3d12|vulkan)/(PrepareAndRecord|SubmitFrame)/F(1|3)/(changed:(0|1|10000)|parent|churn|first_use|growth)/views:(1|3)/real_time$",
        "bench_command_allocator": r"^CommandAllocator/",
        "bench_raster_prepare": r".*",
        "bench_scene_sync": r"^SceneSync/shape_(1000|10000|100000)_dirty_(0|1|1000|10000|100000)/F(1|3)/(single|threaded)/",
        "bench_lifecycle": r"^Lifecycle/DeleteAndSync/count:(10000|100000)/depth:1/unique:false/removed:(1|100)$|^Lifecycle/Frame/count:(10000|100000)/depth:1/unique:false/F(1|3)/single/changes:(0|1|100)/views:(1|3)/",
    }


def summarize(output):
    samples = {}
    failures = []
    for path in sorted(output.glob("comparison-*/*.json")):
        if path.name.endswith("invocation.json"):
            continue
        tag = path.parent.name.split("-")[-1]
        pair = int(path.parent.name.split("-")[1])
        data = json.loads(path.read_text(encoding="utf-8"))
        for entry in data.get("benchmarks", []):
            name = entry.get("run_name", entry["name"])
            if entry.get("error_occurred"):
                failures.append({"file": str(path), "name": name, "error": entry.get("error_message", "")})
                continue
            if entry.get("run_type") != "iteration":
                continue
            factor = {"ns": 0.001, "us": 1, "ms": 1000, "s": 1000000}[entry["time_unit"]]
            row = {"real_us": entry["real_time"] * factor, "cpu_us": entry["cpu_time"] * factor}
            for counter in ("gpu_ms/frame", "host_bytes/frame", "upload_bytes/frame", "submit_us/frame", "reset_us", "record_us", "native_capacity", "scene_buffer_bytes", "native_block_bytes", "native_allocation_bytes", "native_block_count", "native_allocation_count", "native_allocation_bytes_pending"):
                if counter in entry:
                    row[counter] = entry[counter]
            samples.setdefault(name, {}).setdefault(tag, {})[pair] = row
    with (output / "paired-summary.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["benchmark", "metric", "pairs", "baseline_median", "final_median", "paired_ratio_mean", "paired_ratio_ci95_low", "paired_ratio_ci95_high"])
        for name, tags in sorted(samples.items()):
            if "baseline" not in tags or "final" not in tags:
                continue
            pairs = sorted(tags["baseline"].keys() & tags["final"].keys())
            if not pairs:
                continue
            metrics = tags["baseline"][pairs[0]].keys() & tags["final"][pairs[0]].keys()
            for metric in sorted(metrics):
                before = [tags["baseline"][pair][metric] for pair in pairs]
                after = [tags["final"][pair][metric] for pair in pairs]
                ratios = [b / a for a, b in zip(before, after) if a > 0]
                mean = statistics.mean(ratios) if ratios else 0
                # Two-sided Student t for the default three independent pairs.
                critical = {2: 12.706, 3: 4.303, 4: 3.182, 5: 2.776, 6: 2.571}.get(len(ratios), 2.447 if len(ratios) > 6 else 0)
                margin = critical * statistics.stdev(ratios) / math.sqrt(len(ratios)) if len(ratios) > 1 else 0
                writer.writerow([name, metric, len(pairs), statistics.median(before), statistics.median(after), mean, mean - margin, mean + margin])
    (output / "comparison-errors.json").write_text(json.dumps(failures, indent=2), encoding="utf-8")


def percentile(values, fraction):
    ordered = sorted(values)
    location = (len(ordered) - 1) * fraction
    lower = math.floor(location)
    upper = math.ceil(location)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (location - lower)


def summarize_traces(output):
    rows = []
    for events in sorted(output.rglob("*-events.csv")):
        if events.name.endswith("-gpu-events.csv"):
            continue
        backend = events.name.removesuffix("-events.csv")
        directory = events.parent
        messages = directory / f"{backend}-messages.csv"
        warm = [int(row["total_ns"]) for row in csv.DictReader(messages.open(encoding="utf-8")) if "warmup complete" in row["MessageName"]]
        if not warm:
            continue
        cut = max(warm)
        groups = {}
        for row in csv.DictReader(events.open(encoding="utf-8")):
            if row.get("exec_time_ns") and int(row["ns_since_start"]) >= cut:
                groups.setdefault(("CPU", row["name"], "us"), []).append(float(row["exec_time_ns"]) / 1000)
        gpu = directory / f"{backend}-gpu-events.csv"
        if gpu.is_file():
            for row in csv.DictReader(gpu.open(encoding="utf-8")):
                if row.get("GPU execution time") and int(row["Time from start of program"]) >= cut:
                    groups.setdefault(("GPU", row["name"], "us"), []).append(float(row["GPU execution time"]) / 1000)
        plots = directory / f"{backend}-plots.csv"
        if plots.is_file():
            for row in csv.DictReader(plots.open(encoding="utf-8")):
                if row.get("value") and int(row["ns_since_start"]) >= cut and row["name"].endswith(("CompletedGpuMs", "CompletedLatencyMs")):
                    groups.setdefault(("Plot", row["name"], "ms"), []).append(float(row["value"]))
        for (kind, name, unit), values in sorted(groups.items()):
            rows.append([str(directory.relative_to(output)), backend, cut, kind, name, unit, len(values), statistics.mean(values), percentile(values, .5), percentile(values, .95), min(values), max(values)])
    with (output / "trace-summary.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["capture", "backend", "last_warmup_ns", "kind", "zone", "unit", "count", "mean", "p50", "p95", "min", "max"])
        writer.writerows(rows)
    memory = []
    for path in sorted(output.rglob("memory-*.csv")):
        samples = list(csv.DictReader(path.open(encoding="utf-8")))
        if not samples or "private_bytes" not in samples[0]:
            continue
        for metric in ("private_bytes", "working_set_bytes"):
            values = [int(row[metric]) for row in samples]
            tail = values[len(values) * 3 // 4:]
            memory.append([str(path.relative_to(output)), metric, len(values), float(samples[-1]["seconds"]), max(values), statistics.median(tail)])
    with (output / "memory-summary.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["capture", "metric", "samples", "observed_seconds", "peak_bytes", "last_quarter_median_bytes"])
        writer.writerows(memory)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("comparison", "coverage", "summarize", "traces-summary"), default="comparison")
    parser.add_argument("--bin", type=pathlib.Path, default=pathlib.Path("build_release/_build/Release"))
    parser.add_argument("--baseline-bin", type=pathlib.Path, help="Directory containing <target>.baseline.exe")
    parser.add_argument("--output", type=pathlib.Path, default=pathlib.Path("build_release/gpu-lifecycle-perf"))
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--start-pair", type=int, default=0, help="Resume already saved independent pairs at this zero-based index")
    parser.add_argument("--cold-iterations", type=int, default=5, help="Fixed cold-start samples; scene reconstruction is outside the timed interval")
    parser.add_argument("--min-time", help="Comparison defaults to 0.1s; coverage uses one iteration (1x)")
    parser.add_argument("--target", action="append", choices=tuple(workloads()), help="Only run selected existing benchmark entrances")
    parser.add_argument("--filter", help="Override the case filter for selected targets")
    args = parser.parse_args()
    if args.repetitions < 1 or (args.mode == "comparison" and args.repetitions < 3):
        parser.error("comparison requires at least three independent baseline/final pairs")
    args.output.mkdir(parents=True, exist_ok=True)
    if args.mode == "summarize":
        summarize(args.output)
        return
    if args.mode == "traces-summary":
        summarize_traces(args.output)
        return
    selected = {name: pattern for name, pattern in workloads().items() if not args.target or name in args.target}
    baseline = args.baseline_bin or args.bin
    metadata = {"utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "platform": platform.platform(), "processor": platform.processor(), "mode": args.mode, "executables": {}}
    for target in selected:
        for tag in (("baseline", "final") if args.mode == "comparison" else ("final",)):
            exe = (baseline / f"{target}.baseline.exe") if tag == "baseline" else (args.bin / f"{target}.exe")
            if not exe.is_file():
                parser.error(f"missing prebuilt executable: {exe}")
            metadata["executables"][str(exe.resolve())] = hashlib.sha256(exe.read_bytes()).hexdigest()
    (args.output / f"{args.mode}-metadata.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    for pair in range(args.start_pair if args.mode == "comparison" else 0, args.repetitions if args.mode == "comparison" else 1):
        for target, pattern in selected.items():
            tags = ("baseline", "final") if pair % 2 == 0 else ("final", "baseline")
            if args.mode == "coverage":
                tags = ("final",)
            for tag in tags:
                directory = args.output / (f"comparison-{pair}-{tag}" if args.mode == "comparison" else "coverage-final")
                directory.mkdir(exist_ok=True)
                exe = (baseline / f"{target}.baseline.exe") if tag == "baseline" else (args.bin / f"{target}.exe")
                case_filter = args.filter or (pattern if args.mode == "comparison" else ".*")
                runs = [(target, case_filter, args.min_time or ('0.1s' if args.mode == 'comparison' else '1x'))]
                if args.mode == "comparison" and target == "bench_scene_gpu" and not args.filter:
                    runs = [(target, pattern.replace("|first_use|growth", ""), args.min_time or "0.1s"),
                            (target + "-cold", r"^SceneGpu/(d3d12|vulkan)/PrepareAndRecord/F(1|3)/(first_use|growth)/views:(1|3)/real_time$", f"{args.cold_iterations}x")]
                if args.mode == "comparison" and target == "bench_lifecycle" and not args.filter:
                    deletion, frames = pattern.split("|^Lifecycle/Frame/", 1)
                    runs = [(target, "^Lifecycle/Frame/" + frames, args.min_time or "0.1s"),
                            (target + "-deletion", deletion, f"{args.cold_iterations}x")]
                for run_name, run_filter, min_time in runs:
                    command = [str(exe.resolve()), f"--benchmark_filter={run_filter}", f"--benchmark_min_time={min_time}", f"--benchmark_repetitions={1 if args.mode == 'comparison' else args.repetitions}", f"--benchmark_out={(directory / (run_name + '.json')).resolve()}", "--benchmark_out_format=json"]
                    (directory / f"{run_name}.invocation.json").write_text(json.dumps(command, indent=2), encoding="utf-8")
                    print(f"{args.mode} pair={pair + 1} {run_name} {tag}", flush=True)
                    with (directory / f"{run_name}.log").open("w", encoding="utf-8") as log:
                        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=False)
                    if result.returncode:
                        parser.exit(result.returncode, f"workload exited with {result.returncode}; see {directory / (run_name + '.log')}\n")
    if args.mode == "comparison":
        summarize(args.output)
    print(f"Saved {args.mode} data to {args.output.resolve()}")


if __name__ == "__main__":
    main()
