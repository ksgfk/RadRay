"""Join a single captured profile run to inclusive Tracy zones, without adding nested work twice."""
import argparse
import csv
import hashlib
import json
import re
import subprocess
import sys
from bisect import bisect_left, bisect_right
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path

MAX_NS = (1 << 63) - 1
STAGES = ("authoring_begin", "authoring_done", "world_done", "gt_ready", "rt_begin", "rt_recorded", "submit", "complete", "gt_observed")
BOUNDARY = "radray.integrated.preparation.v3"
MARKER_PREFIX = "RRP2|"


def integer(value, minimum=0):
    return type(value) is int and minimum <= value <= MAX_NS


def decimal(value, minimum=0):
    return isinstance(value, str) and re.fullmatch(r"[0-9]{1,19}", value) is not None and minimum <= int(value) <= MAX_NS


def read_json(path, errors, lines=False):
    if not path.is_file():
        errors.append(f"Missing JSON evidence: {path}")
        return [] if lines else {}
    content = path.read_text(encoding="utf-8-sig")
    # Validate the exact in-memory text using the stdlib CLI. Its failure is data, not
    # an exception that bypasses the final evidence report. One child per file, not per row.
    if lines:
        content = "[" + ",".join(line for line in content.splitlines() if line.strip()) + "]"
    checked = subprocess.run([sys.executable, "-m", "json.tool"], input=content, text=True, capture_output=True)
    if checked.returncode:
        errors.append(f"Malformed JSON evidence: {path}")
        return [] if lines else {}
    def unique_object(pairs):
        result = dict(pairs)
        if len(result) != len(pairs):
            errors.append(f"Duplicate JSON object keys in evidence: {path}")
        return result
    value = json.loads(content, object_pairs_hook=unique_object)
    if not isinstance(value, list if lines else dict):
        errors.append(f"Wrong JSON container: {path}")
        return [] if lines else {}
    if lines and any(not isinstance(row, dict) for row in value):
        errors.append(f"JSONL rows must be objects: {path}")
        return []
    return value


def normalized_source(filename):
    filename = filename.replace("\\", "/")
    marker = filename.find("modules/runtime/")
    return filename[marker:] if marker >= 0 else filename


def key_record(zone):
    return {"name": zone.name, "src_file": normalized_source(zone.file), "src_line": zone.line}


@dataclass(frozen=True)
class Zone:
    name: str
    file: str
    line: int
    thread: int
    begin: int
    end: int

    @property
    def key(self):
        return self.name, self.file, self.line


class ZoneIndex:
    def __init__(self, zones):
        self.zones = zones
        self.threads = defaultdict(list)
        self.guards = defaultdict(list)
        self.keyed = defaultdict(list)
        for zone in zones:
            self.threads[zone.thread].append(zone)
            self.keyed[(zone.name, normalized_source(zone.file))].append(zone)
            if zone.name in (GT_GUARD, RT_GUARD) and normalized_source(zone.file) == SYSTEM_FILE:
                self.guards[zone.name].append(zone)
        self.starts = {}
        for thread, values in self.threads.items():
            values.sort(key=lambda zone: zone.begin)
            self.starts[thread] = [zone.begin for zone in values]

    def within(self, thread, begin, end):
        values, starts = self.threads[thread], self.starts[thread]
        first, last = bisect_left(starts, begin), bisect_right(starts, end)
        return [zone for zone in values[first:last] if zone.end <= end]

    def overlapping(self, thread, begin, end):
        last = bisect_left(self.starts[thread], end)
        return [zone for zone in self.threads[thread][:last] if zone.end > begin]


def union_intervals(intervals):
    result = []
    for begin, end in sorted(intervals):
        if end <= begin:
            continue
        if result and begin <= result[-1][1]:
            result[-1] = result[-1][0], max(end, result[-1][1])
        else:
            result.append((begin, end))
    return result


def subtract_intervals(intervals, excluded):
    cuts = union_intervals(excluded)
    result = []
    for begin, end in union_intervals(intervals):
        cursor = begin
        for cut_begin, cut_end in cuts:
            if cut_end <= cursor:
                continue
            if cut_begin >= end:
                break
            if cut_begin > cursor:
                result.append((cursor, min(end, cut_begin)))
            cursor = max(cursor, cut_end)
            if cursor >= end:
                break
        if cursor < end:
            result.append((cursor, end))
    return result


def duration(intervals):
    return sum(end - begin for begin, end in union_intervals(intervals))


def intervals(zones):
    return [(zone.begin, zone.end) for zone in zones]


def quantile(values, percentile):
    values = sorted(values)
    return values[min(len(values) - 1, (len(values) * percentile + 99) // 100 - 1)]


GT_GUARD = "RenderSystem::PrepareFrame"
RT_GUARD = "RenderSystem::Render"
SOURCE_ROOT = "/modules/runtime/src/"
DRAW_PREFIX = "Forward::DrawWorkBuild."
UPLOAD_WORK = "RenderGraph::UploadWorkData"
PREPARE_WORK = "RenderGraph::PrepareWork"
PREPARE_PASSES = "RenderGraph::PreparePasses"
PREPARE_UPLOADS = "RenderGraph::PrepareUploads"
PUBLISH_SETS = "RenderGraph::PublishParameterSets"
VIEW_INPUTS = "ResolveViewFamilies"
LEGACY_DRAW = {"MainViewCull", "MainViewRendererLists", "BuildShadows", "MakeLitBindings", "UploadLights"}
SCENE = {"SceneSnapshotBuild", "Scene.CommitChanges", "Scene.PublishSnapshot"}
DETAILS = {"Scene.CompileStaticDraws", "Scene.ObserveLegacy", "Scene.RetainOwners", "Scene.CopySnapshotPages", "PrepareRendererList", "CreatePassSets"}
GRAPH = {"CreateRenderGraph", "ComposeGraph", "GraphFlightStorage", "RenderGraph::Compile", "RenderGraph::Realize", "RenderGraph::Prepare", "RenderGraph::PlanBarriers", "RenderGraph::PatchBarriers", "RenderGraph::PatchCommandRoutes"}
GT_PRE = {"RenderSystem::BeginUpdateForFlight", "AssetManager::Pump", "ApplicationScheduler::Pump"}
GPU_UPDATE = "Application::GpuBeginUpdateForFlight"
HISTORY = {"ViewStateRegistry::CommitView", "ViewStateRegistry::CommitViewWithHistory"}
SELECTED = SCENE | DETAILS | GRAPH | LEGACY_DRAW | GT_PRE | HISTORY | {GPU_UPDATE, GT_GUARD, RT_GUARD, UPLOAD_WORK, PREPARE_WORK, PREPARE_PASSES, PREPARE_UPLOADS, PUBLISH_SETS, VIEW_INPUTS, "ObjectValueUpdate", "FreezeObjectData", "Profile.Authoring", "Profile.PrepareObservation", "Profile.ComposeObservation", "ForwardGraph::BuildGraph", "RenderGraph::Record", "Update", "TickFrame", "PrepareFrameUploads", "ViewStateRegistry::BeginFlight", "ForwardPipeline::GraphRecorded", "RenderSystem::SubmittedFrame", "RenderSystem::PresentCommit", "RenderPipelineContext::QueueViewCommit", "PresentFinalize"}

SYSTEM_FILE = "modules/runtime/src/render_system.cpp"
GRAPH_FILE = "modules/runtime/src/render_framework/render_graph.cpp"
APPLICATION_FILE = "modules/runtime/src/application.cpp"
VIEW_STATE_FILE = "modules/runtime/src/render_framework/view_state.cpp"
SCOPE_FILES = {**{name: APPLICATION_FILE for name in ("TickFrame", "Update", "PrepareFrameUploads", "ApplicationScheduler::Pump", GPU_UPDATE)},
               **{name: SYSTEM_FILE for name in ("RenderSystem::BeginUpdateForFlight", "BeginGraphFlight", "GraphFlightStorage", "RenderSystem::SubmittedFrame", "RenderSystem::PresentCommit", "PresentFinalize")},
               **{name: VIEW_STATE_FILE for name in HISTORY | {"ViewStateRegistry::BeginFlight"}},
               "AssetManager::Pump": "modules/runtime/src/asset_manager.cpp", "RenderPipelineContext::QueueViewCommit": "modules/runtime/src/render_framework/render_pipeline.cpp",
               "ForwardPipeline::GraphRecorded": "modules/runtime/src/forward_pipeline/forward_pipeline.cpp"}
FORWARD_FILES = {"modules/runtime/src/forward_pipeline/" + name for name in ("forward_pipeline.cpp", "forward_effects.cpp", "forward_frame.cpp", "forward_graph.cpp")}
FIXTURE_VIEWS = {"resolvedViewCount": 3, "viewCount": 3, "fullViewCount": 2, "auxiliaryViewCount": 1, "outputCount": 2, "writtenOutputCount": 2}
FIXTURE_PASSES = {"Forward.DepthNormalsMotion": 2, "Forward.Opaque": 3, "Forward.Transparent": 3, "Forward.LinearDepth": 2,
                  "Forward.DepthPyramid.0": 2, "Forward.AO": 2, "Forward.AO.Horizontal": 2, "Forward.AO.Vertical": 2,
                  "Forward.TileLightCull": 3, "Forward.TAA": 2, "Forward.ToneMapAndComposite": 3, "Forward.ObserverComposite": 1,
                  **{f"Forward.Shadow.{index}": 1 for index in range(4)},
                  **{f"Forward.Bloom.Down{index}": 2 for index in range(5)}, **{f"Forward.Bloom.Up{index}": 2 for index in range(4)}}


def verify_fixture_frame(frame, errors):
    start = len(errors)
    for key, expected in FIXTURE_VIEWS.items():
        if not integer(frame.get(key)) or frame[key] != expected:
            errors.append(f"Frame {frame.get('sampleIndex')}: actual {key} must be {expected}, got {frame.get(key)}.")
    if frame.get("stageCommandsKnown") is not True or not all(integer(frame.get(key), 1) for key in ("actualMeshDraws", "depthCommands", "opaqueCommands", "transparentCommands")):
        errors.append("The three-view fixture requires known, nonzero mesh/depth/opaque/transparent command evidence.")
    if frame.get("reportExecutedPassesKnown") is True:
        if not integer(frame.get("reportExecutedPasses"), 1):
            errors.append("Full report pass evidence is missing or empty.")
    elif frame.get("reportExecutedPassesKnown") is not False or frame.get("reportExecutedPasses") is not None:
        errors.append("Unavailable report pass evidence must remain explicitly unknown/null.")
    return len(errors) == start


def verify_fixture_passes(result, zones, errors):
    # RenderGraph has same-name zones in PreparePasses and Record. Only the exact
    # joined RT thread's Record children prove live command recording in this frame.
    record = result["intervals"]["record"]
    passes = Counter(zone.name for begin, end in union_intervals(record) for zone in zones.within(result["rtThread"], begin, end)
                     if normalized_source(zone.file) == GRAPH_FILE and zone.name.startswith("Forward."))
    for name, expected in FIXTURE_PASSES.items():
        if passes[name] != expected:
            errors.append(f"Frame {result['sampleIndex']}: recorded {name} count must be {expected}, got {passes[name]}.")
    return dict(sorted(passes.items()))


def source_allowed(name, filename):
    filename = normalized_source(filename)
    if name in SCOPE_FILES:
        return filename == SCOPE_FILES[name]
    if name in (GT_GUARD, RT_GUARD, "CreateRenderGraph", "ComposeGraph", VIEW_INPUTS):
        return filename == SYSTEM_FILE
    if name.startswith("RenderGraph::"):
        return filename == GRAPH_FILE
    if name in ("Profile.Authoring", "Profile.PrepareObservation", "Profile.ComposeObservation"):
        return filename == "modules/runtime/tests/runtime_profile_integrated.h"
    if name.startswith("Forward") or name in LEGACY_DRAW | {"SceneSnapshotBuild", "ObjectValueUpdate", "FreezeObjectData"}:
        return filename in FORWARD_FILES
    if name.startswith("Scene."):
        owner = "cpu_draw_store.cpp" if name == "Scene.CompileStaticDraws" else "render_scene_snapshot.cpp"
        return filename == "modules/runtime/src/render_framework/" + owner
    return filename.startswith("modules/runtime/src/render_framework/")


def read_zones(path, errors, extra_keys=()):
    result = []
    extra = {(key["name"], key["src_file"], key["src_line"]) for key in extra_keys}
    with path.open(encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        required = {"name", "src_file", "src_line", "ns_since_start", "exec_time_ns", "thread"}
        if not required.issubset(reader.fieldnames or []):
            errors.append("Zones require the inclusive --unwrap CSV, with source line, timestamp and thread columns.")
            return result
        for row in reader:
            if any(row.get(field) is None for field in required) or None in row:
                errors.append("Malformed inclusive zone CSV row.")
                continue
            name = row["name"]
            filename = row["src_file"].replace("\\", "/")
            selected = name in SELECTED or name.startswith(DRAW_PREFIX)
            marker = name.startswith(MARKER_PREFIX)
            runtime = normalized_source(filename).startswith("modules/runtime/")
            if not selected and not marker and not runtime and name not in {key[0] for key in extra}:
                continue
            if not all(decimal(row[field], 1 if field in ("src_line", "thread") else 0) for field in ("src_line", "thread", "ns_since_start", "exec_time_ns")):
                errors.append("A selected zone has an invalid integer, negative time, or overflow.")
                continue
            key = name, normalized_source(filename), int(row["src_line"])
            if not selected and not marker and not runtime and key not in extra:
                continue
            if marker and normalized_source(filename) != "modules/runtime/tests/runtime_profile_support.h":
                errors.append("A frame marker zone came from an unexpected source.")
                continue
            if selected and not source_allowed(name, filename):
                errors.append(f"Unexpected source for a selected scope: {key}.")
                continue
            begin, elapsed = int(row["ns_since_start"]), int(row["exec_time_ns"])
            if begin + elapsed > MAX_NS:
                errors.append("A selected zone end overflows the time domain.")
                continue
            result.append(Zone(name, filename, int(row["src_line"]), int(row["thread"]), begin, begin + elapsed))
    return result


def read_markers(path, expected_build, expected_harness, expected_run, errors, zones=None):
    result = defaultdict(dict)
    anchors = defaultdict(list)
    if zones is not None:
        for zone in zones.zones:
            if zone.name.startswith(MARKER_PREFIX):
                anchors[zone.name].append(zone)
    consumed = set()
    with path.open(encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        if not {"MessageName", "total_ns"}.issubset(reader.fieldnames or []):
            errors.append("Messages require the --messages CSV.")
            return result
        for row in reader:
            if row.get("MessageName") is None or row.get("total_ns") is None or None in row:
                errors.append("Malformed message CSV row.")
                continue
            message = row["MessageName"]
            if message.startswith("RRP1|"):
                errors.append("Legacy marker messages do not prove thread attribution; rebuild both binaries with this harness.")
                continue
            if not message.startswith(MARKER_PREFIX):
                continue
            pieces = [piece.split("=", 1) for piece in message.split("|")[1:]]
            if any(len(piece) != 2 for piece in pieces):
                errors.append("Malformed trace marker fields.")
                continue
            fields = dict(pieces)
            if len(fields) != len(pieces) or set(fields) != {"index", "flight", "serial", "stage", "run", "build", "harness"}:
                errors.append("Duplicate, missing, or unknown trace marker fields.")
                continue
            if fields.get("build") != expected_build or fields.get("harness") != expected_harness or fields.get("run") != expected_run:
                errors.append("Trace marker run/build/harness identity differs from this benchmark invocation.")
                continue
            if not all(decimal(fields[field]) for field in ("index", "flight", "serial")) or not decimal(row["total_ns"]) or fields["stage"] not in STAGES:
                errors.append("Invalid trace marker stage or integer time/identity.")
                continue
            index, flight, serial = int(fields["index"]), int(fields["flight"]), int(fields["serial"])
            stage = fields["stage"]
            if stage in result[index]:
                errors.append(f"Duplicate trace marker for index {index}, stage {stage}; capture may contain multiple processes.")
            matching = anchors.get(message, [])
            time = int(row["total_ns"])
            if len(matching) != 1 or not matching[0].begin <= time <= matching[0].end:
                errors.append(f"Marker {index}/{stage} lacks exactly one matching inclusive zone/thread anchor.")
                continue
            consumed.add(message)
            result[index][stage] = {"time": time, "flight": flight, "serial": serial, "thread": matching[0].thread}
    if set(anchors) != consumed:
        errors.append("Marker zones and messages do not cover the same exact run/stage sequence.")
    return result


def enclosing(zones, name, time, errors, index):
    found = [zone for zone in zones.guards[name] if zone.begin <= time <= zone.end]
    if len(found) != 1:
        errors.append(f"Frame index {index}: expected one enclosing source-keyed {name}, found {len(found)}.")
        return None
    return found[0]


AUDIT_ASSERTIONS = ("gtGuardCoversRenderPreparation", "fixtureWorldAndAuthoringContainOnlyRenderingWork", "assetsReadyBeforeMeasurement", "noPreparationOutsideGtAndRt", "noUninstrumentedBlockingWaits", "drawScopesCoverAllBindingAndUploadWork", "harnessObservationExcluded", "gtUpdateAndUploadsContainOnlyRenderingWork", "flightRetirementAccountingReviewed", "submittedContinuationSeparated")
PHASES = ("warmup", "steady")


def validate_audit(audit, metadata, errors, source_root=None):
    phases = {phase: {"audit": None, "issues": ["No reviewed coverage audit for this phase."]} for phase in PHASES}
    if not audit:
        return phases
    start = len(errors)
    if audit.get("schemaVersion") != 3 or audit.get("boundaryContract") != BOUNDARY:
        errors.append("Coverage audit requires phase schema 3 and the supported boundary contract.")
    for field in ("runtimeSourcesSha256", "harnessSha256"):
        if audit.get(field) != metadata.get("build", {}).get(field):
            errors.append(f"Coverage audit {field} differs from this build.")
    if audit.get("snapshotMode") != metadata.get("phaseContract", {}).get("snapshotMode") or audit.get("options") != metadata.get("options"):
        errors.append("Coverage audit mode/options differ from this exact fixture configuration.")
    reviews = audit.get("phaseReviews")
    if not isinstance(reviews, dict) or set(reviews) != set(PHASES):
        errors.append("Coverage audit must contain exactly warmup and steady phase reviews; custom ranges are unsupported.")
        return phases
    if set(audit) != {"schemaVersion", "boundaryContract", "runtimeSourcesSha256", "harnessSha256", "snapshotMode", "options", "phaseReviews"}:
        errors.append("Coverage audit has unknown fields or frame selectors; phase membership comes only from the raw sequence.")
    if len(errors) != start:
        return phases
    for phase in PHASES:
        review = reviews[phase]
        if not isinstance(review, dict) or review.get("status") not in ("unreviewed", "reviewed"):
            errors.append(f"Malformed {phase} coverage review status.")
            continue
        if set(review) != {"status", "reviewer", "workerMode", "waitSourceKeys", "assertions", "sourceKeys"}:
            errors.append(f"Unknown {phase} review fields or frame selectors.")
            continue
        if review["status"] == "unreviewed":
            continue
        issues = []
        if validate_phase_review(review, issues, source_root):
            phases[phase] = {"audit": review, "issues": []}
        else:
            phases[phase]["issues"] = issues
    return phases


def validate_phase_review(audit, errors, source_root):
    start = len(errors)
    if not isinstance(audit.get("reviewer"), str) or not audit.get("reviewer", "").strip():
        errors.append("Reviewed phase must name its source/configuration reviewer.")
    root = Path(source_root).resolve() if source_root else None
    if root is None or not root.is_dir():
        errors.append("Coverage review must resolve its pinned source snippets in this run's source tree.")
    # A digest proves which text was reviewed, never that its semantics are covered.
    assertions = audit.get("assertions")
    if not isinstance(assertions, dict):
        errors.append("Coverage audit assertions must be an object.")
        return False
    for name in AUDIT_ASSERTIONS:
        item = assertions.get(name, {})
        if not isinstance(item, dict):
            errors.append(f"Malformed coverage audit assertion: {name}.")
            continue
        refs = item.get("codeEvidence", [])
        if item.get("confirmed") is not True or not isinstance(refs, list) or not refs or any(not isinstance(ref, dict) or not str(ref.get("file", "")).startswith("modules/") or not integer(ref.get("line"), 1) or len(str(ref.get("reason", "")).strip()) < 20 for ref in refs):
            errors.append(f"Coverage audit lacks explicit source/configuration reasoning: {name}.")
            continue
        for ref in refs:
            path = (root / ref["file"]).resolve() if root else None
            if path is None or not path.is_relative_to(root) or not path.is_file() or not integer(ref.get("endLine"), ref["line"]):
                errors.append(f"Coverage audit source range is unavailable or outside the source tree: {name}.")
                continue
            lines = path.read_text(encoding="utf-8-sig").splitlines()
            if ref["endLine"] > len(lines):
                errors.append(f"Coverage audit source range exceeds the file: {name}.")
                continue
            snippet = "\n".join(lines[ref["line"] - 1:ref["endLine"]]) + "\n"
            if ref.get("snippetSha256") != hashlib.sha256(snippet.encode("utf-8")).hexdigest():
                errors.append(f"Coverage audit reviewed source snippet has changed: {name}.")
    if audit.get("workerMode") != "none-after-source-review":
        errors.append("Independent preparation workers are not yet joined by this schema; total remains unknown.")
    if not isinstance(audit.get("sourceKeys"), list) or not audit.get("sourceKeys"):
        errors.append("Coverage audit must enumerate the exact measured name/file/line source keys.")
        return False
    if not isinstance(audit.get("waitSourceKeys", []), list):
        errors.append("Coverage audit waitSourceKeys must be an array.")
        return False
    for key in audit.get("sourceKeys", []) + audit.get("waitSourceKeys", []):
        if not isinstance(key, dict) or not isinstance(key.get("name"), str) or not str(key.get("src_file", "")).startswith("modules/") or not integer(key.get("src_line"), 1) or len(str(key.get("reason", "")).strip()) < 20:
            errors.append("Malformed source key in coverage audit.")
    return len(errors) == start


def audit_template(metadata, zones):
    return {"schemaVersion": 3, "boundaryContract": BOUNDARY,
            **{key: metadata["build"][key] for key in ("runtimeSourcesSha256", "harnessSha256")},
            "snapshotMode": metadata["phaseContract"]["snapshotMode"], "options": metadata["options"],
            "phaseReviews": {phase: {"status": "unreviewed", "reviewer": "", "workerMode": "unknown", "waitSourceKeys": [],
            "assertions": {name: {"confirmed": False, "codeEvidence": [{"file": "", "line": 0, "endLine": 0, "snippetSha256": "", "reason": ""}]} for name in AUDIT_ASSERTIONS},
            "sourceKeys": [{**dict(zip(("name", "src_file", "src_line"), key)), "reason": ""} for key in sorted({(zone.name, normalized_source(zone.file), zone.line) for zone in zones if not zone.name.startswith(MARKER_PREFIX)})]} for phase in PHASES}}


def join_host_boundaries(frame, times, zones, gt, rt, errors):
    def single(name, predicate):
        filename = SCOPE_FILES.get(name, SYSTEM_FILE)
        found = [zone for zone in zones.keyed[(name, filename)] if predicate(zone)]
        if len(found) != 1:
            errors.append(f"Frame {frame['sampleIndex']}: expected one exact source/thread {name} boundary, found {len(found)}.")
            return None
        return found[0]

    update = single("Update", lambda zone: zone.thread == gt.thread and zone.begin <= times["authoring_begin"] and gt.end <= zone.end)
    if update is None:
        return None
    tick = single("TickFrame", lambda zone: zone.thread == gt.thread and zone.begin <= update.begin and update.end <= zone.end)
    if tick is None:
        return None
    gpu_update = single(GPU_UPDATE, lambda zone: zone.thread == gt.thread and tick.begin <= zone.begin and zone.end <= tick.end)
    uploads = single("PrepareFrameUploads", lambda zone: zone.thread == gt.thread and tick.begin <= zone.begin and zone.end <= tick.end)
    pre = {name: single(name, lambda zone: zone.thread == gt.thread and update.begin <= zone.begin and zone.end <= update.end) for name in sorted(GT_PRE)}
    if gpu_update is None or uploads is None or any(zone is None for zone in pre.values()):
        return None
    pre_order = [gpu_update.begin, gpu_update.end, update.begin]
    for name in ("RenderSystem::BeginUpdateForFlight", "AssetManager::Pump", "ApplicationScheduler::Pump"):
        pre_order.extend((pre[name].begin, pre[name].end))
    pre_order.extend((times["authoring_begin"], gt.end, update.end, uploads.begin, uploads.end, rt.begin))
    if pre_order != sorted(pre_order):
        errors.append(f"Frame {frame['sampleIndex']}: GT retirement/pumps/update/upload sequence violates this frame's handoff.")
        return None
    # An outer TickFrame can wait for a slot. We count the bound work phases, not
    # its elapsed wall interval, and never match another frame's nearest upload.
    gt_guards = [zone for zone in zones.guards[GT_GUARD] if zone.thread == gt.thread and update.begin <= zone.begin and zone.end <= update.end]
    if gt_guards != [gt]:
        errors.append(f"Frame {frame['sampleIndex']}: Update contains ambiguous renderer preparation guards.")
        return None
    inside_rt = lambda zone: zone.thread == rt.thread and rt.begin <= zone.begin and zone.end <= rt.end
    storage = single("GraphFlightStorage", inside_rt)
    retirement = single("ViewStateRegistry::BeginFlight", inside_rt)
    begin_flight = single("BeginGraphFlight", inside_rt)
    recorded = single("ForwardPipeline::GraphRecorded", inside_rt)
    finalize = single("PresentFinalize", inside_rt)
    submitted = single("RenderSystem::SubmittedFrame", lambda zone: zone.thread == rt.thread and zone.begin <= times["submit"] <= zone.end)
    if any(zone is None for zone in (storage, retirement, begin_flight, recorded, finalize, submitted)):
        return None
    if not (begin_flight.begin <= storage.begin <= storage.end <= retirement.begin <= retirement.end <= begin_flight.end <= times["rt_begin"] and
            recorded.end <= times["rt_recorded"] <= finalize.begin and rt.end <= submitted.begin <= times["submit"] <= submitted.end <= times["complete"]):
        errors.append(f"Frame {frame['sampleIndex']}: RT flight/record/finalize/submitted boundaries are not one causal frame.")
        return None
    commit = single("RenderSystem::PresentCommit", lambda zone: zone.thread == rt.thread and submitted.begin <= zone.begin and zone.end <= submitted.end)
    history = [zone for name in HISTORY for zone in zones.keyed[(name, VIEW_STATE_FILE)] if zone.thread == rt.thread and submitted.begin <= zone.begin and zone.end <= submitted.end]
    if commit is None or len(history) != frame.get("viewCount") or any(zone.begin < times["submit"] or zone.end > commit.begin for zone in history):
        errors.append(f"Frame {frame['sampleIndex']}: submitted history/output continuation lacks exact per-view evidence.")
        return None
    return {"update": update, "gpuUpdate": gpu_update, "tick": tick, "uploads": uploads, "pre": pre, "storage": storage, "retirement": retirement,
            "recorded": recorded, "finalize": finalize, "submitted": submitted, "history": history, "commit": commit}


def account_frame(frame, marker, zones, mode, period, errors, audit=None, coverage_errors=None):
    initial_errors = len(errors)
    coverage_errors = errors if coverage_errors is None else coverage_errors
    initial_coverage_errors = len(coverage_errors)
    index = frame["round"] * period + frame["sampleIndex"]
    required = set(STAGES)
    if not required.issubset(marker):
        errors.append(f"Frame index {index}: missing trace stages {sorted(required - marker.keys())}.")
        return None
    if any(item["flight"] != frame["flightIndex"] for item in marker.values()):
        errors.append(f"Frame index {index}: trace flight differs from the raw frame.")
    for stage in ("rt_begin", "rt_recorded", "submit", "complete", "gt_observed"):
        if marker[stage]["serial"] != frame["frameSerial"]:
            errors.append(f"Frame index {index}: {stage} serial differs from the raw frame.")
    times = {name: item["time"] for name, item in marker.items()}
    ordered = [times[name] for name in STAGES]
    if ordered != sorted(ordered):
        errors.append(f"Frame index {index}: callback/stage marker order is invalid.")
        return None
    gt = enclosing(zones, GT_GUARD, times["gt_ready"], errors, index)
    rt = enclosing(zones, RT_GUARD, times["rt_begin"], errors, index)
    if gt is None or rt is None:
        return None
    for stage in ("authoring_begin", "authoring_done", "world_done", "gt_ready", "gt_observed"):
        if marker[stage].get("thread") != gt.thread:
            errors.append(f"Frame index {index}: {stage} marker is not on its GT guard thread.")
    for stage in ("rt_begin", "rt_recorded", "submit"):
        if marker[stage].get("thread") != rt.thread:
            errors.append(f"Frame index {index}: {stage} marker is not on its RT guard thread.")
    if gt.begin < times["world_done"] or gt.end > rt.begin or rt.end > times["submit"]:
        errors.append(f"Frame index {index}: full GT/RT guard boundaries violate the frame handoff.")
    host = join_host_boundaries(frame, times, zones, gt, rt, errors)
    if host is None:
        return None
    gt_zones = zones.within(gt.thread, gt.begin, gt.end)
    rt_zones = zones.within(rt.thread, rt.begin, rt.end)
    scene_names = {"SceneSnapshotBuild"} if mode == "legacy" else {"Scene.CommitChanges", "Scene.PublishSnapshot"}
    object_name = "FreezeObjectData" if mode == "legacy" else "ObjectValueUpdate"
    scene = intervals(zone for zone in gt_zones if zone.name in scene_names)
    objects = intervals(zone for zone in gt_zones if zone.name == object_name)
    for expected in scene_names | {object_name}:
        if not any(zone.name == expected for zone in gt_zones):
            errors.append(f"Frame index {index}: missing preparation scope {expected}.")
    authoring = intervals(zone for zone in zones.within(gt.thread, times["authoring_begin"], times["authoring_done"]) if zone.name == "Profile.Authoring")
    if len(authoring) != 1:
        errors.append(f"Frame index {index}: expected one fixture authoring batch.")
    world = [(times["authoring_done"], times["world_done"])]
    typed_draw = [zone for zone in rt_zones if zone.name.startswith(DRAW_PREFIX)]
    legacy_draw = [zone for zone in rt_zones if zone.name in LEGACY_DRAW]
    uploads = [zone for zone in rt_zones if zone.name == UPLOAD_WORK and normalized_source(zone.file) == GRAPH_FILE]
    prepare_work = [zone for zone in rt_zones if zone.name == PREPARE_WORK and normalized_source(zone.file) == GRAPH_FILE]
    prepare_passes = [zone for zone in rt_zones if zone.name == PREPARE_PASSES and normalized_source(zone.file) == GRAPH_FILE]
    prepare_uploads = [zone for zone in rt_zones if zone.name == PREPARE_UPLOADS and normalized_source(zone.file) == GRAPH_FILE]
    publish_sets = [zone for zone in rt_zones if zone.name == PUBLISH_SETS and normalized_source(zone.file) == GRAPH_FILE]
    view_inputs = [zone for zone in rt_zones if zone.name == VIEW_INPUTS and normalized_source(zone.file) == SYSTEM_FILE]
    draw = intervals(typed_draw + legacy_draw + uploads + prepare_work + prepare_passes + prepare_uploads + publish_sets + view_inputs)
    # Old BuildShadows mixes numeric inputs and list work with graph declarations.
    # Its nested ForwardGraph::BuildGraph belongs to GraphMaintenance, never DrawWorkBuild.
    shadows = [zone for zone in legacy_draw if zone.name == "BuildShadows"]
    shadow_declarations = [zone for zone in rt_zones if zone.name == "ForwardGraph::BuildGraph" and any(parent.begin <= zone.begin and zone.end <= parent.end for parent in shadows)]
    declaration_only = subtract_intervals(intervals(shadow_declarations), intervals(typed_draw))
    draw = subtract_intervals(draw, declaration_only)
    if not any(zone.name == DRAW_PREFIX + "Ready" for zone in typed_draw):
        errors.append(f"Frame index {index}: missing full pass-set and native Ready preparation scope.")
    if not any(zone.name == DRAW_PREFIX + "Prepare" for zone in typed_draw) and not legacy_draw:
        errors.append(f"Frame index {index}: missing live draw preparation scope.")
    rt_observation = intervals(zone for zone in rt_zones if zone.name == "Profile.ComposeObservation")
    graph = subtract_intervals(intervals(zone for zone in rt_zones if zone.name in GRAPH), draw + rt_observation + intervals([host["retirement"]]))
    record = intervals(zone for zone in rt_zones if zone.name == "RenderGraph::Record")
    gt_observation = intervals(zone for zone in gt_zones if zone.name == "Profile.PrepareObservation")
    gt_pre = intervals(host["pre"].values()) + intervals([host["gpuUpdate"]])
    gt_uploads = intervals([host["uploads"]])
    gt_work = subtract_intervals(authoring + world + [(gt.begin, gt.end)] + gt_pre + gt_uploads, gt_observation)
    gt_other = subtract_intervals([(gt.begin, gt.end)], scene + objects + gt_observation)
    # Sum disjoint intervals on each thread. Across threads this is work-span accounting,
    # not a wall-clock critical path or operating-system CPU scheduling measurement.
    work_by_thread = defaultdict(list)
    work_by_thread[gt.thread].extend(gt_work)
    work_by_thread[rt.thread].extend(draw)
    work_by_thread[rt.thread].extend(intervals([host["retirement"]]))
    measured = sum(duration(spans) for spans in work_by_thread.values())
    submitted_zones = zones.within(rt.thread, host["submitted"].begin, host["submitted"].end)
    selected = [zone for zone in zones.within(gt.thread, host["tick"].begin, host["tick"].end) + rt_zones + submitted_zones if not zone.name.startswith(MARKER_PREFIX)]
    waits_by_thread = defaultdict(list)
    total_complete = bool(audit)
    if audit:
        expected = [(PREPARE_PASSES, prepare_passes), (PREPARE_UPLOADS, prepare_uploads)]
        if mode == "scene-publication":
            expected.append((PREPARE_WORK, prepare_work))
        for name, scopes in expected:
            if len(scopes) != 1:
                total_complete = False
                coverage_errors.append(f"Frame index {index}: missing or ambiguous full live preparation boundary: {name}.")
        approved = {(key["name"], key["src_file"], key["src_line"]) for key in audit["sourceKeys"]}
        wait_keys = {(key["name"], key["src_file"], key["src_line"]) for key in audit.get("waitSourceKeys", [])}
        for zone in selected:
            key = zone.name, normalized_source(zone.file), zone.line
            if key in wait_keys:
                waits_by_thread[zone.thread].append((zone.begin, zone.end))
            elif key not in approved:
                total_complete = False
                coverage_errors.append(f"Frame index {index}: source key is absent from the reviewed coverage audit: {key}.")
        # Reject a future migrated work scope outside the guards rather than silently
        # dropping it. This schema deliberately has no guessed worker attribution.
        for thread in zones.threads:
            if thread not in (gt.thread, rt.thread) and any(normalized_source(zone.file).startswith("modules/runtime/src/") for zone in zones.overlapping(thread, host["gpuUpdate"].begin, host["submitted"].end)):
                total_complete = False
                coverage_errors.append(f"Frame index {index}: unjoined runtime worker activity overlaps this frame on thread {thread}.")
            for zone in zones.within(thread, host["gpuUpdate"].begin, host["submitted"].end):
                if zone.name.startswith(DRAW_PREFIX) or zone.name == UPLOAD_WORK:
                    if not ((zone.thread == rt.thread and rt.begin <= zone.begin and zone.end <= rt.end) or
                            (zone.thread == gt.thread and any(begin <= zone.begin and zone.end <= end for begin, end in gt_work))):
                        total_complete = False
                        coverage_errors.append(f"Frame index {index}: preparation work escaped the reviewed GT/RT preparation boundaries.")
    work_active = {thread: subtract_intervals(spans, waits_by_thread[thread]) for thread, spans in work_by_thread.items()}
    total_complete = total_complete and len(errors) == initial_errors and len(coverage_errors) == initial_coverage_errors
    total = sum(duration(spans) for spans in work_active.values()) if total_complete else None
    details = defaultdict(list)
    for zone in selected:
        if zone.name in DETAILS:
            details[zone.name].append((zone.begin, zone.end))
    return {
        "sampleIndex": frame["sampleIndex"], "round": frame["round"], "frameSerial": frame["frameSerial"], "flightIndex": frame["flightIndex"], "warmup": frame["warmup"],
        "gtThread": gt.thread, "rtThread": rt.thread,
        "authoringRenderExtraNs": duration(authoring), "fixtureWorldTickNs": duration(world),
        "sceneCommitAndPublishNs": duration(scene), "objectValueUpdateNs": duration(objects), "drawWorkBuildNs": duration(draw),
        "uploadWorkDataNs": duration(intervals(uploads)) if uploads else None,
        "liveWorkPreparationNs": duration(intervals(prepare_work)) if prepare_work else None,
        "livePassPreparationNs": duration(intervals(prepare_passes)) if prepare_passes else None,
        "liveUploadPreparationNs": duration(intervals(prepare_uploads)) if prepare_uploads else None,
        "parameterSetPublicationNs": duration(intervals(publish_sets)) if publish_sets else None,
        "viewInputsNs": duration(intervals(view_inputs)) if view_inputs else None,
        "gtFlightRetirementNs": host["pre"]["RenderSystem::BeginUpdateForFlight"].end - host["pre"]["RenderSystem::BeginUpdateForFlight"].begin,
        "assetPumpNs": host["pre"]["AssetManager::Pump"].end - host["pre"]["AssetManager::Pump"].begin,
        "applicationSchedulerPumpNs": host["pre"]["ApplicationScheduler::Pump"].end - host["pre"]["ApplicationScheduler::Pump"].begin,
        "gtFrameUploadPreparationNs": duration(gt_uploads), "gtUpdateEnvelopeNs": host["update"].end - host["update"].begin,
        "gtGpuFlightUpdateNs": host["gpuUpdate"].end - host["gpuUpdate"].begin,
        "graphFlightStorageNs": host["storage"].end - host["storage"].begin,
        "rtViewFlightRetirementNs": host["retirement"].end - host["retirement"].begin,
        "postRecordBookkeepingNs": host["recorded"].end - host["recorded"].begin,
        "viewCommitQueueNs": duration(intervals(zone for zone in rt_zones if zone.name == "RenderPipelineContext::QueueViewCommit")),
        "presentFinalizeNs": host["finalize"].end - host["finalize"].begin,
        "postSubmitHistoryNs": duration(intervals(host["history"])),
        "postSubmitOutputCommitNs": host["commit"].end - host["commit"].begin,
        "submittedContinuationNs": host["submitted"].end - times["submit"],
        "inputToSubmissionContinuationEndNs": host["submitted"].end - times["authoring_begin"],
        "gtPreparationOtherNs": duration(gt_other),
        "excludedHarnessObservationNs": duration(gt_observation),
        "excludedHarnessComposeObservationNs": duration(rt_observation),
        "measuredPreparationWorkNs": measured, "preparationTotalNs": total, "workerActiveNs": None,
        "independentWorkerWorkSpanNs": 0 if total_complete else None,
        "excludedBlockingWaitNs": sum(duration(spans) - duration(work_active[thread]) for thread, spans in work_by_thread.items()) if total_complete else None,
        "gtPrepareGuardNs": gt.end - gt.begin,
        "gtRenderPreparationNs": duration(gt_work),
        "rtRenderNs": rt.end - rt.begin, "graphMaintenanceNs": duration(graph), "recordNs": duration(record),
        "inputToSubmitNs": times["submit"] - times["authoring_begin"], "inputToCompletionObservedNs": times["complete"] - times["authoring_begin"],
        "detailsNotAddedToTotalNs": {name: duration(spans) for name, spans in details.items()},
        "hostBoundaries": {name: {**key_record(zone), "thread": zone.thread, "begin": zone.begin, "end": zone.end} for name, zone in
                           {**{key: value for key, value in host.items() if isinstance(value, Zone)}, **host["pre"]}.items()},
        "intervals": {"gtPreparation": gt_work, "drawWork": union_intervals(draw), "graphMaintenance": graph, "record": union_intervals(record),
                      "rtViewRetirement": intervals([host["retirement"]]), "postSubmitHistory": union_intervals(intervals(host["history"])),
                      "preparationByThread": {str(thread): spans for thread, spans in work_active.items()}},
        "coverage": {"phase": "warmup" if frame["warmup"] else "steady", "issues": list(coverage_errors),
                     "worker": "none-after-source-review" if total_complete else "unknown", "cpuSchedulingSamples": None,
                     "world": "entire fixture World::Tick interval; not arbitrary game-logic attribution", "totalComplete": total_complete},
    }


def evidence(path):
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    return {"path": str(path.resolve()), "bytes": path.stat().st_size, "sha256": digest}


def verify_evidence(item, path, errors, description):
    if not isinstance(item, dict) or not path.is_file():
        errors.append(f"Missing {description} evidence.")
        return False
    observed = evidence(path)
    valid = str(Path(item.get("path", "")).resolve()).casefold() == str(path.resolve()).casefold() and str(item.get("sha256", "")).lower() == observed["sha256"] and item.get("bytes") == observed["bytes"]
    if not valid:
        errors.append(f"{description} path/size/SHA differs from the preserved run evidence.")
    return valid


def verify_connection(trace, errors):
    start = len(errors)
    connection = trace.get("connection") or {}
    client, capture = connection.get("client") or {}, connection.get("capture") or {}
    port = connection.get("port")
    if not integer(port, 1) or port > 65535:
        errors.append("Capture connection port is invalid.")
    for endpoint, child_name in ((client, "client"), (capture, "captureProcess")):
        child = trace.get(child_name) or {}
        if endpoint.get("pid") != child.get("pid") or endpoint.get("processStartTicks") != child.get("processStartTicks") or endpoint.get("state") != "Established":
            errors.append("Capture TCP endpoints do not belong to the originally owned process identities.")
        if endpoint.get("localAddress") != "127.0.0.1" or endpoint.get("remoteAddress") != "127.0.0.1":
            errors.append("Capture connection was not the recorded localhost pair.")
    if client.get("pid") == capture.get("pid") or client.get("localPort") != port or capture.get("remotePort") != port or not integer(client.get("remotePort"), 1) or client.get("remotePort", 65536) > 65535 or client.get("remotePort") != capture.get("localPort"):
        errors.append("Capture TCP endpoint ports/processes do not form the same connection.")
    times = [connection.get("observedUtc"), connection.get("permitWrittenUtc")]
    children = [trace.get(name) or {} for name in ("client", "captureProcess")]
    times += [child.get(field) for child in children for field in ("startedUtc", "completedUtc")]
    if not all(isinstance(value, str) and re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{7}Z", value) for value in times):
        errors.append("Capture connection/permit/process timestamps are absent or malformed.")
    elif times[0] > times[1] or any(not child["startedUtc"] <= times[0] <= times[1] <= child["completedUtc"] for child in children):
        errors.append("Capture startup permit was not issued during both owned process lifetimes after connection verification.")
    return start == len(errors)


def verify_capture(run, manifest, zones_path, messages_path, errors):
    trace = run.get("trace")
    if not trace:
        return False
    start = len(errors)
    if trace.get("runId") != run.get("runId") or trace.get("connectionMatched") is not True:
        errors.append("Captured run does not have its own verified startup connection/run identity.")
    for name in ("client", "captureProcess"):
        child = trace.get(name) or {}
        if child.get("exitCode") != 0 or child.get("terminated") is not False or child.get("identityVerified") is not True or not integer(child.get("pid"), 1) or not integer(child.get("processStartTicks"), 1):
            errors.append(f"Captured {name} did not finish as the originally owned process.")
    verify_connection(trace, errors)
    permit = trace.get("permit") or {}
    permit_path = Path(permit.get("path", ""))
    if verify_evidence(permit, permit_path, errors, "This run startup permit") and permit_path.read_text(encoding="utf-8") != run.get("runId"):
        errors.append("Startup permit contents differ from this run identity.")
    capture = trace.get("capture") or {}
    capture_path = Path(capture.get("path", ""))
    verify_evidence(capture, capture_path, errors, "This run capture")
    verify_evidence(trace.get("zones"), zones_path, errors, "This run inclusive zones")
    verify_evidence(trace.get("messages"), messages_path, errors, "This run messages")
    expected = [(["--unwrap", str(capture_path)], str(zones_path)), (["--messages", str(capture_path)], str(messages_path))]
    exports = trace.get("exports", [])
    if len(exports) != 2:
        errors.append("Captured run must have exactly two exports from the same trace.")
    else:
        for item, (arguments, output) in zip(exports, expected):
            child = item.get("process") or {}
            if item.get("arguments") != arguments or item.get("output") != output or item.get("exitCode") != 0 or child.get("arguments") != arguments or child.get("exitCode") != 0 or child.get("terminated") is not False or child.get("identityVerified") is not True:
                errors.append("Export provenance must use this trace with inclusive --unwrap and --messages, without filters or --self.")
    tools = manifest.get("tracy", {}).get("evidence", {})
    for name in ("captureTool", "exportTool"):
        tool = tools.get(name) or {}
        if tool.get("verified") is not True or "v" + str(tool.get("version")) != run.get("sourceBefore", {}).get("tracyTag"):
            errors.append(f"{name} version is unverified or does not match the client's dependency.")
        file = tool.get("file") or {}
        verify_evidence(file, Path(file.get("path", "")), errors, name)
    if str((trace.get("client") or {}).get("executable", "")).casefold() != str((run.get("binary") or {}).get("path", "")).casefold():
        errors.append("Captured client executable differs from the measured binary.")
    if (trace.get("captureProcess") or {}).get("arguments") != ["-a", "127.0.0.1", "-p", str((trace.get("connection") or {}).get("port")), "-o", str(capture_path)]:
        errors.append("Capture process arguments do not target this run's trace and localhost port.")
    for child, tool_name in [(trace.get("captureProcess") or {}, "captureTool")] + [(item.get("process") or {}, "exportTool") for item in exports]:
        if str(child.get("executable", "")).casefold() != str((tools.get(tool_name) or {}).get("file", {}).get("path", "")).casefold():
            errors.append("Capture/export child executable differs from its verified tool.")
    return len(errors) == start


def validate_frames(frames, timelines, options, markers, errors):
    warmup, samples = options.get("warmup"), options.get("samples")
    if not integer(warmup, 1) or not integer(samples, 1) or options.get("rounds") != 1 or not integer(options.get("flights"), 1):
        errors.append("Run options require positive warmup/samples/flights and exactly one round.")
        return []
    count = warmup + samples
    if len(frames) != count or len(timelines) != count or set(markers) != set(range(count)):
        errors.append("Raw frames, callbacks and marker indices must each cover the exact warmup+steady sequence.")
    by_serial = {}
    for row in timelines:
        serial = row.get("frameSerial")
        if not integer(serial, 1) or serial in by_serial:
            errors.append("Callback serial is invalid or duplicated.")
        else:
            by_serial[serial] = row
    accepted, seen = [], set()
    for index, frame in enumerate(frames):
        if frame.get("sampleIndex") != index or frame.get("round") != 0 or type(frame.get("warmup")) is not bool or frame.get("warmup") != (index < warmup):
            errors.append(f"Frame {index} has a duplicated/missing index or invalid warmup classification.")
            continue
        serial, flight = frame.get("frameSerial"), frame.get("flightIndex")
        if not integer(serial, 1) or serial in seen or not integer(flight) or flight >= options["flights"]:
            errors.append(f"Frame {index} has an invalid/duplicated serial or flight.")
            continue
        seen.add(serial)
        timeline = by_serial.get(serial, {})
        raw_times = [frame.get("beginNs"), frame.get("recordedNs"), timeline.get("submitCallbackNs"), timeline.get("completionCallbackNs"), frame.get("retireObservedNs")]
        if timeline.get("flightIndex") != flight or timeline.get("round") != 0 or timeline.get("submissionCallbacksAvailable") is not True or timeline.get("completionSucceeded") is not True or not all(integer(time, 1) for time in raw_times):
            errors.append(f"Frame {index} lacks valid successful callbacks with matching frame/flight identity.")
            continue
        if raw_times != sorted(raw_times) or timeline.get("inputToSubmitNs") != raw_times[2] - raw_times[0]:
            errors.append(f"Frame {index} callback clock is non-monotonic or inconsistent.")
            continue
        if not verify_fixture_frame(frame, errors):
            continue
        accepted.append(frame)
    return accepted


def summarize_metrics(rows):
    metrics = {}
    for key in sorted({key for row in rows for key in row if key.endswith("Ns")}):
        values = [row.get(key) for row in rows]
        known = [value for value in values if integer(value)]
        metrics[key] = {"samples": len(values), "knownSamples": len(known), **{f"p{p}Ns": quantile(known, p) if known and len(known) == len(values) else None for p in (50, 95, 99)}}
    return metrics


def summarize_phase_coverage(rows, options):
    result = {}
    for phase in PHASES:
        selected = [row for row in rows if row.get("warmup") is (phase == "warmup")]
        expected = options.get("warmup" if phase == "warmup" else "samples")
        complete = sum((row.get("coverage") or {}).get("totalComplete") is True and integer(row.get("preparationTotalNs")) for row in selected)
        result[phase] = {"frames": len(selected), "expectedFrames": expected, "completeFrames": complete,
                         "totalComplete": integer(expected, 1) and len(selected) == expected and complete == expected,
                         "issues": sorted({issue for row in selected for issue in (row.get("coverage") or {}).get("issues", [])})}
    return result


def finalize_coverage(rows, options, errors, capture_verified):
    warmup, samples = options.get("warmup"), options.get("samples")
    if not integer(warmup, 1) or not integer(samples, 1) or len(rows) != warmup + samples:
        errors.append("Joined rows must preserve the complete warmup+steady frame sequence.")
    else:
        for index, row in enumerate(rows):
            phase = "warmup" if index < warmup else "steady"
            if row.get("sampleIndex") != index or row.get("round") != 0 or type(row.get("warmup")) is not bool or row["warmup"] != (phase == "warmup") or row.get("coverage", {}).get("phase") != phase:
                errors.append(f"Joined row {index} has changed its exact phase or frame identity.")
    if errors or not capture_verified:
        for row in rows:
            row["preparationTotalNs"] = None
            row["independentWorkerWorkSpanNs"] = None
            row["excludedBlockingWaitNs"] = None
            row["coverage"]["totalComplete"] = False
            row["coverage"]["worker"] = "unknown"
            row["coverage"].setdefault("issues", []).append("Run-level evidence/join errors or missing capture provenance invalidate totals in both phases.")
    return summarize_phase_coverage(rows, options)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--label", choices=("candidate", "baseline"), required=True)
    parser.add_argument("--round", type=int, default=0)
    parser.add_argument("--zones", type=Path, required=True, help="Inclusive tracy-csvexport --unwrap output; never --self.")
    parser.add_argument("--messages", type=Path, required=True)
    parser.add_argument("--coverage-audit", type=Path, help="Reviewed source/configuration coverage, pinned to this runtime and harness.")
    parser.add_argument("--output-directory", type=Path, required=True)
    args = parser.parse_args()
    for path in (args.manifest, args.zones, args.messages):
        if not path.is_file():
            parser.error(f"Missing input: {path}")
    if args.output_directory.exists() and any(args.output_directory.iterdir()):
        parser.error("Output directory must be empty; existing evidence is preserved.")
    args.output_directory.mkdir(parents=True, exist_ok=True)
    errors = []
    manifest = read_json(args.manifest, errors)
    runs = [run for run in manifest.get("runs", []) if isinstance(run, dict) and run.get("label") == args.label and run.get("round") == args.round]
    if len(runs) != 1:
        errors.append("Manifest must contain exactly one matching run.")
    run = runs[0] if len(runs) == 1 else {}
    metadata = run.get("metadata") or {}
    mode = (metadata.get("phaseContract") or {}).get("snapshotMode")
    valid_metadata = all(isinstance(metadata.get(key), dict) for key in ("build", "options", "instrumentation", "phaseContract"))
    if not valid_metadata or mode not in ("legacy", "scene-publication"):
        errors.append("Trace accounting requires complete integrated fixture metadata.")
    if run.get("exitCode") != 0 or manifest.get("errors") or not manifest.get("completed"):
        errors.append("The enclosing benchmark manifest is incomplete or contains evidence errors.")
    if valid_metadata:
        for snapshot in (run.get("sourceBefore") or {}, run.get("sourceAfter") or {}):
            if snapshot.get("identity") != {key: metadata["build"].get(key) for key in ("commit", "trackedDirty", "trackedDiffSha256", "runtimeTestsSha256", "runtimeSourcesSha256", "harnessSha256")}:
                errors.append("Run build/source stability identity is inconsistent.")
        if metadata["options"].get("scopeMode") != "inclusive" or metadata["build"].get("profilerEnabled") is not True:
            errors.append("Inclusive CPU profiling must be enabled for a trace join.")
    frames_path = Path(run.get("log", "missing.log")).with_suffix(".frames.jsonl")
    timeline_path = frames_path.with_name(frames_path.name.replace(".frames.jsonl", ".timeline.jsonl"))
    frames = read_json(frames_path, errors, lines=True)
    timelines = read_json(timeline_path, errors, lines=True)
    verify_evidence((run.get("sidecars") or {}).get("frames"), frames_path, errors, "Run frame sidecar")
    verify_evidence((run.get("sidecars") or {}).get("timeline"), timeline_path, errors, "Run callback sidecar")
    audit = read_json(args.coverage_audit, errors) if args.coverage_audit else None
    phase_reviews = validate_audit(audit, metadata, errors, (run.get("sourceBefore") or {}).get("sourceRoot"))
    wait_keys = [key for review in phase_reviews.values() if review["audit"] for key in review["audit"]["waitSourceKeys"]]
    zones = ZoneIndex(read_zones(args.zones, errors, wait_keys))
    capture_verified = verify_capture(run, manifest, args.zones, args.messages, errors)
    if (metadata.get("phaseContract") or {}).get("traceSchema") != 2:
        errors.append("Exact frame/thread joins require marker schema 2 on both builds.")
    markers = read_markers(args.messages, (metadata.get("build") or {}).get("runtimeSourcesSha256"), (metadata.get("build") or {}).get("harnessSha256"), run.get("runId"), errors, zones)
    options = metadata.get("options") or {}
    accepted = validate_frames(frames, timelines, options, markers, errors)
    period = options.get("warmup", 0) + options.get("samples", 0) if accepted else 0
    rows = []
    for frame in accepted:
        index = frame["round"] * period + frame["sampleIndex"]
        review = phase_reviews["warmup" if frame["warmup"] else "steady"]
        coverage_errors = list(review["issues"])
        result = account_frame(frame, markers.get(index, {}), zones, mode, period, errors, review["audit"], coverage_errors)
        if result is not None:
            result["fixtureEvidence"] = {**{key: frame[key] for key in FIXTURE_VIEWS},
                                         **{key: frame[key] for key in ("actualMeshDraws", "depthCommands", "opaqueCommands", "transparentCommands")},
                                         "recordedPasses": verify_fixture_passes(result, zones, errors)}
            rows.append(result)
    steady = [row for row in rows if not row["warmup"]]
    phase_coverage = finalize_coverage(rows, options, errors, capture_verified)
    complete = all(value["totalComplete"] for value in phase_coverage.values())
    with (args.output_directory / "frames.jsonl").open("w", encoding="utf-8") as stream:
        for row in rows:
            stream.write(json.dumps(row, separators=(",", ":")) + "\n")
    metrics = summarize_metrics(steady)
    keys = Counter(zone.key for zone in zones.zones if not zone.name.startswith(MARKER_PREFIX))
    report = {
        "schemaVersion": 3, "label": args.label, "round": args.round, "runId": run.get("runId"), "snapshotMode": mode,
        "boundaryContract": BOUNDARY, "captureProvenanceVerified": capture_verified and not errors,
        "timeBasis": "inclusive Tracy intervals; per-thread union, not OS CPU samples",
        "phaseContract": metadata.get("phaseContract"), "workerActiveNs": None,
        "build": metadata.get("build"), "options": options, "instrumentation": metadata.get("instrumentation"),
        "shaderSha256": (run.get("sourceBefore") or {}).get("shaderSha256"),
        "scenario": manifest.get("scenario"),
        "totalComplete": complete, "steadyTotalComplete": phase_coverage["steady"]["totalComplete"], "phaseCoverage": phase_coverage,
        "phaseMetrics": {phase: summarize_metrics([row for row in rows if row["warmup"] is (phase == "warmup")]) for phase in PHASES},
        "knownCoverageParsed": not errors,
        "missingCoverage": [f"{phase}: reviewed whole-runtime preparation/worker/wait coverage" for phase in PHASES if not phase_coverage[phase]["totalComplete"]] + (["per-run capture provenance"] if not capture_verified else []) + ["OS CPU scheduling/sampling; wall spans are not scheduled CPU time"],
        "metrics": metrics, "joinedFrames": len(rows), "joinedSteadyFrames": len(steady), "errors": errors,
        "sourceKeys": [{"name": key[0], "src_file": key[1], "src_line": key[2], "events": count} for key, count in sorted(keys.items())],
        "inputs": {"manifest": evidence(args.manifest), "frames": evidence(frames_path) if frames_path.is_file() else None, "timeline": evidence(timeline_path) if timeline_path.is_file() else None, "zones": evidence(args.zones), "messages": evidence(args.messages), "capture": (run.get("trace") or {}).get("capture"), "coverageAudit": evidence(args.coverage_audit) if args.coverage_audit and args.coverage_audit.is_file() else None},
        "accountedFrames": evidence(args.output_directory / "frames.jsonl"),
    }
    (args.output_directory / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if valid_metadata:
        (args.output_directory / "coverage-audit.template.json").write_text(json.dumps(audit_template(metadata, zones.zones), indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"knownCoverageParsed": not errors, "joinedSteadyFrames": len(steady), "totalComplete": complete, "errors": len(errors)}))
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
