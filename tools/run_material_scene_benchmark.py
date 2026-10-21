"""Serial baseline/final capture; requires an already built Release benchmark."""
import argparse
import csv
import ctypes
import hashlib
import json
import pathlib
import subprocess
import time


def measure_memory(options, output):
    """Observe process committed/private and resident bytes, not native GPU allocation sizes."""
    class Counters(ctypes.Structure):
        _fields_ = [("cb", ctypes.c_ulong), ("PageFaultCount", ctypes.c_ulong)] + [
            (name, ctypes.c_size_t) for name in (
                "PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage", "QuotaPagedPoolUsage",
                "QuotaPeakNonPagedPoolUsage", "QuotaNonPagedPoolUsage", "PagefileUsage", "PeakPagefileUsage", "PrivateUsage")]

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    kernel.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
    kernel.OpenProcess.restype = ctypes.c_void_p
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    psapi.GetProcessMemoryInfo.argtypes = [ctypes.c_void_p, ctypes.POINTER(Counters), ctypes.c_ulong]
    psapi.GetProcessMemoryInfo.restype = ctypes.c_int
    with output.with_suffix(".log").open("w", encoding="utf-8") as log:
        start = time.perf_counter()
        process = subprocess.Popen(options, stdout=log, stderr=subprocess.STDOUT)
        handle = kernel.OpenProcess(0x0400 | 0x0010, False, process.pid)
        if not handle:
            process.terminate()
            process.wait()
            raise RuntimeError(f"OpenProcess failed: {ctypes.get_last_error()}")
        with output.open("w", newline="", encoding="utf-8") as curve:
            writer = csv.writer(curve)
            writer.writerow(["seconds", "working_set_bytes", "private_bytes"])
            while process.poll() is None:
                counters = Counters()
                counters.cb = ctypes.sizeof(counters)
                if psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
                    writer.writerow([time.perf_counter() - start, counters.WorkingSetSize, counters.PrivateUsage])
                time.sleep(0.05)
        kernel.CloseHandle(handle)
        if process.returncode:
            raise RuntimeError(f"Memory workload failed with exit {process.returncode}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--tracy", type=pathlib.Path, default=pathlib.Path("F:/program/tracy"))
    parser.add_argument("--filter", default=r"MaterialScene/(d3d12|vulkan)/(PrepareRecord|FullFrame)/N(1000|10000|100000)/M(1|100)/V(1|3)/dirty(0|1)/")
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--min-time", default="0.1s")
    parser.add_argument("--no-traces", action="store_true")
    parser.add_argument("--traces-only", action="store_true")
    parser.add_argument("--backend", action="append", choices=("d3d12", "vulkan"), help="Limit trace captures to selected backends")
    parser.add_argument("--trace-filter", default="^MaterialScene/{backend}/FullFrame/N10000/M100/V3/dirty1/real_time$")
    parser.add_argument("--trace-min-time", default="4s")
    parser.add_argument("--memory-only", action="store_true")
    parser.add_argument("--memory-case", action="append", default=[], help="Run each regex in a separate process and save its memory curve")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "executable.json").write_text(json.dumps({"path": str(args.exe.resolve()), "sha256": hashlib.sha256(args.exe.read_bytes()).hexdigest()}, indent=2), encoding="utf-8")
    options = [str(args.exe.resolve()), f"--benchmark_filter={args.filter}", f"--benchmark_min_time={args.min_time}", f"--benchmark_repetitions={args.repetitions}", f"--benchmark_out={args.output.resolve() / 'benchmark.json'}", "--benchmark_out_format=json"]
    (args.output / "invocation.json").write_text(json.dumps(options, indent=2), encoding="utf-8")
    if not args.memory_only and not args.traces_only:
        with (args.output / "benchmark.log").open("w", encoding="utf-8") as log:
            subprocess.run(options, stdout=log, stderr=subprocess.STDOUT, check=True)
    for backend in (() if args.no_traces or args.memory_only else (args.backend or ("d3d12", "vulkan"))):
        trace = args.output.resolve() / f"{backend}.tracy"
        with (args.output / f"{backend}-capture.log").open("w", encoding="utf-8") as log:
            capture = subprocess.Popen([str(args.tracy / "tracy-capture.exe"), "-a", "127.0.0.1", "-o", str(trace), "-f"], stdout=log, stderr=subprocess.STDOUT)
            time.sleep(0.5)
            with (args.output / f"{backend}-profile.log").open("w", encoding="utf-8") as benchlog:
                command = [str(args.exe.resolve()), f"--benchmark_filter={args.trace_filter.replace('{backend}', backend)}", f"--benchmark_min_time={args.trace_min_time}", "--benchmark_repetitions=1"]
                (args.output / f"{backend}-profile-invocation.json").write_text(json.dumps(command, indent=2), encoding="utf-8")
                subprocess.run(command, stdout=benchlog, stderr=subprocess.STDOUT, check=True)
            capture.wait(timeout=60)
            if capture.returncode or not trace.exists():
                raise RuntimeError(f"Tracy capture failed for {backend}")
        with (args.output / f"{backend}-zones.csv").open("w", encoding="utf-8") as out:
            subprocess.run([str(args.tracy / "tracy-csvexport.exe"), str(trace)], stdout=out, check=True)
        for suffix, flag in (("events", "-u"), ("messages", "-m"), ("gpu-events", "-g")):
            with (args.output / f"{backend}-{suffix}.csv").open("w", encoding="utf-8") as out:
                subprocess.run([str(args.tracy / "tracy-csvexport.exe"), flag, str(trace)], stdout=out, check=True)
        with (args.output / f"{backend}-plots.csv").open("w", encoding="utf-8") as out:
            subprocess.run([str(args.tracy / "tracy-csvexport.exe"), "-u", "-p", str(trace)], stdout=out, check=True)
    for index, case in enumerate(args.memory_case):
        memory = args.output.resolve() / f"memory-{index}.csv"
        command = [str(args.exe.resolve()), f"--benchmark_filter={case}", "--benchmark_min_time=2s", f"--benchmark_repetitions={args.repetitions}", f"--benchmark_out={memory.with_suffix('.json')}", "--benchmark_out_format=json"]
        memory.with_suffix(".invocation.json").write_text(json.dumps(command, indent=2), encoding="utf-8")
        measure_memory(command, memory)
    print(f"Saved measurements to {args.output.resolve()}")


if __name__ == "__main__":
    main()
