#!/usr/bin/env python3
"""Run serial CTest validation and retain backend coverage with native evidence.

Requires an already built configuration. Does not build, change the checkout,
download a compiler, or treat unavailable required backends as success.
"""

from __future__ import annotations

import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import xml.etree.ElementTree as ET


REPO = Path(__file__).resolve().parent.parent


def command_output(args: list[str]) -> str:
    result = subprocess.run(args, cwd=REPO, capture_output=True, check=True)
    return result.stdout.decode("utf-8", errors="replace").strip()


def parse_case(case: ET.Element) -> dict:
    props = {p.get("name"): p.get("value") for p in case.findall("properties/property")}
    output = "\n".join(case.itertext())
    status = "FAIL" if case.find("failure") is not None or case.find("error") is not None else "SKIP" if case.find("skipped") is not None else "PASS"
    return {"name": case.get("name"), "suite": case.get("classname"), "status": status,
            "seconds": float(case.get("time", "0")), "backend": props.get("backend"),
            "properties": props, "environment_failure": status == "FAIL" and "setup failed at stage" in output}


def summarize(cases: list[dict], required: list[str], exit_code: int) -> dict:
    totals = Counter(c["status"] for c in cases)
    coverage = {backend: dict(Counter(c["status"] for c in cases if c["backend"] == backend)) for backend in ("d3d12", "vulkan")}
    issues = []
    for backend in required:
        counts = coverage[backend]
        if not counts.get("PASS", 0):
            issues.append(f"required backend {backend} has no executed passing test")
        if counts.get("SKIP", 0):
            issues.append(f"required backend {backend} has skipped tests")
    probes, unexpected = [], 0
    for case in cases:
        props = case["properties"]
        errors = int(props.get("observed_validation_errors", "0"))
        if props.get("evidence_class") == "isolated_expected_error":
            probes.append({"backend": case["backend"], "test": case["name"], "expected": int(props.get("expected_validation_errors", "0")), "observed": errors})
            if errors != int(props.get("expected_validation_errors", "0")):
                issues.append(f"native validation probe mismatch: {case['name']}")
        else:
            unexpected += errors
    if unexpected:
        issues.append(f"{unexpected} unexpected native validation errors")
    if not cases:
        issues.append("no test XML evidence")
    if totals.get("FAIL", 0) or exit_code:
        issues.append(f"test failure or CTest exit code {exit_code}")
    return {"success": not issues, "counts": dict(totals), "coverage": coverage,
            "environment_failures": sum(c["environment_failure"] for c in cases),
            "expected_validation_probes": probes, "unexpected_validation_errors": unexpected, "issues": issues}


def complete_crashed_cases(cases: list[dict], junit: ET.Element) -> None:
    """CTest retains process crashes even when gtest cannot flush its XML."""
    known = {f"{c['suite']}.{c['name']}" for c in cases}
    for case in junit.iter("testcase"):
        log = case.findtext("system-out", "")
        match = re.search(r"Google Test filter = (.+)", log)
        if match and match.group(1).strip() in known:
            continue
        parsed = parse_case(case)
        if parsed["status"] == "FAIL":
            lower = log.lower()
            if "d3d12 select adapter" in lower:
                parsed["backend"] = "d3d12"
            elif "vulkan" in lower:
                parsed["backend"] = "vulkan"
            parsed["evidence_source"] = "ctest_only_process_failure"
            cases.append(parsed)


def self_test() -> None:
    fixture = ET.fromstring('<testsuite><testcase name="ok"><properties><property name="backend" value="d3d12"/></properties></testcase><testcase name="absent"><skipped/></testcase><testcase name="bad"><failure>vulkan setup failed at stage 6</failure></testcase></testsuite>')
    cases = [parse_case(c) for c in fixture]
    assert summarize(cases[:1], ["d3d12"], 0)["success"]
    assert summarize(cases[:2], [], 0)["success"]
    assert not summarize(cases[:1], ["vulkan"], 0)["success"]
    failed = summarize(cases, [], 0)
    assert not failed["success"] and failed["environment_failures"] == 1
    cases[0]["status"] = "SKIP"
    assert not summarize(cases[:1], ["d3d12"], 0)["success"]
    cases[0]["status"] = "PASS"
    cases[0]["properties"] = {"observed_validation_errors": "1"}
    assert not summarize(cases[:1], [], 0)["success"]
    cases[0]["properties"].update(evidence_class="isolated_expected_error", expected_validation_errors="1")
    assert summarize(cases[:1], [], 0)["success"]
    assert not summarize(cases[:1], [], 8)["success"]
    crashed = ET.fromstring('<testsuite><testcase name="crash"><failure/><system-out>d3d12 select adapter</system-out></testcase></testsuite>')
    recovered = []
    complete_crashed_cases(recovered, crashed)
    assert summarize(recovered, ["d3d12"], 8)["counts"] == {"FAIL": 1}
    print("PASS: coverage, environment failures, optional skips, native probe isolation and exit status")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--config", default="Debug", choices=("Debug", "Release", "RelWithDebInfo", "MinSizeRel"))
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--required-backends", default="d3d12,vulkan")
    parser.add_argument("--ctest", default="ctest")
    parser.add_argument("--regex")
    parser.add_argument("--gpu-validation", action="store_true", help="Small isolated GPU validation run; avoid using for benchmarks or delayed-fence pressure")
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if not args.build_dir or not args.output_dir:
        parser.error("--build-dir and --output-dir are required")
    required = [s.strip().lower() for s in args.required_backends.split(",") if s.strip()]
    if any(s not in ("d3d12", "vulkan") for s in required):
        parser.error("required backends must be d3d12 and/or vulkan")
    build, output = args.build_dir.resolve(), args.output_dir.resolve()
    if output.exists() and any(output.iterdir()):
        parser.error("output directory must be empty; use a new directory for each run")
    output.mkdir(parents=True, exist_ok=True)
    xml_dir = output / "gtest"
    xml_dir.mkdir()
    env = os.environ.copy()
    env["RADRAY_TEST_REQUIRED_BACKENDS"] = ",".join(required)
    env["RADRAY_TEST_GPU_VALIDATION"] = "1" if args.gpu_validation else "0"
    env["GTEST_OUTPUT"] = "xml:" + str(xml_dir) + os.sep
    command = [args.ctest, "--test-dir", str(build), "-C", args.config, "-j", "1", "--timeout", str(args.timeout), "--output-on-failure", "--output-junit", str(output / "ctest.xml")]
    if args.regex:
        command += ["-R", args.regex]
    cache = {}
    for line in (build / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace").splitlines():
        if "=" in line and ":" in line and (line.startswith("RADRAY_") or line.startswith("CMAKE_CXX_COMPILER:")):
            key, value = line.split("=", 1)
            cache[key.split(":", 1)[0]] = value
    patch = subprocess.run(["git", "diff", "HEAD", "--binary"], cwd=REPO, capture_output=True, check=True).stdout
    untracked = command_output(["git", "ls-files", "--others", "--exclude-standard"]).splitlines()
    files = {name: hashlib.sha256((REPO / name).read_bytes()).hexdigest() for name in untracked if (REPO / name).is_file()}
    metadata = {"timestamp_utc": datetime.now(timezone.utc).isoformat(), "sha": command_output(["git", "rev-parse", "HEAD"]),
                "tracked_diff_sha256": hashlib.sha256(patch).hexdigest(), "untracked_sha256": files,
                "status": command_output(["git", "status", "--short"]), "config": args.config, "cache": cache,
                "platform": platform.platform(), "required_backends": required, "gpu_validation": args.gpu_validation,
                "command": command}
    if os.name == "nt":
        metadata["windows_adapters"] = command_output(["powershell", "-NoProfile", "-Command", "Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,DriverDate,PNPDeviceID | ConvertTo-Json -Compress"])
    (output / "metadata.json").write_text(json.dumps(metadata, indent=2, ensure_ascii=False), encoding="utf-8")
    print(f"Running {args.config}; required backends: {required}; evidence: {output}", flush=True)
    with (output / "ctest.log").open("w", encoding="utf-8") as log:
        result = subprocess.run(command, cwd=REPO, env=env, stdout=log, stderr=subprocess.STDOUT)
    cases = []
    for path in sorted(xml_dir.glob("*.xml")):
        for case in ET.parse(path).getroot().iter("testcase"):
            parsed = parse_case(case)
            parsed["xml"] = str(path.relative_to(output))
            cases.append(parsed)
    native_error_cases = []
    junit_path = output / "ctest.xml"
    junit = ET.parse(junit_path).getroot() if junit_path.exists() else ET.Element("testsuite")
    complete_crashed_cases(cases, junit)
    summary = summarize(cases, required, result.returncode)
    if not junit_path.exists():
        summary["success"] = False
        summary["issues"].append("CTest did not produce its JUnit report")
    for case in junit.iter("testcase"):
        log = case.findtext("system-out", "")
        if "GPU validation:" in log and "H04OneExpectedNativeValidationError" not in case.get("name", "") and "H04ValidationCallbacks" not in case.get("name", ""):
            native_error_cases.append(case.get("name"))
    if native_error_cases:
        summary["success"] = False
        summary["issues"].append("Unexpected native validation log in: " + ", ".join(native_error_cases))
    summary.update(metadata=metadata, ctest_exit_code=result.returncode, cases=cases)
    (output / "summary.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False), encoding="utf-8")
    print(json.dumps({k: v for k, v in summary.items() if k not in ("cases", "metadata")}, ensure_ascii=False, indent=2))
    return 0 if summary["success"] else 1


if __name__ == "__main__":
    sys.exit(main())
