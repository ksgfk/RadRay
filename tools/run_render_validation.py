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
        validated = [c for c in cases if c["backend"] == backend and c["status"] == "PASS"
                     and c["properties"].get("validation") == "enabled"]
        if not validated:
            issues.append(f"required backend {backend} has no executed validation-enabled passing test")
        if coverage[backend].get("SKIP", 0):
            issues.append(f"required backend {backend} has skipped tests")
    probes, diagnostics, warnings, algorithms = [], [], [], []
    unexpected = collection_failures = 0
    for case in cases:
        props = case["properties"]
        validated = props.get("validation") == "enabled" and case["status"] != "SKIP"
        if props.get("diagnostic_algorithm"):
            algorithms.append({"algorithm": props["diagnostic_algorithm"], "test": case["name"], "status": case["status"]})
            if case["status"] != "PASS" or props.get("diagnostic_algorithm_result") != "passed":
                issues.append(f"diagnostic algorithm evidence failed or missing: {case['name']}")
        if case["backend"] in ("d3d12", "vulkan") and case["status"] == "PASS" and props.get("validation") not in ("enabled", "disabled"):
            issues.append(f"actual validation state is missing: {case['name']}")

        def count(name: str, mandatory: bool = False) -> int:
            value = props.get(name)
            if value is None and not mandatory:
                return 0
            if value is None or not value.isdecimal():
                issues.append(f"missing or invalid {name}: {case['name']}")
                return 0
            return int(value)

        errors = count("observed_validation_errors", validated)
        expected_errors = count("expected_validation_errors", props.get("evidence_class") == "isolated_expected_error")
        failures = count("diagnostic_collection_failures", validated and case["backend"] == "d3d12")
        expected_failures = count("expected_collection_failures", props.get("evidence_class") == "isolated_collection_failure")
        if props.get("evidence_class") == "isolated_expected_error":
            probes.append({"backend": case["backend"], "test": case["name"], "expected": expected_errors, "observed": errors})
            if expected_errors == 0 or errors != expected_errors:
                issues.append(f"native validation probe mismatch: {case['name']}")
        else:
            unexpected += errors
            if expected_errors:
                issues.append(f"expected errors outside an isolated probe: {case['name']}")
        if props.get("evidence_class") == "isolated_collection_failure":
            probes.append({"backend": case["backend"], "test": case["name"], "expected_collection_failures": expected_failures, "observed_collection_failures": failures})
            if expected_failures == 0 or failures != expected_failures:
                issues.append(f"collection failure probe mismatch: {case['name']}")
        else:
            collection_failures += failures
            if expected_failures:
                issues.append(f"expected collection failures outside an isolated probe: {case['name']}")
        if validated and case["backend"] == "d3d12":
            if props.get("diagnostic_mode") not in ("callback", "polling"):
                issues.append(f"missing or invalid D3D12 collection mode: {case['name']}")
            for field in ("diagnostic_self_test", "diagnostic_expectations", "diagnostic_finalization"):
                if props.get(field) != "passed":
                    issues.append(f"{field} missing or failed: {case['name']}")
            mode, selection, query = (props.get(field) for field in
                                      ("diagnostic_mode", "diagnostic_selection", "diagnostic_infoqueue1_hresult"))
            if selection not in ("native", "base-interface-only") or query not in ("0x00000000", "0x80004002"):
                issues.append(f"missing or invalid native interface selection: {case['name']}")
            expected_mode = "callback" if selection == "native" and query == "0x00000000" else "polling"
            if mode != expected_mode:
                issues.append(f"interface selection and actual mode disagree: {case['name']}")
            if props.get("diagnostic_d3d12_strategy") != ("callback" if mode == "callback" else "checked_clear") or props.get("diagnostic_dxgi_strategy") != "checked_clear":
                issues.append(f"diagnostic capacity strategies missing or inconsistent: {case['name']}")
            expected_flush = "failed" if failures else "passed"
            if props.get("diagnostic_flush") != expected_flush:
                issues.append(f"diagnostic checkpoint result missing or inconsistent: {case['name']}")
            health = "incomplete" if failures else "complete"
            if props.get("diagnostic_health") != health:
                issues.append(f"diagnostic health missing or inconsistent: {case['name']}")
            diagnostics.append({"test": case["name"], "mode": props.get("diagnostic_mode"), "health": props.get("diagnostic_health"),
                                "selection": selection, "infoqueue1_hresult": query,
                                "d3d12_strategy": props.get("diagnostic_d3d12_strategy"), "dxgi_strategy": props.get("diagnostic_dxgi_strategy"),
                                "expectations": props.get("diagnostic_expectations"), "finalization": props.get("diagnostic_finalization"),
                                "flush": props.get("diagnostic_flush"), "self_test": props.get("diagnostic_self_test"),
                                "collection_failures": failures, "native_errors": errors})
        warning_count = count("observed_validation_warnings", validated and case["backend"] == "d3d12")
        if warning_count:
            warnings.append({"backend": case["backend"], "test": case["name"], "count": warning_count})
    if unexpected:
        issues.append(f"{unexpected} unexpected native validation errors")
    if collection_failures:
        issues.append(f"{collection_failures} unexpected diagnostic collection failures")
    if not cases:
        issues.append("no test XML evidence")
    if totals.get("FAIL", 0) or exit_code:
        issues.append(f"test failure or CTest exit code {exit_code}")
    return {"success": not issues, "counts": dict(totals), "coverage": coverage,
            "environment_failures": sum(c["environment_failure"] for c in cases),
            "expected_validation_probes": probes, "unexpected_validation_errors": unexpected,
            "unexpected_collection_failures": collection_failures, "diagnostics": diagnostics, "diagnostic_algorithms": algorithms,
            "warnings": warnings, "skipped_tests": [c["name"] for c in cases if c["status"] == "SKIP"], "issues": issues}


def win10_legacy_evidence(metadata: dict, cases: list[dict]) -> dict:
    reasons = []
    runtime = metadata.get("windows_diagnostics", {})
    system = runtime.get("os", {})
    build = str(system.get("build", ""))
    if "Windows 10" not in system.get("caption", "") or not build.isdecimal() or int(build) >= 22000:
        reasons.append("requires a real Windows 10 client; forcing a base interface on Windows 11 is not equivalent")
    if not system.get("version") or system.get("ubr") is None:
        reasons.append("Windows version/build revision evidence is missing")
    files = {entry.get("name"): entry.get("file_version") for entry in runtime.get("runtime_files", [])}
    if any(files.get(name) in (None, "", "not_present") for name in ("d3d12.dll", "d3d12SDKLayers.dll", "dxgidebug.dll")) or not files.get("D3D12Core.dll"):
        reasons.append("native diagnostic runtime versions are missing")
    passing = [case["properties"] for case in cases if case["status"] == "PASS"]
    natural = [props for props in passing if props.get("diagnostic_selection") == "native"
               and props.get("diagnostic_infoqueue1_hresult") == "0x80004002" and props.get("diagnostic_mode") == "polling"
               and props.get("diagnostic_finalization") == "passed"]
    if not any(props.get("diagnostic_probe") == "allocator_reset" for props in natural):
        reasons.append("missing natural E_NOINTERFACE fallback with synchronous finalization and allocator Reset error")
    if not any(props.get("diagnostic_probe") == "dxgi_native_error" for props in natural):
        reasons.append("missing DXGI native error evidence on the actual legacy runtime")
    required = {"checked_clear", "clear_race", "dxgi_multi_queue", "channel_lifecycle"}
    observed = {props.get("diagnostic_algorithm") for props in passing if props.get("diagnostic_algorithm_result") == "passed"}
    if required - observed:
        reasons.append("missing diagnostic algorithm evidence: " + ", ".join(sorted(required - observed)))
    return {"status": "verified" if not reasons else "pending", "reasons": reasons}


def complete_crashed_cases(cases: list[dict], junit: ET.Element) -> None:
    """CTest retains process crashes even when gtest cannot flush its XML."""
    known = {f"{c['suite']}.{c['name']}" for c in cases}
    for case in junit.iter("testcase"):
        log = case.findtext("system-out", "")
        match = re.search(r"Google Test filter = (.+)", log)
        if match and match.group(1).strip() in known:
            continue
        parsed = parse_case(case)
        lower = log.lower()
        if "d3d12 select adapter" in lower:
            parsed["backend"] = "d3d12"
        elif "vulkan" in lower:
            parsed["backend"] = "vulkan"
        parsed["status"] = "FAIL"
        parsed["evidence_source"] = "ctest_only_missing_gtest_evidence"
        cases.append(parsed)



def self_test() -> None:
    fixture = ET.fromstring('<testsuite><testcase name="ok"><properties><property name="backend" value="d3d12"/></properties></testcase><testcase name="absent"><skipped/></testcase><testcase name="bad"><failure>vulkan setup failed at stage 6</failure></testcase></testsuite>')
    cases = [parse_case(case) for case in fixture]
    props = {"validation": "enabled", "observed_validation_errors": "0", "observed_validation_warnings": "0",
             "diagnostic_mode": "callback", "diagnostic_health": "complete", "diagnostic_self_test": "passed",
             "diagnostic_collection_failures": "0", "diagnostic_selection": "native", "diagnostic_infoqueue1_hresult": "0x00000000",
             "diagnostic_d3d12_strategy": "callback", "diagnostic_dxgi_strategy": "checked_clear",
             "diagnostic_expectations": "passed", "diagnostic_finalization": "passed", "diagnostic_flush": "passed"}
    cases[0]["properties"] = dict(props)
    assert summarize(cases[:1], ["d3d12"], 0)["success"]
    assert summarize(cases[:2], [], 0)["success"]
    assert not summarize(cases[:1], ["vulkan"], 0)["success"]
    failed = summarize(cases, [], 0)
    assert not failed["success"] and failed["environment_failures"] == 1
    for key in props:
        cases[0]["properties"] = {name: value for name, value in props.items() if name != key}
        assert not summarize(cases[:1], ["d3d12"], 0)["success"], key
    cases[0]["properties"] = dict(props, validation="disabled")
    assert not summarize(cases[:1], ["d3d12"], 0)["success"]
    cases[0]["properties"] = dict(props)
    cases[0]["status"] = "SKIP"
    assert not summarize(cases[:1], ["d3d12"], 0)["success"]
    cases[0]["status"] = "PASS"
    cases[0]["properties"] = dict(props, observed_validation_errors="1")
    assert not summarize(cases[:1], [], 0)["success"]
    cases[0]["properties"].update(evidence_class="isolated_expected_error", expected_validation_errors="1")
    assert summarize(cases[:1], [], 0)["success"]
    cases[0]["properties"] = dict(props, diagnostic_collection_failures="1", diagnostic_health="incomplete", diagnostic_flush="failed")
    assert not summarize(cases[:1], [], 0)["success"]
    cases[0]["properties"].update(evidence_class="isolated_collection_failure", expected_collection_failures="1")
    assert summarize(cases[:1], [], 0)["success"]
    assert not summarize(cases[:1], [], 8)["success"]
    cases[0]["properties"]["diagnostic_self_test"] = "failed"
    assert not summarize(cases[:1], [], 0)["success"]
    crashed = ET.fromstring('<testsuite><testcase name="crash"><failure/><system-out>d3d12 select adapter</system-out></testcase></testsuite>')
    recovered = []
    complete_crashed_cases(recovered, crashed)
    assert summarize(recovered, ["d3d12"], 8)["counts"] == {"FAIL": 1}
    missing = []
    complete_crashed_cases(missing, ET.fromstring('<testsuite><testcase name="missing"><system-out>Google Test filter = Suite.Missing</system-out></testcase></testsuite>'))
    assert not summarize(missing, [], 0)["success"]
    cases[0]["properties"] = dict(props, diagnostic_expectations="failed")
    assert not summarize(cases[:1], [], 0)["success"]
    cases[0]["properties"] = dict(props, diagnostic_mode="polling", diagnostic_d3d12_strategy="checked_clear")
    assert not summarize(cases[:1], [], 0)["success"]
    cases[0]["properties"]["diagnostic_selection"] = "base-interface-only"
    assert summarize(cases[:1], [], 0)["success"]
    assert win10_legacy_evidence({}, cases)["status"] == "pending"
    legacy_props = dict(props, diagnostic_mode="polling", diagnostic_d3d12_strategy="checked_clear", diagnostic_infoqueue1_hresult="0x80004002")
    legacy = [{"status": "PASS", "properties": dict(legacy_props, diagnostic_probe=probe)} for probe in ("allocator_reset", "dxgi_native_error")]
    legacy += [{"status": "PASS", "properties": {"diagnostic_algorithm": algorithm, "diagnostic_algorithm_result": "passed"}}
               for algorithm in ("checked_clear", "clear_race", "dxgi_multi_queue", "channel_lifecycle")]
    metadata = {"windows_diagnostics": {"os": {"caption": "Microsoft Windows 10 Pro", "build": "19045", "version": "10.0.19045", "ubr": 1},
                "runtime_files": [{"name": name, "file_version": "10.0.19041.1"} for name in
                                  ("d3d12.dll", "D3D12Core.dll", "d3d12SDKLayers.dll", "dxgidebug.dll")]}}
    assert win10_legacy_evidence(metadata, legacy)["status"] == "verified"
    legacy[0]["properties"]["diagnostic_selection"] = "base-interface-only"
    assert win10_legacy_evidence(metadata, legacy)["status"] == "pending"
    print("PASS: required evidence, exclusive capture, synchronous finalization, exact matching, native Win10 fallback and crashes")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--config", default="Debug", choices=("Debug", "Release", "RelWithDebInfo", "MinSizeRel"))
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--required-backends", default="d3d12,vulkan")
    parser.add_argument("--ctest", default="ctest")
    parser.add_argument("--regex")
    parser.add_argument("--gpu-validation", action="store_true", help="Small isolated GPU validation run; avoid using for benchmarks or delayed-fence pressure")
    parser.add_argument("--require-win10-legacy", action="store_true", help="Require actual Windows 10 E_NOINTERFACE fallback and native queue-semantics evidence")
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
                "command": command,
                "runtime_environment": {name: env[name] for name in
                    ("VK_LOADER_LAYERS_DISABLE", "VK_LAYER_PATH", "VK_DRIVER_FILES", "RADRAY_TEST_D3D12_ADAPTER_INDEX", "RADRAY_TEST_VULKAN_ADAPTER_INDEX")
                    if name in env}}
    if os.name == "nt":
        runtime_command = r"""
$system = Get-CimInstance Win32_OperatingSystem
$version = Get-ItemProperty 'HKLM:\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion'
$files = @('d3d12.dll','D3D12Core.dll','d3d12SDKLayers.dll','dxgidebug.dll') | ForEach-Object {
    $path = Join-Path $env:WINDIR "System32/$_"
    [pscustomobject]@{name=$_; path=$path; file_version=$(if (Test-Path -LiteralPath $path) { (Get-Item -LiteralPath $path).VersionInfo.FileVersion } else { 'not_present' })}
}
@{os=@{caption=$system.Caption; version=$system.Version; build=$system.BuildNumber; ubr=$version.UBR}; runtime_files=@($files)} | ConvertTo-Json -Depth 4 -Compress
"""
        metadata["windows_diagnostics"] = json.loads(command_output(["powershell", "-NoProfile", "-Command", runtime_command]))
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
    summary["callback_coverage"] = dict(Counter(
        case["properties"]["diagnostic_callback_runtime"] for case in cases
        if case["status"] == "PASS" and "diagnostic_callback_runtime" in case["properties"]))
    summary["win10_legacy_acceptance"] = win10_legacy_evidence(metadata, cases)
    if args.require_win10_legacy and summary["win10_legacy_acceptance"]["status"] != "verified":
        summary["success"] = False
        summary["issues"].extend(summary["win10_legacy_acceptance"]["reasons"])
    if args.gpu_validation:
        for backend in required:
            if not any(case["backend"] == backend and case["status"] == "PASS"
                       and case["properties"].get("gpu_validation") == "enabled" for case in cases):
                summary["success"] = False
                summary["issues"].append(f"required backend {backend} has no passing GPU-validation-enabled evidence")
    if not junit_path.exists():
        summary["success"] = False
        summary["issues"].append("CTest did not produce its JUnit report")
    for case in junit.iter("testcase"):
        log = case.findtext("system-out", "")
        if "GPU validation:" not in log:
            continue
        match = re.search(r"Google Test filter = (.+)", log)
        evidence = next((item for item in cases if match and f"{item['suite']}.{item['name']}" == match.group(1).strip()), None)
        props = evidence["properties"] if evidence else {}
        isolated = props.get("evidence_class") in ("isolated_expected_error", "isolated_collection_failure")
        expected = sum(int(props.get(name, "0")) for name in ("expected_validation_errors", "expected_collection_failures")
                       if props.get(name, "0").isdecimal())
        if not isolated or log.count("GPU validation:") != expected:
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
