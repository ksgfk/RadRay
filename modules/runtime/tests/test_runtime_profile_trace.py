import sys
sys.dont_write_bytecode = True
import unittest
import csv
import hashlib
import tempfile
from copy import deepcopy
from pathlib import Path

from runtime_profile_trace import AUDIT_ASSERTIONS, BOUNDARY, FIXTURE_PASSES, FIXTURE_VIEWS, Zone, ZoneIndex, account_frame, audit_template, duration, finalize_coverage, key_record, read_json, read_markers, read_zones, subtract_intervals, summarize_metrics, union_intervals, validate_audit, verify_connection, verify_fixture_frame, verify_fixture_passes
from runtime_profile_compare import compare_rounds, make_report


class TraceAccountingTest(unittest.TestCase):
    def test_nested_and_overlapping_intervals_are_counted_once(self):
        self.assertEqual(union_intervals([(0, 10), (2, 5), (9, 20), (20, 25)]), [(0, 25)])
        self.assertEqual(duration([(0, 10), (2, 5), (9, 20)]), 20)
        self.assertEqual(subtract_intervals([(0, 100), (10, 20)], [(5, 30), (20, 40), (60, 70)]), [(0, 5), (40, 60), (70, 100)])

    def fixture(self):
        system = "F:/source/modules/runtime/src/render_system.cpp"
        forward = "F:/source/modules/runtime/src/forward_pipeline/forward_effects.cpp"
        scene = "F:/source/modules/runtime/src/render_framework/render_scene_snapshot.cpp"
        graph = "F:/source/modules/runtime/src/render_framework/render_graph.cpp"
        test = "F:/source/modules/runtime/tests/runtime_profile_integrated.h"
        app = "F:/source/modules/runtime/src/application.cpp"
        views = "F:/source/modules/runtime/src/render_framework/view_state.cpp"
        zones = [
            Zone("TickFrame", app, 100, 1, 0, 250),
            Zone("Application::GpuBeginUpdateForFlight", app, 106, 1, 0, 0),
            Zone("Update", app, 101, 1, 0, 215),
            Zone("RenderSystem::BeginUpdateForFlight", system, 102, 1, 1, 1),
            Zone("AssetManager::Pump", "F:/source/modules/runtime/src/asset_manager.cpp", 103, 1, 2, 2),
            Zone("ApplicationScheduler::Pump", app, 104, 1, 3, 3),
            Zone("PrepareFrameUploads", app, 105, 1, 220, 220),
            Zone("Profile.Authoring", test, 1, 1, 10, 20),
            Zone("RenderSystem::PrepareFrame", system, 2, 1, 100, 200),
            Zone("Scene.CommitChanges", scene, 3, 1, 110, 130),
            Zone("Scene.CompileStaticDraws", scene, 4, 1, 115, 125),
            Zone("Scene.PublishSnapshot", scene, 5, 1, 130, 150),
            Zone("Scene.RetainOwners", scene, 6, 1, 135, 145),
            Zone("ObjectValueUpdate", forward, 7, 1, 160, 175),
            Zone("RenderSystem::Render", system, 8, 2, 300, 600),
            Zone("ComposeGraph", system, 9, 2, 300, 430),
            Zone("Forward::DrawWorkBuild.Prepare", forward, 10, 2, 330, 390),
            Zone("RenderGraph::PrepareWork", graph, 21, 2, 330, 390),
            Zone("RenderGraph::Prepare", graph, 11, 2, 450, 540),
            Zone("RenderGraph::PreparePasses", graph, 22, 2, 480, 530),
            Zone("RenderGraph::PrepareUploads", graph, 23, 2, 460, 460),
            Zone("Forward::DrawWorkBuild.Ready", forward, 12, 2, 480, 530),
            Zone("PrepareRendererList", graph, 13, 2, 490, 520),
            Zone("RenderGraph::Record", graph, 14, 2, 550, 580),
            Zone("BeginGraphFlight", system, 106, 2, 300, 300),
            Zone("GraphFlightStorage", system, 107, 2, 300, 300),
            Zone("ViewStateRegistry::BeginFlight", views, 108, 2, 300, 300),
            Zone("ForwardPipeline::GraphRecorded", "F:/source/modules/runtime/src/forward_pipeline/forward_pipeline.cpp", 109, 2, 580, 589),
            Zone("PresentFinalize", system, 110, 2, 590, 595),
            Zone("RenderSystem::SubmittedFrame", system, 111, 2, 600, 640),
            Zone("RenderSystem::PresentCommit", system, 112, 2, 635, 639),
            Zone("ViewStateRegistry::CommitViewWithHistory", views, 113, 2, 615, 616),
            Zone("ViewStateRegistry::CommitViewWithHistory", views, 113, 2, 617, 618),
            Zone("ViewStateRegistry::CommitViewWithHistory", views, 113, 2, 619, 620),
        ]
        frame = {"round": 0, "sampleIndex": 0, "frameSerial": 51, "flightIndex": 1, "warmup": False, "viewCount": 3}
        stages = {"authoring_begin": 5, "authoring_done": 25, "world_done": 50, "gt_ready": 190, "rt_begin": 310, "rt_recorded": 590, "submit": 610, "complete": 700, "gt_observed": 730}
        markers = {stage: {"time": time, "flight": 1, "serial": 51 if stage in {"rt_begin", "rt_recorded", "submit", "complete", "gt_observed"} else 0, "thread": 2 if stage in {"rt_begin", "rt_recorded", "submit", "complete"} else 1} for stage, time in stages.items()}
        return zones, frame, markers

    def test_full_gt_guard_keeps_moved_and_unclassified_gt_work(self):
        zones, frame, markers = self.fixture()
        errors = []
        result = account_frame(frame, markers, ZoneIndex(zones), "scene-publication", 1, errors)
        self.assertEqual(errors, [])
        self.assertEqual(result["sceneCommitAndPublishNs"], 40)
        self.assertEqual(result["gtPreparationOtherNs"], 45)
        self.assertEqual(result["gtRenderPreparationNs"], 135)
        self.assertEqual(result["drawWorkBuildNs"], 110)
        self.assertEqual(result["measuredPreparationWorkNs"], 245)
        self.assertEqual(result["graphMaintenanceNs"], 110)
        self.assertEqual(result["recordNs"], 30)
        self.assertEqual(result["inputToSubmitNs"], 605)
        self.assertIsNone(result["preparationTotalNs"])
        self.assertIsNone(result["workerActiveNs"])

    def test_repeated_name_in_another_source_cannot_supply_a_guard(self):
        zones, frame, markers = self.fixture()
        zones = [zone for zone in zones if zone.name != "RenderSystem::PrepareFrame"]
        zones.append(Zone("RenderSystem::PrepareFrame", "F:/source/modules/runtime/src/fake.cpp", 2, 1, 100, 200))
        errors = []
        self.assertIsNone(account_frame(frame, markers, ZoneIndex(zones), "scene-publication", 1, errors))
        self.assertTrue(errors)

    def test_upload_work_is_preparation_and_is_removed_from_graph_maintenance(self):
        for mode in ("scene-publication", "legacy"):
            with self.subTest(mode=mode):
                zones, frame, markers = self.fixture()
                if mode == "legacy":
                    names = {"Scene.CommitChanges": "SceneSnapshotBuild", "ObjectValueUpdate": "FreezeObjectData"}
                    zones = [Zone(names.get(zone.name, zone.name), zone.file, zone.line, zone.thread, zone.begin, zone.end) for zone in zones]
                zones.extend([
                    Zone("RenderGraph::UploadWorkData", "F:/source/modules/runtime/src/render_framework/render_graph.cpp", 20, 2, 460, 480),
                    Zone("RenderGraph::UploadWorkData", "F:/source/modules/runtime/src/fake.cpp", 21, 2, 440, 450),
                ])
                errors = []
                result = account_frame(frame, markers, ZoneIndex(zones), mode, 1, errors)
                self.assertEqual(errors, [])
                self.assertEqual(result["uploadWorkDataNs"], 20)
                self.assertEqual(result["drawWorkBuildNs"], 130)
                self.assertEqual(result["measuredPreparationWorkNs"], 265)
                self.assertEqual(result["graphMaintenanceNs"], 90)
                self.assertIsNone(result["preparationTotalNs"])

    def test_legacy_shadow_declarations_are_subtracted_from_work(self):
        zones, frame, markers = self.fixture()
        zones = [zone for zone in zones if zone.name not in {"Scene.CommitChanges", "Scene.PublishSnapshot", "ObjectValueUpdate", "Forward::DrawWorkBuild.Prepare", "RenderGraph::PrepareWork"}]
        filename = "F:/source/modules/runtime/src/forward_pipeline/forward_effects.cpp"
        zones.extend([
            Zone("SceneSnapshotBuild", filename, 15, 1, 110, 150),
            Zone("FreezeObjectData", filename, 16, 1, 160, 175),
            Zone("BuildShadows", filename, 17, 2, 330, 410),
            Zone("ForwardGraph::BuildGraph", filename, 18, 2, 350, 380),
            Zone("ShadowCascadeCullAndList", filename, 19, 2, 335, 345),
        ])
        errors = []
        result = account_frame(frame, markers, ZoneIndex(zones), "legacy", 1, errors)
        self.assertEqual(errors, [])
        self.assertEqual(result["drawWorkBuildNs"], 100)
        self.assertEqual(result["measuredPreparationWorkNs"], 235)
        zones.append(Zone("Forward::DrawWorkBuild.PassFrameInputs", "F:/source/modules/runtime/src/forward_pipeline/forward_graph.cpp", 120, 2, 355, 365))
        errors = []
        result = account_frame(frame, markers, ZoneIndex(zones), "legacy", 1, errors)
        self.assertEqual(errors, [])
        self.assertEqual(result["drawWorkBuildNs"], 110)
        self.assertEqual(result["measuredPreparationWorkNs"], 245)

    def test_wrong_flight_or_missing_stage_is_never_a_valid_join(self):
        zones, frame, markers = self.fixture()
        markers["submit"]["flight"] = 0
        errors = []
        account_frame(frame, markers, ZoneIndex(zones), "scene-publication", 1, errors)
        self.assertTrue(errors)
        del markers["rt_begin"]
        errors = []
        self.assertIsNone(account_frame(frame, markers, ZoneIndex(zones), "scene-publication", 1, errors))
        self.assertTrue(errors)

    def test_same_frame_times_on_the_wrong_thread_are_rejected(self):
        zones, frame, markers = self.fixture()
        markers["rt_recorded"]["thread"] = 3
        errors = []
        account_frame(frame, markers, ZoneIndex(zones), "scene-publication", 1, errors)
        self.assertTrue(any("RT guard thread" in message for message in errors))

    def test_reviewed_waits_are_subtracted_without_parent_double_counting(self):
        zones, frame, markers = self.fixture()
        wait = Zone("FixtureWait", "F:/source/modules/runtime/src/render_system.cpp", 25, 1, 115, 145)
        zones.append(wait)
        audit = {"sourceKeys": [key_record(zone) for zone in zones if zone != wait], "waitSourceKeys": [key_record(wait)]}
        errors = []
        result = account_frame(frame, markers, ZoneIndex(zones), "scene-publication", 1, errors, audit)
        self.assertEqual(errors, [])
        self.assertEqual(result["preparationTotalNs"], 215)
        self.assertEqual(result["excludedBlockingWaitNs"], 30)
        self.assertEqual(result["measuredPreparationWorkNs"], 245)

    def test_new_worker_scope_keeps_total_unknown_even_with_reviewed_gt_rt(self):
        zones, frame, markers = self.fixture()
        audit = {"sourceKeys": [key_record(zone) for zone in zones], "waitSourceKeys": []}
        zones.append(Zone("NewWorkerPreparation", "F:/source/modules/runtime/src/new_worker.cpp", 10, 3, 240, 270))
        errors = []
        result = account_frame(frame, markers, ZoneIndex(zones), "scene-publication", 1, errors, audit)
        self.assertIsNone(result["preparationTotalNs"])
        self.assertTrue(any("unjoined runtime worker" in message for message in errors))

    def test_whole_live_work_and_passes_cover_effects_and_count_nested_ready_once(self):
        zones, frame, markers = self.fixture()
        zones = [Zone(zone.name, zone.file, zone.line, zone.thread,
                      320 if zone.name == "RenderGraph::PrepareWork" else 450 if zone.name == "RenderGraph::PreparePasses" else zone.begin,
                      400 if zone.name == "RenderGraph::PrepareWork" else 540 if zone.name == "RenderGraph::PreparePasses" else zone.end) for zone in zones]
        errors = []
        audit = {"sourceKeys": [key_record(zone) for zone in zones], "waitSourceKeys": []}
        result = account_frame(frame, markers, ZoneIndex(zones), "scene-publication", 1, errors, audit)
        self.assertEqual(errors, [])
        self.assertEqual(result["drawWorkBuildNs"], 170)
        self.assertEqual(result["liveWorkPreparationNs"], 80)
        self.assertEqual(result["livePassPreparationNs"], 90)
        self.assertEqual(result["graphMaintenanceNs"], 50)
        self.assertEqual(result["preparationTotalNs"], 305)

    def test_missing_live_work_boundary_and_unreviewed_source_keep_total_unknown(self):
        zones, frame, markers = self.fixture()
        audit = {"sourceKeys": [key_record(zone) for zone in zones], "waitSourceKeys": []}
        unreviewed = Zone("NewPassPreparation", "modules/runtime/src/new.cpp", 20, 2, 500, 510)
        enclosing_worker = Zone("LongWorkerPreparation", "modules/runtime/src/worker.cpp", 20, 3, 0, 900)
        for changed in ([zone for zone in zones if zone.name != "RenderGraph::PrepareWork"], zones + [unreviewed], zones + [enclosing_worker]):
            errors = []
            result = account_frame(frame, markers, ZoneIndex(changed), "scene-publication", 1, errors, audit)
            self.assertTrue(errors)
            self.assertIsNone(result["preparationTotalNs"])
            self.assertFalse(result["coverage"]["totalComplete"])

    def test_test_counter_observation_is_excluded_from_production_preparation(self):
        zones, frame, markers = self.fixture()
        zones.append(Zone("Profile.PrepareObservation", "modules/runtime/tests/runtime_profile_integrated.h", 10, 1, 180, 195))
        errors = []
        result = account_frame(frame, markers, ZoneIndex(zones), "scene-publication", 1, errors)
        self.assertEqual(errors, [])
        self.assertEqual(result["excludedHarnessObservationNs"], 15)
        self.assertEqual(result["gtRenderPreparationNs"], 120)
        self.assertEqual(result["measuredPreparationWorkNs"], 230)

    def test_source_digest_and_assertions_alone_do_not_prove_coverage(self):
        metadata = {"build": {"runtimeSourcesSha256": "r", "harnessSha256": "h"}, "phaseContract": {"snapshotMode": "scene-publication"}, "options": {"views": 3}}
        audit = audit_template(metadata, self.fixture()[0])
        review = audit["phaseReviews"]["steady"]
        review.update(status="reviewed", reviewer="Fixture source review", workerMode="none-after-source-review")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "modules/runtime/src/review.cpp"
            source.parent.mkdir(parents=True)
            source.write_text("void SynchronousPreparation() {}\n", encoding="utf-8")
            ref = {"file": "modules/runtime/src/review.cpp", "line": 1, "endLine": 1, "snippetSha256": hashlib.sha256(source.read_text(encoding="utf-8").encode("utf-8")).hexdigest(), "reason": "This reviewed test source demonstrates an explicitly synchronous preparation boundary."}
            review["assertions"] = {name: {"confirmed": True, "codeEvidence": [ref]} for name in AUDIT_ASSERTIONS}
            for key in review["sourceKeys"]:
                key["reason"] = "This exact source key was reviewed for fixture accounting ownership."
            errors = []
            phases = validate_audit(audit, metadata, errors, root)
            self.assertEqual(errors, [])
            self.assertIsNotNone(phases["steady"]["audit"])
            self.assertIsNone(phases["warmup"]["audit"])
            review["workerMode"] = "unknown"
            self.assertIsNone(validate_audit(audit, metadata, errors, root)["steady"]["audit"])
            review["workerMode"] = "none-after-source-review"
            source.write_text("void SynchronousPreparation() { StartWorker(); }\n", encoding="utf-8")
            changed = validate_audit(audit, metadata, errors, root)
            self.assertIsNone(changed["steady"]["audit"])
            self.assertTrue(any("snippet has changed" in message for message in changed["steady"]["issues"]))

    def test_marker_requires_matching_zone_identity_time_and_thread(self):
        marker = "RRP2|index=0|flight=1|serial=51|stage=rt_begin|run=run|build=build|harness=harness"
        anchor = Zone(marker, "modules/runtime/tests/runtime_profile_support.h", 110, 7, 10, 20)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "messages.csv"
            with path.open("w", newline="", encoding="utf-8") as stream:
                writer = csv.writer(stream)
                writer.writerow(["MessageName", "total_ns"])
                writer.writerow([marker, "15"])
            errors = []
            result = read_markers(path, "build", "harness", "run", errors, ZoneIndex([anchor]))
            self.assertEqual(errors, [])
            self.assertEqual(result[0]["rt_begin"]["thread"], 7)
            for anchors, run in (([], "run"), ([anchor, anchor], "run"), ([anchor], "different-run")):
                errors = []
                read_markers(path, "build", "harness", run, errors, ZoneIndex(anchors))
                self.assertTrue(errors)

    def test_csv_keeps_unknown_runtime_worker_scopes_for_coverage_review(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "zones.csv"
            path.write_text("name,src_file,src_line,ns_since_start,exec_time_ns,thread,value\nUnknownPreparation,modules/runtime/src/new_worker.cpp,5,100,20,4,\n", encoding="utf-8")
            errors = []
            zones = read_zones(path, errors)
            self.assertEqual(errors, [])
            self.assertEqual(len(zones), 1)
            self.assertEqual(zones[0].thread, 4)

    def test_capture_requires_opposite_endpoints_and_owned_start_times(self):
        trace = {"client": {"pid": 1, "processStartTicks": 10, "startedUtc": "2026-09-12T00:00:00.0000000Z", "completedUtc": "2026-09-12T00:00:10.0000000Z"},
                 "captureProcess": {"pid": 2, "processStartTicks": 20, "startedUtc": "2026-09-12T00:00:00.0000000Z", "completedUtc": "2026-09-12T00:00:11.0000000Z"},
                 "connection": {"observedUtc": "2026-09-12T00:00:01.0000000Z", "permitWrittenUtc": "2026-09-12T00:00:02.0000000Z", "port": 8086,
                    "client": {"pid": 1, "processStartTicks": 10, "localAddress": "127.0.0.1", "remoteAddress": "127.0.0.1", "localPort": 8086, "remotePort": 50000, "state": "Established"},
                    "capture": {"pid": 2, "processStartTicks": 20, "localAddress": "127.0.0.1", "remoteAddress": "127.0.0.1", "localPort": 50000, "remotePort": 8086, "state": "Established"}}}
        self.assertTrue(verify_connection(trace, []))
        for side, field, value in (("client", "pid", 99), ("capture", "processStartTicks", 21), ("capture", "remotePort", 8087), ("client", "state", "Listen")):
            invalid = deepcopy(trace)
            invalid["connection"][side][field] = value
            errors = []
            self.assertFalse(verify_connection(invalid, errors))
            self.assertTrue(errors)

    def test_actual_view_output_and_depth_evidence_cannot_be_replaced_with_configuration(self):
        frame = {**FIXTURE_VIEWS, "stageCommandsKnown": True, "actualMeshDraws": 7000, "depthCommands": 1750, "opaqueCommands": 2625,
                 "transparentCommands": 375, "reportExecutedPassesKnown": False, "reportExecutedPasses": None}
        self.assertTrue(verify_fixture_frame(frame, []))
        for key, value in (("viewCount", 1), ("fullViewCount", 0), ("outputCount", 1), ("writtenOutputCount", 1), ("depthCommands", 0),
                           ("stageCommandsKnown", False), ("reportExecutedPasses", 0)):
            with self.subTest(key=key):
                errors = []
                self.assertFalse(verify_fixture_frame({**frame, key: value}, errors))
                self.assertTrue(errors)

    def test_only_same_frame_rt_record_children_prove_effect_execution(self):
        graph = "F:/source/modules/runtime/src/render_framework/render_graph.cpp"
        zones = [Zone(name, graph, 200, 2, 400 + index, 401 + index) for index, name in enumerate(name for name, count in FIXTURE_PASSES.items() for _ in range(count))]
        result = {"sampleIndex": 0, "rtThread": 2, "intervals": {"record": [(400, 500)]}}
        errors = []
        self.assertEqual(verify_fixture_passes(result, ZoneIndex(zones), errors), FIXTURE_PASSES)
        self.assertEqual(errors, [])
        for mutation in (Zone(zones[0].name, graph, 200, 2, 300, 301), Zone(zones[0].name, graph, 200, 3, 400, 401),
                         Zone(zones[0].name, "F:/source/modules/runtime/src/fake.cpp", 200, 2, 400, 401)):
            errors = []
            verify_fixture_passes(result, ZoneIndex([mutation] + zones[1:]), errors)
            self.assertTrue(errors)

    def test_tile_light_culling_is_required_for_the_auxiliary_view_too(self):
        graph = "F:/source/modules/runtime/src/render_framework/render_graph.cpp"
        other_names = [name for name, count in FIXTURE_PASSES.items() if name != "Forward.TileLightCull" for _ in range(count)]
        result = {"sampleIndex": 0, "rtThread": 2, "intervals": {"record": [(400, 500)]}}
        # The observer keeps HDR local lighting when its full post effects are disabled.
        # Require all three dispatches, and reject either a missing or an extra one.
        for count in (2, 3, 4):
            with self.subTest(tile_passes=count):
                names = other_names + ["Forward.TileLightCull"] * count
                zones = [Zone(name, graph, 200, 2, 400 + index, 401 + index) for index, name in enumerate(names)]
                errors = []
                verify_fixture_passes(result, ZoneIndex(zones), errors)
                self.assertEqual(bool(errors), count != 3)

    def test_new_view_inputs_and_deferred_descriptor_publication_are_draw_preparation(self):
        zones, frame, markers = self.fixture()
        zones.extend([
            Zone("ResolveViewFamilies", "F:/source/modules/runtime/src/render_system.cpp", 30, 2, 300, 305),
            Zone("RenderGraph::PublishParameterSets", "F:/source/modules/runtime/src/render_framework/render_graph.cpp", 31, 2, 530, 535),
            Zone("Profile.ComposeObservation", "F:/source/modules/runtime/tests/runtime_profile_integrated.h", 32, 2, 305, 309),
        ])
        errors = []
        result = account_frame(frame, markers, ZoneIndex(zones), "scene-publication", 1, errors)
        self.assertEqual(errors, [])
        self.assertEqual(result["drawWorkBuildNs"], 120)
        self.assertEqual(result["graphMaintenanceNs"], 96)
        self.assertEqual(result["parameterSetPublicationNs"], 5)
        self.assertEqual(result["viewInputsNs"], 5)
        self.assertEqual(result["excludedHarnessComposeObservationNs"], 4)

    def test_gt_update_pumps_upload_and_rt_retirement_are_counted_with_waits_removed_once(self):
        zones, frame, markers = self.fixture()
        spans = {"RenderSystem::BeginUpdateForFlight": (1, 2), "AssetManager::Pump": (2, 3), "ApplicationScheduler::Pump": (3, 4),
                 "PrepareFrameUploads": (220, 240), "BeginGraphFlight": (300, 308), "GraphFlightStorage": (300, 302), "ViewStateRegistry::BeginFlight": (302, 308)}
        zones = [Zone(zone.name, zone.file, zone.line, zone.thread, *spans.get(zone.name, (zone.begin, zone.end))) for zone in zones]
        wait = Zone("UploadWait", "F:/source/modules/runtime/src/application.cpp", 121, 1, 225, 230)
        zones.append(wait)
        audit = {"sourceKeys": [key_record(zone) for zone in zones if zone != wait], "waitSourceKeys": [key_record(wait)]}
        errors = []
        result = account_frame(frame, markers, ZoneIndex(zones), "scene-publication", 1, errors, audit)
        self.assertEqual(errors, [])
        self.assertEqual(result["gtRenderPreparationNs"], 158)
        self.assertEqual(result["gtFrameUploadPreparationNs"], 20)
        self.assertEqual(result["rtViewFlightRetirementNs"], 6)
        self.assertEqual(result["graphMaintenanceNs"], 104)
        self.assertEqual(result["measuredPreparationWorkNs"], 274)
        self.assertEqual(result["preparationTotalNs"], 269)
        self.assertEqual(result["excludedBlockingWaitNs"], 5)

    def test_host_update_and_upload_join_rejects_missing_duplicate_wrong_thread_and_cross_tick(self):
        zones, frame, markers = self.fixture()
        upload = next(zone for zone in zones if zone.name == "PrepareFrameUploads")
        invalid_uploads = [[], [upload, upload], [Zone(upload.name, upload.file, upload.line, 2, 220, 220)],
                           [Zone(upload.name, upload.file, upload.line, 1, 251, 260)], [Zone(upload.name, upload.file, upload.line, 1, 180, 185)],
                           [Zone(upload.name, "F:/source/modules/runtime/src/fake.cpp", upload.line, 1, 220, 220)]]
        for replacements in invalid_uploads:
            with self.subTest(replacements=replacements):
                changed = [zone for zone in zones if zone.name != upload.name] + replacements
                errors = []
                self.assertIsNone(account_frame(frame, markers, ZoneIndex(changed), "scene-publication", 1, errors))
                self.assertTrue(errors)
        for missing in ("Update", "TickFrame", "AssetManager::Pump", "ApplicationScheduler::Pump", "RenderSystem::BeginUpdateForFlight"):
            errors = []
            self.assertIsNone(account_frame(frame, markers, ZoneIndex([zone for zone in zones if zone.name != missing]), "scene-publication", 1, errors))
            self.assertTrue(errors)

    def test_gpu_pre_update_span_is_counted_and_reviewed_wait_removed_once(self):
        zones, frame, markers = self.fixture()
        spans = {"Application::GpuBeginUpdateForFlight": (0, 1), "Update": (1, 215)}
        zones = [Zone(zone.name, zone.file, zone.line, zone.thread, *spans.get(zone.name, (zone.begin, zone.end))) for zone in zones]
        wait = Zone("GpuUpdateReviewedWait", "modules/runtime/src/application.cpp", 999, 1, 0, 1)
        review = {"sourceKeys": [key_record(zone) for zone in zones], "waitSourceKeys": [key_record(wait)]}
        errors = []
        result = account_frame(frame, markers, ZoneIndex(zones + [wait]), "scene-publication", 1, errors, review)
        self.assertEqual(errors, [])
        self.assertEqual(result["gtGpuFlightUpdateNs"], 1)
        self.assertEqual(result["gtRenderPreparationNs"], 136)
        self.assertEqual(result["measuredPreparationWorkNs"], 246)
        self.assertEqual(result["excludedBlockingWaitNs"], 1)
        self.assertEqual(result["preparationTotalNs"], 245)
        self.assertEqual(result["coverage"]["phase"], "steady")

    def test_gpu_pre_update_boundary_rejects_missing_duplicate_wrong_source_thread_and_cross_frame(self):
        zones, frame, markers = self.fixture()
        gpu = next(zone for zone in zones if zone.name == "Application::GpuBeginUpdateForFlight")
        for replacements in ([], [gpu, gpu], [Zone(gpu.name, gpu.file, gpu.line, 2, 0, 0)],
                             [Zone(gpu.name, "modules/runtime/src/fake.cpp", gpu.line, 1, 0, 0)],
                             [Zone(gpu.name, gpu.file, gpu.line, 1, 251, 260)], [Zone(gpu.name, gpu.file, gpu.line, 1, 5, 6)]):
            errors = []
            changed = [zone for zone in zones if zone != gpu] + replacements
            self.assertIsNone(account_frame(frame, markers, ZoneIndex(changed), "scene-publication", 1, errors))
            self.assertTrue(errors)

    def test_worker_before_update_in_gpu_flight_window_keeps_total_unknown(self):
        zones, frame, markers = self.fixture()
        spans = {"Application::GpuBeginUpdateForFlight": (0, 1), "Update": (1, 215)}
        zones = [Zone(zone.name, zone.file, zone.line, zone.thread, *spans.get(zone.name, (zone.begin, zone.end))) for zone in zones]
        review = {"sourceKeys": [key_record(zone) for zone in zones], "waitSourceKeys": []}
        zones.append(Zone("UploadCompletionWorker", "modules/runtime/src/worker.cpp", 999, 3, 0, 1))
        errors, issues = [], []
        result = account_frame(frame, markers, ZoneIndex(zones), "scene-publication", 1, errors, review, issues)
        self.assertEqual(errors, [])
        self.assertTrue(any("unjoined runtime worker" in issue for issue in issues))
        self.assertIsNone(result["preparationTotalNs"])
        self.assertFalse(result["coverage"]["totalComplete"])

    def test_history_continuation_is_exactly_joined_and_never_added_to_preparation(self):
        zones, frame, markers = self.fixture()
        zones = [Zone(zone.name, zone.file, zone.line, zone.thread, zone.begin, 630 if zone.name == "ViewStateRegistry::CommitViewWithHistory" and zone.begin == 619 else zone.end) for zone in zones]
        audit = {"sourceKeys": [key_record(zone) for zone in zones], "waitSourceKeys": []}
        errors = []
        result = account_frame(frame, markers, ZoneIndex(zones), "scene-publication", 1, errors, audit)
        self.assertEqual(errors, [])
        self.assertEqual(result["postSubmitHistoryNs"], 13)
        self.assertEqual(result["postSubmitOutputCommitNs"], 4)
        self.assertEqual(result["submittedContinuationNs"], 30)
        self.assertEqual(result["inputToSubmissionContinuationEndNs"], 635)
        self.assertEqual(result["preparationTotalNs"], 245)
        for missing in ("RenderSystem::SubmittedFrame", "ViewStateRegistry::CommitViewWithHistory", "RenderSystem::PresentCommit"):
            errors = []
            self.assertIsNone(account_frame(frame, markers, ZoneIndex([zone for zone in zones if zone.name != missing]), "scene-publication", 1, errors))
            self.assertTrue(errors)
        escaped = Zone("Forward::DrawWorkBuild.Migrated", "F:/source/modules/runtime/src/forward_pipeline/forward_effects.cpp", 122, 2, 622, 628)
        audit["sourceKeys"].append(key_record(escaped))
        errors = []
        result = account_frame(frame, markers, ZoneIndex(zones + [escaped]), "scene-publication", 1, errors, audit)
        self.assertIsNone(result["preparationTotalNs"])
        self.assertTrue(any("escaped" in message for message in errors))

    def phase_rows(self):
        zones, frame, markers = self.fixture()
        review = {"sourceKeys": [key_record(zone) for zone in zones], "waitSourceKeys": []}
        rows, errors = [], []
        for index in range(2):
            frame = {**frame, "sampleIndex": index, "warmup": index == 0}
            rows.append(account_frame(frame, markers, ZoneIndex(zones), "scene-publication", 2, errors,
                                      review if index else None, [] if index else ["Cold compiler workers and blocking waits remain unreviewed."]))
        self.assertEqual(errors, [])
        return rows

    def test_unknown_cold_rows_are_preserved_without_poisoning_independently_reviewed_steady(self):
        rows = self.phase_rows()
        errors = []
        phases = finalize_coverage(rows, {"warmup": 1, "samples": 1}, errors, True)
        self.assertEqual(errors, [])
        self.assertEqual(len(rows), 2)
        self.assertFalse(phases["warmup"]["totalComplete"])
        self.assertTrue(phases["warmup"]["issues"])
        self.assertTrue(phases["steady"]["totalComplete"])
        self.assertIsNone(rows[0]["preparationTotalNs"])
        self.assertEqual(rows[1]["preparationTotalNs"], 245)
        self.assertIsNone(summarize_metrics(rows)["preparationTotalNs"]["p50Ns"])
        self.assertEqual(summarize_metrics(rows[1:])["preparationTotalNs"]["p50Ns"], 245)

    def test_phase_membership_missing_cold_and_run_errors_reject_all_totals(self):
        rows = self.phase_rows()
        wrong_warmup, wrong_phase = deepcopy(rows), deepcopy(rows)
        wrong_warmup[0]["warmup"] = False
        wrong_phase[0]["coverage"]["phase"] = "steady"
        for changed, errors, capture in ((wrong_warmup, [], True), (wrong_phase, [], True), (rows[1:], [], True),
                                          (deepcopy(rows), ["Wrong run/serial/thread/source join."], True), (deepcopy(rows), [], False)):
            phases = finalize_coverage(changed, {"warmup": 1, "samples": 1}, errors, capture)
            self.assertFalse(phases["steady"]["totalComplete"])
            self.assertTrue(all(row["preparationTotalNs"] is None for row in changed))

    def test_phase_audit_rejects_custom_ranges_missing_phases_and_duplicate_json_keys(self):
        metadata = {"build": {"runtimeSourcesSha256": "r", "harnessSha256": "h"}, "phaseContract": {"snapshotMode": "scene-publication"}, "options": {"warmup": 1, "samples": 1}}
        base = audit_template(metadata, self.fixture()[0])
        missing, unknown, custom, top = (deepcopy(base) for _ in range(4))
        del missing["phaseReviews"]["warmup"]
        unknown["phaseReviews"]["favorable"] = unknown["phaseReviews"]["steady"]
        custom["phaseReviews"]["steady"]["sampleIndices"] = [1]
        top["frameRanges"] = [[1, 2], [1, 2]]
        for changed in (missing, unknown, custom, top):
            errors = []
            validate_audit(changed, metadata, errors)
            self.assertTrue(errors)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "duplicate.json"
            path.write_text('{"phaseReviews":{"steady":{},"steady":{}}}', encoding="utf-8")
            errors = []
            read_json(path, errors)
            self.assertTrue(any("Duplicate JSON" in error for error in errors))

    def test_steady_unknown_worker_or_wait_is_not_filtered_to_known_subset(self):
        rows = self.phase_rows()
        zones, frame, markers = self.fixture()
        review = {"sourceKeys": [key_record(zone) for zone in zones], "waitSourceKeys": []}
        for name, filename, thread in (("UnknownWorker", "modules/runtime/src/worker.cpp", 3),
                                       ("UnknownWait", "modules/runtime/src/render_system.cpp", 1)):
            errors, issues = [], []
            changed = zones + [Zone(name, filename, 999, thread, 115, 145)]
            row = account_frame({**frame, "sampleIndex": 2}, markers, ZoneIndex(changed), "scene-publication", 3, errors, review, issues)
            self.assertEqual(errors, [])
            self.assertTrue(issues)
            self.assertIsNone(row["preparationTotalNs"])
            preserved = deepcopy(rows) + [row]
            phases = finalize_coverage(preserved, {"warmup": 1, "samples": 2}, errors, True)
            self.assertEqual(errors, [])
            self.assertEqual(phases["steady"]["completeFrames"], 1)
            self.assertFalse(phases["steady"]["totalComplete"])
            metric = summarize_metrics(preserved[1:])["preparationTotalNs"]
            self.assertEqual(metric["samples"], 2)
            self.assertEqual(metric["knownSamples"], 1)
            self.assertIsNone(metric["p50Ns"])

    def test_steady_gate_can_be_known_while_overall_and_cold_totals_remain_unknown(self):
        def rounds(label, value):
            return [{"summary": {"metrics": {"preparationTotalNs": {}}, "options": {"warmup": 120, "samples": 1000},
                        "build": {"configuration": "Release"}, "instrumentation": {}, "round": index, "runId": f"{label}-{index}",
                        "captureProvenanceVerified": True, "totalComplete": False, "steadyTotalComplete": True},
                     "rows": [{"preparationTotalNs": value, "coverage": {"totalComplete": True}} for _ in range(1000)],
                     "warmupRows": [{"preparationTotalNs": None, "coverage": {"totalComplete": False}} for _ in range(120)],
                     "evidence": {}, "frames": {}} for index in range(5)]
        baseline, candidate = rounds("baseline", 100), rounds("candidate", 60)
        report = make_report(baseline, candidate, [])
        self.assertTrue(report["protocolConforming"])
        self.assertTrue(report["steadyTotalComplete"])
        self.assertTrue(report["preparationTotalTargetPassed"])
        self.assertFalse(report["totalComplete"])
        self.assertIsNone(report["warmupMetrics"]["preparationTotalNs"]["candidate"]["p50Ns"])
        candidate[0]["rows"][0]["preparationTotalNs"] = None
        candidate[0]["rows"][0]["coverage"]["totalComplete"] = False
        self.assertFalse(make_report(baseline, candidate, [])["steadyTotalComplete"])
        self.assertIsNone(make_report(baseline, candidate, [])["preparationTotalTargetPassed"])

    def test_unknown_or_partially_known_metric_never_becomes_zero_or_a_pass(self):
        def rounds(label, value):
            return [{"summary": {"metrics": {"preparationTotalNs": {}, "gtRenderPreparationNs": {}}, "options": {"warmup": 120, "samples": 1000},
                        "build": {"configuration": "Release"}, "instrumentation": {}, "round": index, "runId": f"{label}-{index}", "captureProvenanceVerified": True, "totalComplete": False},
                     "rows": [{"preparationTotalNs": value, "gtRenderPreparationNs": 100} for _ in range(1000)], "evidence": {}, "frames": {}} for index in range(5)]
        baseline, candidate = rounds("baseline", None), rounds("candidate", None)
        result = make_report(baseline, candidate, [])
        self.assertTrue(result["protocolConforming"])
        self.assertFalse(result["totalComplete"])
        self.assertIsNone(result["preparationTotalTargetPassed"])
        self.assertIsNone(result["metrics"]["preparationTotalNs"]["candidate"]["p50Ns"])
        candidate[0]["rows"][0]["preparationTotalNs"] = 1
        self.assertIsNone(compare_rounds(baseline, candidate)["preparationTotalNs"]["candidate"]["p50Ns"])
        candidate[0]["rows"][0]["fixtureEvidence"] = {"actualMeshDraws": 1}
        errors = []
        self.assertFalse(make_report(baseline, candidate, errors)["protocolConforming"])
        self.assertTrue(any("workload differs" in message for message in errors))


if __name__ == "__main__":
    unittest.main()
