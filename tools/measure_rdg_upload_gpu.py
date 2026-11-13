"""Run paired RDG upload GPU benchmarks; keep raw samples and Tracy evidence."""

import argparse
import csv
import ctypes
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import time


def pin(process, mask):
    if os.name == "nt":
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.restype = ctypes.c_void_p
        kernel.SetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        handle = kernel.OpenProcess(0x0200, False, process.pid)
        ok = handle and kernel.SetProcessAffinityMask(handle, mask)
        if handle:
            kernel.CloseHandle(handle)
        if not ok:
            process.kill()
            raise SystemExit("Cannot set benchmark affinity")
    else:
        os.sched_setaffinity(process.pid, {i for i in range(mask.bit_length()) if mask & (1 << i)})


def wait_text(process, path, text, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and process.poll() is None:
        if text in path.read_text(encoding="utf-8", errors="replace"):
            return True
        time.sleep(0.1)
    return False


def main():
    parser = argparse.ArgumentParser(__doc__)
    for variant in ("baseline", "fixed"):
        for mode in ("off", "on"):
            parser.add_argument(f"--{variant}-{mode}", type=Path, required=True)
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--csvexport", type=Path, required=True)
    parser.add_argument("--timeline-export", type=Path, help="Optional raw GPU timeline CSV exporter, including zero-duration zones")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--samples", type=int, default=1000)
    parser.add_argument("--warmup", type=int, default=200)
    parser.add_argument("--affinity", type=lambda text: int(text, 0), default=0xff)
    parser.add_argument("--backends", nargs="+", choices=["d3d12", "vulkan"], default=["d3d12", "vulkan"])
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    results = []
    for backend in args.backends:
        for mode in ("off", "disconnected", "connected"):
            for repetition in range(args.rounds):
                order = ("baseline", "fixed") if repetition % 2 == 0 else ("fixed", "baseline")
                for variant in order:
                    run = args.output / f"{backend}-{mode}-{repetition + 1}-{variant}"
                    run.mkdir()
                    executable = getattr(args, f"{variant}_{'off' if mode == 'off' else 'on'}").resolve()
                    command = [str(executable), f"--rdg_backend={backend}", f"--rdg_samples={args.samples}",
                               f"--rdg_warmup={args.warmup}", f"--rdg_raw={run.resolve() / 'samples.csv'}",
                               f"--benchmark_out={run.resolve() / 'benchmark.json'}", "--benchmark_out_format=json"]
                    if mode == "connected":
                        command.append("--rdg_capture_wait")
                    print(run.name, flush=True)
                    with (run / "benchmark.log").open("w") as log, (run / "capture.log").open("w") as capture_log:
                        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=log, stderr=log)
                        pin(process, args.affinity)
                        capture = None
                        if mode == "connected":
                            if not wait_text(process, run / "benchmark.log", "Waiting for capture"):
                                process.kill()
                                raise SystemExit(f"Benchmark did not reach capture gate: {run}")
                            capture = subprocess.Popen([str(args.capture.resolve()), "-a", "127.0.0.1", "-o", str(run.resolve() / "session.tracy")], stdout=capture_log, stderr=capture_log)
                            # Capture stdout is buffered when redirected. Verify connection by
                            # the complete exported GPU frame count after the run.
                            time.sleep(3)
                            if capture.poll() is not None:
                                capture.kill()
                                process.kill()
                                raise SystemExit(f"Tracy did not connect: {run}")
                            process.stdin.write(b"\n")
                            process.stdin.flush()
                        code = process.wait(timeout=180)
                        if capture:
                            capture_code = capture.wait(timeout=60)
                            if capture_code:
                                raise SystemExit(f"Tracy failed: {run}")
                        if code:
                            raise SystemExit(f"Benchmark failed ({code}): {run}")
                    data = json.loads((run / "benchmark.json").read_text())
                    row = data["benchmarks"][0]
                    if row.get("error_occurred"):
                        raise SystemExit(f"Benchmark reported an error: {run}")
                    result = dict(backend=backend, mode=mode, repetition=repetition + 1, variant=variant,
                                  executable=str(executable), sha256=hashlib.sha256(executable.read_bytes()).hexdigest(),
                                  command=command, affinity=hex(args.affinity), metrics=row)
                    if capture:
                        with (run / "gpu.csv").open("w") as gpu, (run / "export.log").open("w") as errors:
                            subprocess.run([str(args.csvexport.resolve()), "-g", str(run.resolve() / "session.tracy")], stdout=gpu, stderr=errors, check=True)
                        counts = {}
                        with (run / "gpu.csv").open(newline="", encoding="utf-8") as source:
                            for zone in csv.DictReader(source):
                                name = zone.get("name", zone.get("zone", next(iter(zone.values()))))
                                counts[name] = counts.get(name, 0) + 1
                        result["gpu_positive_duration_zone_counts"] = counts
                        if counts.get("RDG benchmark frame", 0) < args.samples + args.warmup - 1:
                            raise SystemExit(f"Incomplete GPU capture: {run}: {counts}")
                        if args.timeline_export:
                            with (run / "gpu-timeline.csv").open("w") as timeline:
                                subprocess.run([str(args.timeline_export.resolve()), str(run.resolve() / "session.tracy")], stdout=timeline, check=True)
                            counts, incomplete, zero = {}, {}, {}
                            with (run / "gpu-timeline.csv").open(newline="", encoding="utf-8") as source:
                                for zone in csv.DictReader(source):
                                    name = zone["name"]
                                    counts[name] = counts.get(name, 0) + 1
                                    if zone["complete"] == "0":
                                        incomplete[name] = incomplete.get(name, 0) + 1
                                    elif zone["gpu_ns"] == "0":
                                        zero[name] = zero.get(name, 0) + 1
                            frames = args.warmup + args.samples
                            expected = {"UploadBuffer": 512 * frames, "UploadTexture": 512 * frames} if variant == "baseline" else {"RDG UploadBatch": 2 * frames}
                            expected.update({"middle consumer": frames, "RDG benchmark frame": frames})
                            if counts != expected:
                                raise SystemExit(f"Unexpected GPU timeline counts: {run}: {counts}")
                            result.update(gpu_zone_counts=counts, gpu_unresolved_zone_counts=incomplete, gpu_zero_duration_zone_counts=zero)
                    results.append(result)
                    (args.output / "runs.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    summary = []
    for backend in args.backends:
        for mode in ("off", "disconnected", "connected"):
            for variant in ("baseline", "fixed"):
                rows = [r["metrics"] for r in results if (r["backend"], r["mode"], r["variant"]) == (backend, mode, variant)]
                summary.append(dict(backend=backend, mode=mode, variant=variant, rounds=len(rows),
                                    **{key: statistics.median(row[key] for row in rows) for key in rows[0] if key.endswith("_us")}))
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
