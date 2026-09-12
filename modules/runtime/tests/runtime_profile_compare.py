"""Compare five independently joined raw Tracy runs without promoting incomplete totals."""
import argparse
import json
import math
from pathlib import Path
import sys
sys.dont_write_bytecode = True

from runtime_profile_trace import BOUNDARY, FIXTURE_PASSES, FIXTURE_VIEWS, PHASES, evidence, integer, quantile, read_json, summarize_metrics, summarize_phase_coverage, verify_evidence


def variation(values):
    mean = sum(values) / len(values)
    return math.sqrt(sum((value - mean) ** 2 for value in values) / len(values)) / mean if mean else 0.0


def load_rounds(directory, label, errors):
    result = []
    for path in sorted(directory.rglob("summary.json")):
        summary = read_json(path, errors)
        if summary.get("label") != label:
            continue
        if not summary.get("knownCoverageParsed") or summary.get("errors"):
            errors.append(f"Unresolved trace join errors: {path}")
        if summary.get("schemaVersion") != 3 or summary.get("boundaryContract") != BOUNDARY:
            errors.append(f"Accounting requires phase schema 3 and the current boundary contract: {path}")
        if any(not isinstance(summary.get(key), dict) for key in ("build", "options", "instrumentation", "inputs", "metrics")):
            errors.append(f"Incomplete accounting summary metadata: {path}")
            continue
        rows_path = path.with_name("frames.jsonl")
        rows = read_json(rows_path, errors, lines=True)
        verify_evidence(summary.get("accountedFrames"), rows_path, errors, "Accounted raw frames")
        warmup, samples = summary["options"].get("warmup"), summary["options"].get("samples")
        if not integer(warmup, 1) or not integer(samples, 1):
            errors.append(f"Invalid frame counts: {path}")
            continue
        if len(rows) != warmup + samples:
            errors.append(f"Truncated or extended raw frame sequence: {path}")
        serials = set()
        for index, row in enumerate(rows):
            if row.get("sampleIndex") != index or row.get("round") != 0 or type(row.get("warmup")) is not bool or row.get("warmup") != (index < warmup):
                errors.append(f"Raw frame sequence/warmup classification changed: {path}, index {index}")
            serial = row.get("frameSerial")
            if not integer(serial, 1) or serial in serials:
                errors.append(f"Invalid or duplicate frame serial: {path}, index {index}")
            if integer(serial, 1):
                serials.add(serial)
            for key, value in row.items():
                if key.endswith("Ns") and key != "detailsNotAddedToTotalNs" and value is not None and not integer(value):
                    errors.append(f"Invalid raw interval value {key}: {path}, index {index}")
            complete = (row.get("coverage") or {}).get("totalComplete") is True
            if (row.get("preparationTotalNs") is not None) != complete or (row.get("coverage") or {}).get("phase") != ("warmup" if index < warmup else "steady"):
                errors.append(f"Raw total coverage disagrees with its exact phase/value: {path}, index {index}")
            fixture = row.get("fixtureEvidence") or {}
            if any(fixture.get(key) != value for key, value in FIXTURE_VIEWS.items()) or any((fixture.get("recordedPasses") or {}).get(key) != value for key, value in FIXTURE_PASSES.items()):
                errors.append(f"Raw evidence is missing required actual views, outputs or recorded effects: {path}, index {index}")
        steady = [row for row in rows if row.get("warmup") is False]
        phases = summarize_phase_coverage(rows, summary["options"])
        if summary.get("phaseCoverage") != phases or summary.get("totalComplete") is not all(phase["totalComplete"] for phase in phases.values()) or summary.get("steadyTotalComplete") is not phases["steady"]["totalComplete"]:
            errors.append(f"Summary coverage differs from all preserved warmup/steady rows: {path}")
        if summary.get("phaseMetrics") != {phase: summarize_metrics([row for row in rows if row.get("warmup") is (phase == "warmup")]) for phase in PHASES}:
            errors.append(f"Phase metrics differ from complete preserved phase rows: {path}")
        if summary.get("joinedFrames") != len(rows) or summary.get("joinedSteadyFrames") != len(steady) or summary["metrics"] != summarize_metrics(steady):
            errors.append(f"Summary metrics differ from the preserved raw intervals: {path}")
        result.append({"summary": summary, "rows": steady, "warmupRows": [row for row in rows if row.get("warmup") is True], "evidence": evidence(path), "frames": evidence(rows_path)})
    if not result:
        errors.append(f"No {label} raw accounting summaries found in {directory}.")
    ids = [entry["summary"].get("runId") for entry in result]
    if any(value is None for value in ids) or len(ids) != len(set(ids)):
        errors.append(f"Repeated run identity in {label} evidence.")
    captures = [(entry["summary"]["inputs"].get("messages") or {}).get("sha256") for entry in result]
    if any(value is None for value in captures) or len(captures) != len(set(captures)):
        errors.append(f"Repeated capture messages in {label} evidence.")
    if result:
        first = result[0]["summary"]
        for entry in result[1:]:
            for field in ("build", "options", "instrumentation", "shaderSha256", "boundaryContract", "scenario"):
                if entry["summary"].get(field) != first.get(field):
                    errors.append(f"Changing {field} across {label} rounds.")
    return result


def compare_rounds(baseline, candidate):
    metrics = {}
    if not baseline or not candidate:
        return metrics
    shared = baseline[0]["summary"]["metrics"].keys() & candidate[0]["summary"]["metrics"].keys()
    for metric in sorted(shared):
        entry = {}
        for label, rounds in (("baseline", baseline), ("candidate", candidate)):
            values = [row.get(metric) for run in rounds for row in run["rows"]]
            known = [value for value in values if integer(value)]
            complete = bool(values) and len(known) == len(values)
            per_round = [[row.get(metric) for row in run["rows"]] for run in rounds]
            medians = [quantile(values, 50) for values in per_round] if complete and all(per_round) else []
            entry[label] = {"samples": len(values), "knownSamples": len(known), "roundP50Ns": medians, "p50Cv": variation(medians) if medians else None,
                            **{f"p{p}Ns": quantile(known, p) if complete else None for p in (50, 95, 99)},
                            "roundQuantiles": [{f"p{p}Ns": quantile(values, p) for p in (50, 95, 99)} for values in per_round] if medians else []}
        if len(entry) == 2:
            entry["ratios"] = {f"p{p}": entry["candidate"][f"p{p}Ns"] / entry["baseline"][f"p{p}Ns"] if entry["baseline"][f"p{p}Ns"] not in (None, 0) and entry["candidate"][f"p{p}Ns"] is not None else None for p in (50, 95, 99)}
            entry["stable"] = all(len(entry[label]["roundP50Ns"]) == 5 and entry[label]["p50Cv"] is not None and entry[label]["p50Cv"] <= .05 for label in ("baseline", "candidate"))
            metrics[metric] = entry
    return metrics


def make_report(baseline, candidate, errors):
    if baseline and candidate:
        old, new = baseline[0]["summary"], candidate[0]["summary"]
        for field in ("harnessSha256", "compiler", "configuration", "flags", "profilerEnabled", "detailedProfilingEnabled"):
            if old["build"].get(field) != new["build"].get(field):
                errors.append(f"Paired build {field} differs.")
        for field in ("options", "instrumentation", "shaderSha256", "boundaryContract", "scenario"):
            if old.get(field) != new.get(field):
                errors.append(f"Paired {field} differs.")
        if {run["summary"].get("runId") for run in baseline} & {run["summary"].get("runId") for run in candidate}:
            errors.append("The same invocation was used on both sides of the comparison.")
        old_rounds = {run["summary"].get("round"): run for run in baseline}
        for run in candidate:
            old_run = old_rounds.get(run["summary"].get("round"))
            old_rows = (old_run.get("warmupRows", []) + old_run["rows"]) if old_run else []
            new_rows = run.get("warmupRows", []) + run["rows"]
            if old_run and (len(old_rows) != len(new_rows) or any(old_row.get("fixtureEvidence") != new_row.get("fixtureEvidence") for old_row, new_row in zip(old_rows, new_rows))):
                errors.append(f"Paired actual draw/view/output/recorded-pass workload differs in round {run['summary'].get('round')}.")
    metrics = compare_rounds(baseline, candidate)
    conforming = not errors and all(len(rounds) == 5 and {run["summary"].get("round") for run in rounds} == set(range(5)) and
        all(len(run["rows"]) == 1000 and run["summary"]["options"].get("warmup", 0) >= 120 and run["summary"]["options"].get("samples") == 1000 and
            run["summary"]["build"].get("configuration") == "Release" and run["summary"].get("captureProvenanceVerified") is True for run in rounds) for rounds in (baseline, candidate))
    complete = conforming and all(run["summary"].get("totalComplete") is True for run in baseline + candidate)
    steady_complete = conforming and all(run["summary"].get("steadyTotalComplete") is True and
        len(run["rows"]) == 1000 and all((row.get("coverage") or {}).get("totalComplete") is True and integer(row.get("preparationTotalNs")) for row in run["rows"]) for run in baseline + candidate)
    gates = {}
    for metric, p50_limit, p95_limit in (("drawWorkBuildNs", .5, .65), ("graphMaintenanceNs", .3, .5), ("preparationTotalNs", .7, .8), ("gtRenderPreparationNs", None, 1.1)):
        item = metrics.get(metric)
        eligible = conforming and item and item["stable"] and (metric != "preparationTotalNs" or steady_complete)
        ratios = item["ratios"] if item else {}
        gates[metric] = all(ratios.get(key) is not None and ratios[key] <= limit for key, limit in (("p50", p50_limit), ("p95", p95_limit)) if limit is not None) if eligible else None
    gt = metrics.get("gtRenderPreparationNs")
    report = {"schemaVersion": 3, "protocolConforming": conforming, "totalComplete": complete, "steadyTotalComplete": steady_complete, "metrics": metrics, "errors": errors,
              "phaseCoverage": {label: [run["summary"].get("phaseCoverage") for run in rounds] for label, rounds in (("baseline", baseline), ("candidate", candidate))},
              "warmupMetrics": compare_rounds([{**run, "rows": run.get("warmupRows", [])} for run in baseline], [{**run, "rows": run.get("warmupRows", [])} for run in candidate]),
              "gtP95Within110Percent": gt["ratios"]["p95"] <= 1.1 if conforming and gt and gt["stable"] and gt["ratios"]["p95"] is not None else None,
              "preparationTotalTargetPassed": gates["preparationTotalNs"], "targetsPassed": gates, "workerActiveNs": None,
              "scope": "source-keyed instrumented GT/RT work intervals; incomplete whole-runtime coverage is not promoted to PreparationTotal",
              "inputs": {"baseline": [{"summary": run["evidence"], "frames": run["frames"]} for run in baseline], "candidate": [{"summary": run["evidence"], "frames": run["frames"]} for run in candidate]}}
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-directory", type=Path, required=True)
    parser.add_argument("--candidate-directory", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("Output already exists; prior evidence is preserved.")
    errors = []
    baseline = load_rounds(args.baseline_directory, "baseline", errors)
    candidate = load_rounds(args.candidate_directory, "candidate", errors)
    report = make_report(baseline, candidate, errors)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"protocolConforming": report["protocolConforming"], "totalComplete": report["totalComplete"], "errors": len(errors), "output": str(args.output)}))
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
