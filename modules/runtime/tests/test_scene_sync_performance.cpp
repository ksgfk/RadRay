// Measurement boundaries and invocation: docs/guide/build-test.md
#include "scene_test_support.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <numeric>
#include <random>
#include <semaphore>
#include <thread>
#include <fmt/format.h>
#include <radray/profiler.h>
#include <radray/runtime/components/directional_light_component.h>
#include <radray/runtime/components/point_light_component.h>
#include <radray/runtime/components/spot_light_component.h>
#include <radray/runtime/components/static_mesh_component.h>
#include <radray/runtime/game_framework/actor.h>
#ifdef RADRAY_ENABLE_MIMALLOC
#include <mimalloc.h>
#include <mimalloc-stats.h>
#endif

namespace radray {
namespace {

using Clock = std::chrono::steady_clock;

int64_t Now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
double Microseconds(int64_t begin, int64_t end) { return double(end - begin) / 1000.0; }

enum class Workload { Transform,
                      SameValue,
                      HierarchyLeaf,
                      HierarchyRoot,
                      Rebind,
                      Lights,
                      Churn,
                      Burst,
                      Reparent,
                      Mixed };

struct Scenario {
    string Name;
    Workload Kind{Workload::Transform};
    uint32_t Shapes{10000}, Changes{100}, Lights{0}, LightChanges{0}, Repeats{1}, Depth{1};
    bool Wide{false}, Sequential{false};
};

vector<Scenario> Scenarios(bool small) {
    vector<Scenario> result;
    for (uint32_t n : (small ? vector<uint32_t>{256} : vector<uint32_t>{1000, 10000, 100000})) {
        for (uint32_t changes : {0u, 1u, std::max(1u, n / 100), n / 10, n})
            result.push_back({fmt::format("shape_{}_dirty_{}", n, changes), Workload::Transform, n, changes});
    }
    for (uint32_t n : (small ? vector<uint32_t>{256} : vector<uint32_t>{10000, 100000}))
        result.push_back({fmt::format("shape_{}_sequential_all", n), Workload::Transform, n, n, 0, 0, 1, 1, false, true});
    const uint32_t n = small ? 256 : 10000;
    const uint32_t changes = std::max(1u, n / 100);
    for (uint32_t repeats : {1u, 10u, 100u})
        result.push_back({fmt::format("repeat_{}", repeats), Workload::Transform, n, changes, 0, 0, repeats});
    result.push_back({"same_value_100", Workload::SameValue, n, changes, 0, 0, 100});
    for (uint32_t depth : {1u, 4u, 16u}) {
        result.push_back({fmt::format("chain_{}_leaf", depth), Workload::HierarchyLeaf, n, changes, 0, 0, 1, depth});
        result.push_back({fmt::format("chain_{}_root", depth), Workload::HierarchyRoot, n, changes, 0, 0, 1, depth});
    }
    for (uint32_t width : {4u, 16u})
        result.push_back({fmt::format("wide_{}_root", width), Workload::HierarchyRoot, n, changes, 0, 0, 1, width, true});
    result.push_back({"rebind_ready", Workload::Rebind, n, changes});
    for (uint32_t lights : {1u, 16u, 64u, 256u}) {
        result.push_back({fmt::format("light_{}_clean", lights), Workload::Lights, 0, 0, lights, 0});
        result.push_back({fmt::format("light_{}_one", lights), Workload::Lights, 0, 0, lights, 1});
        result.push_back({fmt::format("light_{}_all", lights), Workload::Lights, 0, 0, lights, lights});
    }
    result.push_back({"churn_10", Workload::Churn, n, std::min(10u, n)});
    result.push_back({"burst_replace_10pct", Workload::Burst, n, n / 10});
    result.push_back({"reparent", Workload::Reparent, n, changes});
    result.push_back({"mixed", Workload::Mixed, n, changes, 64, 4});
    return result;
}

unique_ptr<StaticMesh> MakeMesh(float extent) {
    const array<float, 9> positions{0, 0, 0, extent, 0, 0, 0, extent, 0};
    const array<uint32_t, 3> indices{0, 1, 2};
    MeshResource mesh;
    mesh.Bins.emplace_back(std::as_bytes(std::span{positions}));
    mesh.Bins.emplace_back(std::as_bytes(std::span{indices}));
    MeshPrimitive primitive;
    primitive.VertexCount = 3;
    primitive.VertexBuffers.push_back({"POSITION", 0, 0, VertexDataType::FLOAT, 3, 0, 12});
    primitive.IndexBuffer = {1, 3, 0, 4};
    mesh.Primitives.push_back(std::move(primitive));
    return make_unique<StaticMesh>(std::move(mesh), vector<StaticMeshSection>{}, Eigen::Vector3f::Zero(), Eigen::Vector3f::Constant(extent), GpuMesh{});
}

array<int64_t, 2> AllocationTotals() {
#ifdef RADRAY_ENABLE_MIMALLOC
    if (std::getenv("RADRAY_SCENE_SYNC_ALLOCATIONS") == nullptr) return {-1, -1};
    mi_collect(true);
    mi_stats_t stats;
    mi_stats_init(&stats);
    if (mi_stats_get(&stats)) return {stats.malloc_normal_count.total + stats.malloc_huge_count.total,
                                      stats.malloc_normal.total + stats.malloc_huge.total};
#endif
    return {-1, -1};
}

void MergeThreadAllocations() {
#ifdef RADRAY_ENABLE_MIMALLOC
    mi_collect(true);
    mi_theap_stats_merge_to_heap(mi_theap_get_default());
#endif
}

bool TracksContainerAllocations() {
#ifdef RADRAY_ENABLE_MIMALLOC
    const auto before = AllocationTotals();
    vector<byte> probe(16384);
    const auto after = AllocationTotals();
    return mi_is_in_heap_region(probe.data()) && after[0] > before[0] && after[1] > before[1];
#else
    return false;
#endif
}

struct ExpectedShape {
    ShapeId Id;
    AssetId Asset;
    Eigen::Matrix4f Matrix;
    Eigen::Vector3f BoundsMin, BoundsMax;
    const GpuMesh* Mesh;
    bool ReverseCulling;
};

struct ExpectedScene {
    vector<ExpectedShape> Shapes;
    vector<ShapeId> Removed;
    LightSceneData Lights;
};

bool SameCommon(const LightCommonData& a, const LightCommonData& b) {
    return a.Id == b.Id && a.Color.isApprox(b.Color) && a.Intensity == b.Intensity &&
           a.ShadowDepthBias == b.ShadowDepthBias && a.ShadowNormalBias == b.ShadowNormalBias &&
           a.AffectsWorld == b.AffectsWorld && a.CastShadow == b.CastShadow;
}

bool SamePoint(const PointLightParameters& a, const PointLightParameters& b) {
    return a.Position.isApprox(b.Position) && a.Direction.isApprox(b.Direction) &&
           a.AttenuationRadius == b.AttenuationRadius && a.FalloffExponent == b.FalloffExponent &&
           a.SourceRadius == b.SourceRadius && a.SoftSourceRadius == b.SoftSourceRadius &&
           a.SourceLength == b.SourceLength && a.InverseSquaredFalloff == b.InverseSquaredFalloff;
}

bool Validate(const RenderScene& scene, const ExpectedScene& expected) {
    auto lease = scene.AcquireRead();
    if (scene.GetStaticMeshes().size() != expected.Shapes.size() || scene.GetLights().Count() != expected.Lights.Count()) return false;
    for (const auto& e : expected.Shapes) {
        const auto mesh = scene.GetStaticMesh(e.Id);
        if (!mesh || mesh->Mesh.MeshAssetId != e.Asset || mesh->Mesh.RenderMesh.Get() != e.Mesh ||
            mesh->Mesh.Sections.size() != 1 || mesh->Mesh.Sections[0].IndexCount != 3 ||
            !mesh->LocalToWorld.isApprox(e.Matrix) || !mesh->WorldBoundsMin.isApprox(e.BoundsMin) ||
            !mesh->WorldBoundsMax.isApprox(e.BoundsMax) || mesh->ReverseCulling != e.ReverseCulling) return false;
    }
    for (const auto id : expected.Removed)
        if (scene.ContainsShape(id)) return false;
    const auto& lights = scene.GetLights();
    if (lights.DirectionalLights.size() != expected.Lights.DirectionalLights.size() ||
        lights.PointLights.size() != expected.Lights.PointLights.size() ||
        lights.SpotLights.size() != expected.Lights.SpotLights.size() || !lights.RectLights.empty()) return false;
    for (const auto& e : expected.Lights.DirectionalLights) {
        const auto light = lights.GetDirectionalLight(e.Common.Id);
        if (!light || !SameCommon(light->Common, e.Common) || !light->Direction.isApprox(e.Direction)) return false;
    }
    for (const auto& e : expected.Lights.PointLights) {
        const auto light = lights.GetPointLight(e.Common.Id);
        if (!light || !SameCommon(light->Common, e.Common) || !SamePoint(light->Point, e.Point)) return false;
    }
    for (const auto& e : expected.Lights.SpotLights) {
        const auto light = lights.GetSpotLight(e.Common.Id);
        if (!light || !SameCommon(light->Common, e.Common) || !SamePoint(light->Point, e.Point) ||
            light->InnerConeAngle != e.InnerConeAngle || light->OuterConeAngle != e.OuterConeAngle) return false;
    }
    return true;
}

struct GTFrame {
    int64_t Start{0}, Published{0};
    double Mutation{0}, Tick{0}, Lifecycle{0}, Collect{0}, Seal{0}, Publish{0}, Wait{0}, Completion{0};
    uint64_t Sequence{0};
};

struct RTFrame {
    int64_t Begin{0}, End{0};
    uint64_t Transforms{0}, MeshStates{0}, Creates{0}, Removes{0}, LightRecords{0}, PayloadBytes{0};
    bool Valid{true};
};

enum class Command { Frame,
                     MergeAllocations,
                     Stop };

struct alignas(64) FlightSlot {
    std::binary_semaphore Done{0};
    Command Job{Command::Frame};
    size_t Sample{0};
    uint64_t Sequence{0};
    bool Pending{false}, Verify{false};
    ExpectedScene Expected;
};

class SyncBenchmark {
public:
    SyncBenchmark(Scenario scenario, uint32_t flights, bool threaded, size_t sampleCount, bool gate = false)
        : _scenario(std::move(scenario)), _flights(flights), _threaded(threaded), _gate(gate),
          _renderer(&_app, flights), _gt(sampleCount), _rt(sampleCount) {
        for (uint32_t i = 0; i < flights; ++i) _slots.push_back(make_unique<FlightSlot>());
        const auto allocations = AllocationTotals();
        const auto start = Now();
        _sceneId = test::ConnectWorld(_world, _renderer);
        _meshes[0] = _assets.AddReady<StaticMesh>(AssetId{1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, MakeMesh(1));
        _meshes[1] = _assets.AddReady<StaticMesh>(AssetId{2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, MakeMesh(2));
        _components.reserve(_scenario.Shapes);
        for (uint32_t i = 0; i < _scenario.Shapes; ++i) {
            Nullable<SceneComponent*> parent{nullptr};
            if (i % _scenario.Depth != 0) parent = _components[_scenario.Wide ? i - i % _scenario.Depth : i - 1];
            _components.push_back(SpawnMesh(i, parent));
        }
        if (_scenario.Kind == Workload::Reparent) {
            _parents.push_back(_world.SpawnActor()->AddComponent<SceneComponent>());
            _parents.push_back(_world.SpawnActor()->AddComponent<SceneComponent>());
            _parents[0]->SetRelativeLocation({100, 0, 0});
            _parents[1]->SetRelativeLocation({-100, 0, 0});
        }
        for (uint32_t i = 0; i < _scenario.Lights; ++i) {
            auto* actor = _world.SpawnActor();
            LightComponent* light = i % 3 == 0 ? static_cast<LightComponent*>(actor->AddComponent<DirectionalLightComponent>()) : i % 3 == 1 ? static_cast<LightComponent*>(actor->AddComponent<PointLightComponent>())
                                                                                                                                             : static_cast<LightComponent*>(actor->AddComponent<SpotLightComponent>());
            light->SetRelativeLocation({float(i), 2, 3});
            light->SetLightColor({0.25f, 0.5f, 0.75f});
            _lights.push_back(light);
        }
        _setupUs = Microseconds(start, Now());
        const auto label = fmt::format("SceneSync {} F={} {}", _scenario.Name, flights, threaded ? "threaded" : "single");
        RADRAY_PROFILE_MESSAGE(label);
        const auto after = AllocationTotals();
        _allocationTracking = after[0] > allocations[0] && TracksContainerAllocations();
        _setupAllocations = _allocationTracking ? after[0] - allocations[0] : -1;
        _setupBytes = _allocationTracking ? after[1] - allocations[1] : -1;
        for (uint32_t i = 0; i < _scenario.Shapes; ++i) {
            if (_scenario.Kind == Workload::HierarchyLeaf && (i + 1) % _scenario.Depth != 0 && i + 1 != _scenario.Shapes) continue;
            if (_scenario.Kind == Workload::HierarchyRoot && i % _scenario.Depth != 0) continue;
            _selection.push_back(i);
        }
        std::mt19937 generator{0x5CE1u};
        if (!_scenario.Sequential) std::shuffle(_selection.begin(), _selection.end(), generator);
        _removedShapes.reserve(_scenario.Shapes);
        const auto initial = Now();
        test::PrepareScene(_world, _renderer, 0);
        test::ConsumeFrame(_renderer, 0);
        test::CompleteFrame(_renderer, 0, false);
        _initialSyncUs = Microseconds(initial, Now());
        if (_threaded) _worker = std::thread{[this] { Worker(); }};
        RADRAY_PROFILE_THREAD("RadRay GT");
    }

    ~SyncBenchmark() { Stop(); }

    void Run(size_t begin, size_t count, bool verify) {
        for (size_t sample = begin; sample < begin + count; ++sample) {
            const uint32_t flight = uint32_t(_ticket % _flights);
            auto& slot = *_slots[flight];
            auto& frame = _gt[sample];
            frame.Wait = Reclaim(flight);
            frame.Start = Now();
            {
                RADRAY_PROFILE_SCOPE_N("SceneSync.Mutate");
                Mutate(sample);
            }
            auto phase = Now();
            frame.Mutation = Microseconds(frame.Start, phase);
            _world.Tick(1.0f / 60.0f);
            auto end = Now();
            frame.Tick = Microseconds(phase, end);
            phase = end;
            _world.FinalizeWorldGT();
            end = Now();
            frame.Lifecycle = Microseconds(phase, end);
            phase = end;
            _world.CollectRenderUpdates();
            end = Now();
            frame.Collect = Microseconds(phase, end);
            phase = end;
            _renderer.SealFrameGT(flight);
            end = Now();
            frame.Seal = Microseconds(phase, end);
            if (verify) CaptureExpected(slot.Expected);
            slot.Job = Command::Frame;
            slot.Sample = sample;
            slot.Verify = verify;
            slot.Sequence = frame.Sequence = _renderer.GetUpdateSequence(flight);
            phase = Now();
            _renderer.PublishFrameGT(flight);
            frame.Published = Now();
            frame.Publish = Microseconds(phase, frame.Published);
            slot.Pending = true;
            ++_ticket;
            if (_threaded) {
                _ready.release();
                if (_gate && _ticket == _flights) {
                    _publishedWhileGated = uint32_t(_ticket);
                    _initialGate.release();
                }
            } else
                Apply(flight, slot);
            RADRAY_PROFILE_FRAME();
        }
        Drain();
    }

    array<int64_t, 2> Allocations() {
        if (!_allocationTracking) return {-1, -1};
        if (_threaded) Control(Command::MergeAllocations);
        return AllocationTotals();
    }

    bool FinishAndValidate() {
        Stop();
        ExpectedScene expected;
        CaptureExpected(expected);
        const auto scene = _renderer.GetSceneRT(_sceneId);
        return scene && Validate(*scene, expected) && !_gateTimedOut &&
               std::all_of(_rt.begin(), _rt.end(), [](const auto& frame) { return frame.Valid; });
    }

    void Write(std::ostream& raw, std::ostream& cases, size_t begin, size_t count, array<int64_t, 2> allocations) const {
        const string mode = _threaded ? "threaded" : "single";
        for (size_t sample = begin; sample < begin + count; ++sample) {
            const auto& g = _gt[sample];
            const auto& r = _rt[sample];
            const double apply = Microseconds(r.Begin, r.End);
            const double gt = g.Mutation + g.Tick + g.Lifecycle + g.Collect + g.Seal + g.Publish;
            raw << fmt::format("{},{},{},{},{},{},{},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{},{},{},{},{},{}\n",
                               _scenario.Name, mode, _flights, sample - begin, g.Sequence, g.Start, r.End,
                               g.Mutation, g.Tick, g.Lifecycle, g.Collect, g.Seal, g.Publish, apply,
                               Microseconds(g.Published, r.Begin), Microseconds(g.Start, r.End), g.Wait, g.Completion, gt, gt + apply,
                               r.Transforms, r.MeshStates, r.Creates, r.Removes, r.LightRecords, r.PayloadBytes);
        }
        const double span = Microseconds(_gt[begin].Start, _rt[begin + count - 1].End);
        cases << fmt::format("{},{},{},{},{},{},{},{},{},{},{:.3f},{:.3f},{},{},{},{},{:.3f},{:.6f}\n",
                             _scenario.Name, mode, _flights, _scenario.Shapes, _scenario.Changes, _scenario.Lights, _scenario.LightChanges,
                             _scenario.Repeats, _scenario.Depth, _scenario.Wide, _setupUs, _initialSyncUs, _setupAllocations, _setupBytes,
                             allocations[0], allocations[1], span, double(count) * 1e6 / span);
        fmt::print("SCENE_SYNC {} {} F={} {:.1f} us/frame, {} frames\n", _scenario.Name, mode, _flights, span / double(count), count);
        std::fflush(stdout);
    }

    uint32_t PublishedWhileGated() const { return _publishedWhileGated; }

private:
    StaticMeshComponent* SpawnMesh(uint32_t index, Nullable<SceneComponent*> parent = nullptr) {
        auto* component = _world.SpawnActor()->AddSceneComponent<StaticMeshComponent>(parent, AttachmentRule::KeepLocal);
        component->SetStaticMesh(_meshes[0]);
        component->SetRelativeLocation({float(index % 97), 1, 0});
        component->SetRelativeScale({index % 7 == 0 ? -1.0f : 1.0f, 1, 1});
        return component;
    }

    uint32_t Selected(size_t frame, uint32_t offset) const {
        if (_scenario.Sequential) return _selection[offset];
        return _selection[(frame * 131 + offset) % _selection.size()];
    }

    void Move(size_t frame, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) {
            auto* component = _components[Selected(frame, i)];
            for (uint32_t repeat = 0; repeat < _scenario.Repeats; ++repeat) {
                if (_scenario.Kind == Workload::SameValue)
                    component->SetRelativeLocation(component->GetRelativeLocation());
                else
                    component->SetRelativeLocation({float((frame + 1) * _scenario.Repeats + repeat), float(i % 17), 2});
            }
        }
    }

    void Replace(size_t frame, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) {
            const auto index = Selected(frame, i);
            _removedShapes.push_back(_components[index]->GetShapeId());
            _world.DestroyActor(_components[index]->GetOwner().Get());
            _components[index] = SpawnMesh(index);
        }
    }

    void Mutate(size_t frame) {
        _removedShapes.clear();
        const uint32_t count = std::min<uint32_t>(_scenario.Changes, uint32_t(_selection.size()));
        switch (_scenario.Kind) {
            case Workload::Transform:
            case Workload::SameValue:
            case Workload::HierarchyLeaf:
            case Workload::HierarchyRoot: Move(frame, count); break;
            case Workload::Rebind:
                for (uint32_t i = 0; i < count; ++i) {
                    auto* component = _components[Selected(frame, i)];
                    component->SetStaticMesh(_meshes[component->GetStaticMesh().GetAssetId() == _meshes[0].GetAssetId() ? 1 : 0]);
                }
                break;
            case Workload::Churn: Replace(frame, count); break;
            case Workload::Burst:
                if (frame % 32 == 0) Replace(frame, count);
                break;
            case Workload::Reparent:
                for (uint32_t i = 0; i < count; ++i) {
                    auto* component = _components[Selected(frame, i)];
                    component->RequestReparent(_parents[component->GetAttachParent().Get() == _parents[0] ? 1 : 0], AttachmentRule::KeepLocal);
                }
                break;
            case Workload::Mixed:
                Move(frame, frame % 32 == 0 ? _scenario.Shapes : count);
                Replace(frame, std::min(10u, _scenario.Shapes));
                break;
            case Workload::Lights: break;
        }
        for (uint32_t i = 0; i < _scenario.LightChanges; ++i) {
            auto* light = _lights[(frame + i) % _lights.size()];
            light->SetIntensity(float(frame + 2));
            light->SetRelativeLocation({float(frame + 1), float(i), 3});
        }
    }

    void CaptureExpected(ExpectedScene& expected) const {
        expected.Shapes.clear();
        expected.Removed = _removedShapes;
        expected.Lights.Clear();
        expected.Shapes.reserve(_components.size());
        for (auto* component : _components) {
            const auto& mesh = component->GetStaticMesh();
            const auto matrix = component->GetWorldMatrix();
            Eigen::Vector3f minimum = Eigen::Vector3f::Constant(std::numeric_limits<float>::max());
            Eigen::Vector3f maximum = -minimum;
            for (uint32_t corner = 0; corner < 8; ++corner) {
                Eigen::Vector4f point;
                for (uint32_t axis = 0; axis < 3; ++axis) point[axis] = (corner & (1u << axis)) ? mesh->GetBoundsMax()[axis] : mesh->GetBoundsMin()[axis];
                point[3] = 1;
                const Eigen::Vector3f transformed = (matrix * point).head<3>();
                minimum = minimum.cwiseMin(transformed);
                maximum = maximum.cwiseMax(transformed);
            }
            expected.Shapes.push_back({component->GetShapeId(), mesh.GetAssetId(), matrix, minimum, maximum, &mesh->GetRenderMesh(),
                                       matrix(0, 0) * matrix(1, 1) * matrix(2, 2) < 0});
        }
        for (auto* component : _lights) {
            LightCommonData common;
            common.Id = component->GetLightId();
            common.Color = component->GetLightColor();
            common.Intensity = component->GetIntensity();
            common.AffectsWorld = component->AffectsWorld();
            common.CastShadow = component->CastShadow();
            if (component->GetLightType() == LightType::Directional) {
                expected.Lights.DirectionalLights.push_back({common, component->GetLightDirection()});
                continue;
            }
            auto* point = static_cast<PointLightComponent*>(component);
            common.ShadowDepthBias = point->GetShadowDepthBias();
            common.ShadowNormalBias = point->GetShadowNormalBias();
            PointLightParameters params{point->GetWorldLocation(), point->GetLightDirection(), point->GetAttenuationRadius(),
                                        point->GetLightFalloffExponent(), point->GetSourceRadius(), point->GetSoftSourceRadius(),
                                        point->GetSourceLength(), point->UseInverseSquaredFalloff()};
            if (component->GetLightType() == LightType::Point)
                expected.Lights.PointLights.push_back({common, params});
            else {
                auto* spot = static_cast<SpotLightComponent*>(point);
                expected.Lights.SpotLights.push_back({common, params, spot->GetInnerConeAngle(), spot->GetOuterConeAngle()});
            }
        }
    }

    void CheckPayload(const SceneUpdateBatch& batch, size_t frame) const {
        EXPECT_EQ(batch.LightsChanged, _scenario.LightChanges != 0);
        EXPECT_EQ(batch.Lights.Count(), _scenario.LightChanges ? _scenario.Lights : 0u);
        if (_scenario.Kind == Workload::Transform || _scenario.Kind == Workload::SameValue || _scenario.Kind == Workload::HierarchyLeaf) {
            EXPECT_EQ(batch.Transforms.size(), _scenario.Kind == Workload::SameValue ? 0u : std::min<size_t>(_scenario.Changes, _selection.size()));
            EXPECT_TRUE(batch.MeshStates.empty());
            EXPECT_TRUE(batch.CreateShapes.empty());
            EXPECT_TRUE(batch.RemoveShapes.empty());
        }
        if (_scenario.Kind == Workload::Churn || _scenario.Kind == Workload::Burst || _scenario.Kind == Workload::Mixed) {
            const size_t count = _scenario.Kind == Workload::Mixed ? std::min(10u, _scenario.Shapes) : _scenario.Kind == Workload::Burst && frame % 32 != 0 ? 0u
                                                                                                                                                            : _scenario.Changes;
            EXPECT_EQ(batch.CreateShapes.size(), count);
            EXPECT_EQ(batch.RemoveShapes.size(), count);
        }
        if (_scenario.Kind == Workload::Rebind) EXPECT_EQ(batch.MeshStates.size(), _scenario.Changes);
    }

    void Apply(uint32_t flight, const FlightSlot& slot) {
        auto& frame = _rt[slot.Sample];
        const auto& batch = test::SceneBatch(_renderer, _sceneId, flight);
        frame.Transforms = batch.Transforms.size();
        frame.MeshStates = batch.MeshStates.size();
        frame.Creates = batch.CreateShapes.size();
        frame.Removes = batch.RemoveShapes.size();
        frame.LightRecords = batch.Lights.Count();
        frame.PayloadBytes = frame.Transforms * sizeof(ShapeTransformUpdate) + frame.MeshStates * sizeof(StaticMeshStateUpdate) +
                             (frame.Creates + frame.Removes) * sizeof(ShapeId) +
                             batch.Lights.DirectionalLights.size() * sizeof(DirectionalLightData) +
                             batch.Lights.PointLights.size() * sizeof(PointLightData) + batch.Lights.SpotLights.size() * sizeof(SpotLightData) +
                             batch.Lights.RectLights.size() * sizeof(RectLightData);
        frame.Begin = Now();
        _renderer.ConsumeRenderUpdates(flight, slot.Sequence);
        frame.End = Now();
        if (slot.Verify) {
            CheckPayload(batch, slot.Sample);
            const auto scene = _renderer.GetSceneRT(_sceneId);
            frame.Valid = scene && Validate(*scene, slot.Expected);
        }
    }

    double Reclaim(uint32_t flight) {
        auto& slot = *_slots[flight];
        if (!slot.Pending) return 0;
        const auto begin = Now();
        if (_threaded) slot.Done.acquire();
        const auto completed = Now();
        test::CompleteFrame(_renderer, flight, false);
        _assets.Pump();
        _gt[slot.Sample].Completion = Microseconds(completed, Now());
        slot.Pending = false;
        return Microseconds(begin, completed);
    }

    void Drain() {
        for (uint32_t flight = 0; flight < _flights; ++flight) Reclaim(flight);
    }

    void Control(Command command) {
        const uint32_t flight = uint32_t(_ticket % _flights);
        auto& slot = *_slots[flight];
        slot.Job = command;
        _ready.release();
        slot.Done.acquire();
    }

    void Worker() {
        RADRAY_PROFILE_THREAD("RadRay RT");
        if (_gate && !_initialGate.try_acquire_for(std::chrono::seconds(10))) _gateTimedOut = true;
        for (uint64_t ticket = 0;;) {
            _ready.acquire();
            const uint32_t flight = uint32_t(ticket % _flights);
            auto& slot = *_slots[flight];
            const auto command = slot.Job;
            if (command == Command::Frame) {
                Apply(flight, slot);
                ++ticket;
            } else if (command == Command::MergeAllocations)
                MergeThreadAllocations();
            slot.Done.release();
            if (command == Command::Stop) break;
        }
    }

    void Stop() {
        Drain();
        if (_worker.joinable()) {
            Control(Command::Stop);
            _worker.join();
        }
    }

    Scenario _scenario;
    uint32_t _flights;
    bool _threaded, _gate, _gateTimedOut{false}, _allocationTracking{false};
    uint32_t _publishedWhileGated{0};
    uint64_t _ticket{0};
    double _setupUs{0}, _initialSyncUs{0};
    int64_t _setupAllocations{-1}, _setupBytes{-1};
    Application _app;
    AssetManager _assets;
    RenderSystem _renderer;
    test::ScopedWorld _world;
    SceneId _sceneId;
    array<StreamingAssetRef<StaticMesh>, 2> _meshes;
    vector<StaticMeshComponent*> _components;
    vector<LightComponent*> _lights;
    vector<SceneComponent*> _parents;
    vector<uint32_t> _selection;
    vector<ShapeId> _removedShapes;
    vector<GTFrame> _gt;
    vector<RTFrame> _rt;
    vector<unique_ptr<FlightSlot>> _slots;
    std::counting_semaphore<8> _ready{0};
    std::binary_semaphore _initialGate{0};
    std::thread _worker;
};

uint32_t EnvironmentCount(const char* name, uint32_t fallback) {
    Nullable<const char*> value{std::getenv(name)};
    if (!value) return fallback;
    uint32_t result = 0;
    const std::string_view input{value.Get()};
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size() || result == 0 || result > 1000000) {
        ADD_FAILURE() << "Invalid " << name;
        return fallback;
    }
    return result;
}

bool SelectedScenario(std::string_view name) {
    Nullable<const char*> value{std::getenv("RADRAY_SCENE_SYNC_CASES")};
    if (!value || *value.Get() == '\0') return true;
    std::string_view filter{value.Get()};
    while (!filter.empty()) {
        const auto comma = filter.find(',');
        if (filter.substr(0, comma) == name) return true;
        if (comma == std::string_view::npos) break;
        filter.remove_prefix(comma + 1);
    }
    return false;
}

TEST(SceneSyncCorrectness, AllWorkloadsAndFlights) {
    for (const auto& scenario : Scenarios(true))
        for (uint32_t flights : {1u, 2u, 3u})
            for (bool threaded : {false, true}) {
                SCOPED_TRACE(fmt::format("{} F={} threaded={}", scenario.Name, flights, threaded));
                SyncBenchmark benchmark{scenario, flights, threaded, 40};
                benchmark.Run(0, 40, true);
                EXPECT_TRUE(benchmark.FinishAndValidate());
            }
}

TEST(SceneSyncCorrectness, ProducerFillsAllFlightsBeforeConsumerStarts) {
    for (uint32_t flights : {1u, 2u, 3u}) {
        SyncBenchmark benchmark{{"pipeline_gate", Workload::Transform, 128, 32}, flights, true, 12, true};
        benchmark.Run(0, 12, true);
        EXPECT_TRUE(benchmark.FinishAndValidate());
        EXPECT_EQ(benchmark.PublishedWhileGated(), flights);
    }
}

TEST(SceneSyncAllocation, CountsIncludeGTAndRT) {
    if (std::getenv("RADRAY_SCENE_SYNC_ALLOCATIONS") == nullptr) GTEST_SKIP() << "Separate allocation pass only";
    ASSERT_TRUE(TracksContainerAllocations()) << "Container allocations are not observed; use /MT and MI_STATS=FULL";
    std::binary_semaphore ready{0}, start{0}, done{0}, finish{0};
    bool workerOwned = true;
    std::thread worker{[&] {
        {
            vector<byte> warmup(16384);
#ifdef RADRAY_ENABLE_MIMALLOC
            workerOwned = mi_is_in_heap_region(warmup.data());
#endif
        }
        MergeThreadAllocations();
        ready.release();
        start.acquire();
        for (uint32_t i = 0; i < 32; ++i) {
            vector<byte> probe(16384);
#ifdef RADRAY_ENABLE_MIMALLOC
            workerOwned = workerOwned && mi_is_in_heap_region(probe.data());
#endif
        }
        MergeThreadAllocations();
        done.release();
        finish.acquire();
    }};
    ready.acquire();
    const auto before = AllocationTotals();
    start.release();
    vector<byte> probe(16384);
    done.acquire();
    const auto after = AllocationTotals();
    finish.release();
    worker.join();
    EXPECT_TRUE(workerOwned);
    EXPECT_EQ(after[0] - before[0], 33);
    EXPECT_GE(after[1] - before[1], 33 * 16384);
}

TEST(SceneSyncPerformance, Matrix) {
    if (std::getenv("RADRAY_RUN_SCENE_SYNC_BENCHMARK") == nullptr) GTEST_SKIP() << "Use tools/run_scene_sync_benchmark.py";
    Nullable<const char*> path{std::getenv("RADRAY_SCENE_SYNC_OUTPUT")};
    ASSERT_TRUE(path);
    const uint32_t warmup = std::max(64u, EnvironmentCount("RADRAY_SCENE_SYNC_WARMUP", 64));
    const uint32_t count = EnvironmentCount("RADRAY_SCENE_SYNC_FRAMES", 512);
    const bool allocationPass = std::getenv("RADRAY_SCENE_SYNC_ALLOCATIONS") != nullptr;
    const size_t first = 8 + warmup;
    std::ofstream raw{fmt::format("{}.frames.csv", path.Get())};
    std::ofstream cases{fmt::format("{}.cases.csv", path.Get())};
    ASSERT_TRUE(raw.is_open() && cases.is_open());
    raw << "scenario,mode,flights,frame,sequence,start_ns,end_ns,mutation_us,tick_us,lifecycle_us,collect_us,seal_us,publish_us,apply_us,queue_us,e2e_us,flight_wait_us,completion_us,gt_work_us,work_us,transforms,mesh_states,creates,removes,light_records,payload_bytes\n";
    cases << "scenario,mode,flights,shapes,changes,lights,light_changes,repeats,depth,wide,setup_us,initial_sync_us,setup_allocations,setup_bytes,allocations,allocation_bytes,span_us,frames_per_second\n";
    size_t executed = 0;
    vector<uint32_t> flights{1u, 2u, 3u};
    vector<char> threaded{0, 1};
    if (std::getenv("RADRAY_SCENE_SYNC_FLIGHTS") != nullptr) flights = {EnvironmentCount("RADRAY_SCENE_SYNC_FLIGHTS", 2)};
    if (Nullable<const char*> mode{std::getenv("RADRAY_SCENE_SYNC_THREADED")}) {
        const std::string_view value{mode.Get()};
        threaded = {value != "0" && value != "false"};
    }
    for (const auto& scenario : Scenarios(false)) {
        if (!SelectedScenario(scenario.Name)) continue;
        for (uint32_t flightCount : flights)
            for (char isThreaded : threaded) {
                SCOPED_TRACE(fmt::format("{} F={} threaded={}", scenario.Name, flightCount, bool(isThreaded)));
                SyncBenchmark benchmark{scenario, flightCount, bool(isThreaded), first + count};
                benchmark.Run(0, 8, true);
                benchmark.Run(8, warmup, false);
                const auto before = allocationPass ? benchmark.Allocations() : array<int64_t, 2>{-1, -1};
                benchmark.Run(first, count, false);
                const auto after = allocationPass ? benchmark.Allocations() : array<int64_t, 2>{-1, -1};
                ASSERT_TRUE(benchmark.FinishAndValidate());
                const array<int64_t, 2> delta = before[0] < 0 || after[0] < 0 ? array<int64_t, 2>{-1, -1} : array<int64_t, 2>{after[0] - before[0], after[1] - before[1]};
                benchmark.Write(raw, cases, first, count, delta);
                ++executed;
            }
    }
    EXPECT_GT(executed, 0u);
    raw.flush();
    cases.flush();
    EXPECT_TRUE(raw.good() && cases.good());
}

}  // namespace
}  // namespace radray
