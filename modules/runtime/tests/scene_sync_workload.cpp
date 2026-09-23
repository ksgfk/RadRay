#include "scene_sync_workload.h"
#include "scene_test_support.h"

#include <algorithm>
#include <chrono>
#include <random>
#include <semaphore>
#include <thread>
#include <radray/runtime/components/directional_light_component.h>
#include <radray/runtime/components/point_light_component.h>
#include <radray/runtime/components/spot_light_component.h>
#include <radray/runtime/components/static_mesh_component.h>
#include <radray/runtime/game_framework/actor.h>

namespace radray::test {

/// Realistic frame mixes. These compose the single-axis mechanisms above at ratios taken from typical
/// frames instead of extremes; no scenario here exercises a mechanism the other workloads do not.
void AppendRealistic(vector<Scenario>& result, bool small) {
    const auto share = [](uint32_t total, uint32_t divisor) { return std::max(1u, total / divisor); };
    const uint32_t level = small ? 256u : 100000u;
    const uint32_t open = small ? 256u : 20000u;
    const uint32_t stage = small ? 256u : 10000u;
    const uint32_t crowd = small ? 96u : 2000u;
    // 巡游：关卡已加载，相机在动但相机不是 Shape；只有机关/少数 NPC 移动，一盏灯在闪。
    result.push_back({.Name = "level_walkthrough", .Kind = Workload::Level, .Shapes = level, .Changes = share(level, 1000), .Lights = 64, .LightChanges = 1});
    // 交火：1% 物体在动，多盏灯跟着变，少量 LOD 改绑，每 16 帧轮换一批 chunk。
    result.push_back({.Name = "level_firefight", .Kind = Workload::Level, .Shapes = level, .Changes = share(level, 100), .Lights = 64, .LightChanges = 8, .StreamCount = share(level, 500), .StreamPeriod = 16, .RebindCount = share(level, 2000)});
    // 开放世界流式：持续少量移动，每 8 帧成批换入换出 2.5% 的物体。
    result.push_back({.Name = "open_world_streaming", .Kind = Workload::Level, .Shapes = open, .Changes = share(open, 100), .Lights = 32, .LightChanges = 2, .StreamCount = share(open, 40), .StreamPeriod = 8});
    // 群体：每个角色是 1 根 + 2 挂件的宽树，每帧所有根移动，变换沿层级传播。
    result.push_back({.Name = "crowd_animation", .Kind = Workload::Level, .Shapes = crowd, .Changes = share(crowd, 3), .Lights = 16, .LightChanges = 2, .Depth = 3, .Wide = true});
    // 过场：物体动得不多，但全部灯每帧重新捕获。
    result.push_back({.Name = "cinematic_lights", .Kind = Workload::Level, .Shapes = stage, .Changes = share(stage, 50), .Lights = 256, .LightChanges = 256});
    // 编辑器空闲：只有一个物体被拖动，灯不变；用来看每帧的固定开销地板。
    result.push_back({.Name = "editor_idle", .Kind = Workload::Level, .Shapes = open, .Changes = 1, .Lights = 8, .LightChanges = 0});
}

vector<Scenario> SceneSyncScenarios(bool small) {
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
    result.push_back({"reparent_all", Workload::Reparent, n, n});
    result.push_back({"reparent_subtrees", Workload::Reparent, n, changes, 0, 0, 1, 16});
    result.push_back({"chain_16_mixed", Workload::Transform, n, n / 10, 0, 0, 1, 16});
    result.push_back({"mixed", Workload::Mixed, n, changes, 64, 4});
    AppendRealistic(result, small);
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

namespace {
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
    return a.Color.isApprox(b.Color) && a.Intensity == b.Intensity &&
           a.AffectsWorld == b.AffectsWorld && a.CastShadow == b.CastShadow;
}

bool SamePoint(const PointLightParameters& a, const PointLightParameters& b) {
    return a.Position.isApprox(b.Position) && a.Direction.isApprox(b.Direction) &&
           a.AttenuationRadius == b.AttenuationRadius && a.FalloffExponent == b.FalloffExponent &&
           a.SourceRadius == b.SourceRadius && a.SoftSourceRadius == b.SoftSourceRadius &&
           a.SourceLength == b.SourceLength && a.ShadowDepthBias == b.ShadowDepthBias &&
           a.ShadowNormalBias == b.ShadowNormalBias && a.InverseSquaredFalloff == b.InverseSquaredFalloff;
}

bool Validate(const RenderScene& scene, const ExpectedScene& expected) {
    auto lease = scene.AcquireRead();
    if (scene.GetStaticMeshes().size() != expected.Shapes.size() || scene.GetLights().Count() != expected.Lights.Count()) return false;
    for (const auto& e : expected.Shapes) {
        const auto mesh = scene.GetStaticMesh(e.Id);
        if (!mesh || mesh->Mesh.MeshAssetId != e.Asset || mesh->Mesh.GetRenderMesh().Get() != e.Mesh ||
            mesh->Mesh.GetSections().size() != 1 || mesh->Mesh.GetSections()[0].IndexCount != 3 ||
            !mesh->LocalToWorld.isApprox(e.Matrix) || !mesh->WorldBoundsMin.isApprox(e.BoundsMin) ||
            !mesh->WorldBoundsMax.isApprox(e.BoundsMax) || mesh->ReverseCulling != e.ReverseCulling) return false;
    }
    for (const auto id : expected.Removed)
        if (scene.ContainsShape(id)) return false;
    const auto& lights = scene.GetLights();
    if (lights.DirectionalLights.Size() != expected.Lights.DirectionalLights.Size() ||
        lights.PointLights.Size() != expected.Lights.PointLights.Size() ||
        lights.SpotLights.Size() != expected.Lights.SpotLights.Size() || !lights.RectLights.Empty()) return false;
    const auto& expectedDirectional = expected.Lights.DirectionalLights;
    for (size_t row = 0; row < expectedDirectional.Size(); ++row) {
        const auto& e = expectedDirectional.Data[row];
        const auto light = lights.GetDirectionalLight(expectedDirectional.Ids[row]);
        if (!light || !SameCommon(light->Common, e.Common) || !light->Direction.isApprox(e.Direction)) return false;
    }
    const auto& expectedPoint = expected.Lights.PointLights;
    for (size_t row = 0; row < expectedPoint.Size(); ++row) {
        const auto& e = expectedPoint.Data[row];
        const auto light = lights.GetPointLight(expectedPoint.Ids[row]);
        if (!light || !SameCommon(light->Common, e.Common) || !SamePoint(light->Point, e.Point)) return false;
    }
    const auto& expectedSpot = expected.Lights.SpotLights;
    for (size_t row = 0; row < expectedSpot.Size(); ++row) {
        const auto& e = expectedSpot.Data[row];
        const auto light = lights.GetSpotLight(expectedSpot.Ids[row]);
        if (!light || !SameCommon(light->Common, e.Common) || !SamePoint(light->Point, e.Point) ||
            light->InnerConeAngle != e.InnerConeAngle || light->OuterConeAngle != e.OuterConeAngle) return false;
    }
    return true;
}

struct alignas(64) FlightSlot {
    std::binary_semaphore Done{0};
    size_t Frame{0};
    uint64_t Sequence{0};
    bool Pending{false}, Stop{false};
    ExpectedScene Expected;
};
}  // namespace

class SceneSyncWorkload::Impl {
public:
    Impl(Scenario scenario, uint32_t flights, bool threaded, bool verify, bool gate)
        : _scenario(std::move(scenario)), _flights(flights), _threaded(threaded), _verify(verify), _gate(gate),
          _renderer(&_app, flights) {
        for (uint32_t i = 0; i < flights; ++i) _slots.push_back(make_unique<FlightSlot>());
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
        for (uint32_t i = 0; i < _scenario.Shapes; ++i) {
            if (_scenario.Kind == Workload::HierarchyLeaf && (i + 1) % _scenario.Depth != 0 && i + 1 != _scenario.Shapes) continue;
            if ((_scenario.Kind == Workload::HierarchyRoot || _scenario.Kind == Workload::Level || _scenario.Kind == Workload::Reparent) && i % _scenario.Depth != 0) continue;
            _selection.push_back(i);
        }
        std::mt19937 generator{0x5CE1u};
        if (!_scenario.Sequential) std::shuffle(_selection.begin(), _selection.end(), generator);
        _removedShapes.reserve(_scenario.Shapes);
        test::PrepareScene(_world, _renderer, 0);
        test::ConsumeFrame(_renderer, 0);
        test::CompleteFrame(_renderer, 0, false);
        if (_threaded) _worker = std::thread{[this] { Worker(); }};
    }

    ~Impl() { Stop(); }

    void RunFrames(size_t count) {
        for (size_t i = 0; i < count; ++i) {
            const auto flight = uint32_t(_ticket % _flights);
            auto& slot = *_slots[flight];
            Reclaim(flight);
            // Bound float-valued setters while preserving all periodic workloads.
            slot.Frame = size_t(_ticket % 65536);
            Mutate(slot.Frame);
            _world.Tick(1.0f / 60.0f);
            _world.FinalizeWorldGT();
            _world.CollectRenderUpdates();
            _renderer.SealFrameGT(flight);
            if (_verify) CaptureExpected(slot.Expected);
            slot.Sequence = _renderer.GetUpdateSequence(flight);
            _renderer.PublishFrameGT(flight);
            slot.Pending = true;
            ++_ticket;
            if (_threaded) {
                _ready.release();
                if (_gate && _ticket == _flights) {
                    _publishedWhileGated = uint32_t(_ticket);
                    _initialGate.release();
                }
            } else {
                Apply(flight, slot);
            }
        }
        Drain();
    }

    bool FinishAndValidate() {
        Stop();
        ExpectedScene expected;
        CaptureExpected(expected);
        const auto scene = _renderer.GetSceneRT(_sceneId);
        return scene && Validate(*scene, expected) && !_gateTimedOut && _valid;
    }

    uint32_t PublishedWhileGated() const { return _publishedWhileGated; }
    uint64_t Visible() const { return _visible; }

private:
    StaticMeshComponent* SpawnMesh(uint32_t index, Nullable<SceneComponent*> parent = nullptr) {
        auto* component = _world.SpawnActor()->AddSceneComponent<StaticMeshComponent>(parent, AttachmentRule::KeepLocal);
        if (_scenario.UniqueAssets)
            component->SetStaticMesh(_assets.AddReady<StaticMesh>(AssetId{index + 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, MakeMesh(1)));
        else
            component->SetStaticMesh(_meshes[0]);
        component->SetRelativeLocation({float(index % 97), 1, 0});
        component->SetRelativeScale({index % 7 == 0 ? -1.0f : 1.0f, 1, 1});
        return component;
    }

    uint32_t Selected(size_t frame, uint32_t offset) const {
        if (_scenario.Sequential) return _selection[offset];
        return _selection[(frame * 131 + offset) % _selection.size()];
    }

    void Move(size_t frame, uint32_t base, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) {
            auto* component = _components[Selected(frame, base + i)];
            for (uint32_t repeat = 0; repeat < _scenario.Repeats; ++repeat) {
                if (_scenario.Kind == Workload::SameValue)
                    component->SetRelativeLocation(component->GetRelativeLocation());
                else
                    component->SetRelativeLocation({float((frame + 1) * _scenario.Repeats + repeat), float(i % 17), 2});
            }
        }
    }

    void Replace(size_t frame, uint32_t base, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) {
            const auto index = Selected(frame, base + i);
            if (_verify) _removedShapes.push_back(_components[index]->GetShapeId());
            _world.DestroyActor(_components[index]->GetOwner().Get());
            _components[index] = SpawnMesh(index);
        }
    }

    void Rebind(size_t frame, uint32_t base, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) {
            auto* component = _components[Selected(frame, base + i)];
            component->SetStaticMesh(_meshes[component->GetStaticMesh().GetAssetId() == _meshes[0].GetAssetId() ? 1 : 0]);
        }
    }

    void Mutate(size_t frame) {
        _removedShapes.clear();
        const uint32_t count = std::min<uint32_t>(_scenario.Changes, uint32_t(_selection.size()));
        switch (_scenario.Kind) {
            case Workload::Transform:
            case Workload::SameValue:
            case Workload::HierarchyLeaf:
            case Workload::HierarchyRoot: Move(frame, 0, count); break;
            case Workload::Rebind: Rebind(frame, 0, count); break;
            case Workload::Churn: Replace(frame, 0, count); break;
            case Workload::Burst:
                if (frame % 32 == 0) Replace(frame, 0, count);
                break;
            case Workload::Reparent:
                for (uint32_t i = 0; i < count; ++i) {
                    auto* component = _components[Selected(frame, i)];
                    component->RequestReparent(_parents[component->GetAttachParent().Get() == _parents[0] ? 1 : 0], AttachmentRule::KeepLocal);
                }
                break;
            case Workload::Mixed:
                Move(frame, 0, frame % 32 == 0 ? _scenario.Shapes : count);
                Replace(frame, 0, std::min(10u, _scenario.Shapes));
                break;
            case Workload::Level:
                Move(frame, 0, count);
                Rebind(frame, count, _scenario.RebindCount);
                if (frame % _scenario.StreamPeriod == 0) Replace(frame, count + _scenario.RebindCount, _scenario.StreamCount);
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
            common.Color = component->GetLightColor();
            common.Intensity = component->GetIntensity();
            common.AffectsWorld = component->AffectsWorld();
            common.CastShadow = component->CastShadow();
            const auto id = component->GetLightId();
            if (component->GetLightType() == LightType::Directional) {
                expected.Lights.DirectionalLights.Ids.push_back(id);
                expected.Lights.DirectionalLights.Data.push_back({common, component->GetLightDirection()});
                continue;
            }
            auto* point = static_cast<PointLightComponent*>(component);
            PointLightParameters params;
            params.Position = point->GetWorldLocation();
            params.Direction = point->GetLightDirection();
            params.AttenuationRadius = point->GetAttenuationRadius();
            params.FalloffExponent = point->GetLightFalloffExponent();
            params.SourceRadius = point->GetSourceRadius();
            params.SoftSourceRadius = point->GetSoftSourceRadius();
            params.SourceLength = point->GetSourceLength();
            params.ShadowDepthBias = point->GetShadowDepthBias();
            params.ShadowNormalBias = point->GetShadowNormalBias();
            params.InverseSquaredFalloff = point->UseInverseSquaredFalloff();
            if (component->GetLightType() == LightType::Point) {
                expected.Lights.PointLights.Ids.push_back(id);
                expected.Lights.PointLights.Data.push_back({common, params});
            } else {
                auto* spot = static_cast<SpotLightComponent*>(point);
                expected.Lights.SpotLights.Ids.push_back(id);
                expected.Lights.SpotLights.Data.push_back({common, params, spot->GetInnerConeAngle(), spot->GetOuterConeAngle()});
            }
        }
    }

    bool CheckPayload(const SceneUpdateBatch& batch, size_t frame) const {
        bool valid = batch.LightsChanged == (_scenario.LightChanges != 0) &&
                     batch.Lights.Count() == (_scenario.LightChanges ? _scenario.Lights : 0u);
        if (_scenario.Kind == Workload::Transform || _scenario.Kind == Workload::SameValue || _scenario.Kind == Workload::HierarchyLeaf) {
            if (_scenario.Depth == 1 || _scenario.Kind != Workload::Transform)
                valid &= batch.LocalTransforms.size() == (_scenario.Kind == Workload::SameValue ? 0u : std::min<size_t>(_scenario.Changes, _selection.size()));
            valid &= batch.MeshStates.empty() && batch.CreateShapes.empty() && batch.RemoveShapes.empty();
        }
        if (_scenario.Kind == Workload::Churn || _scenario.Kind == Workload::Burst || _scenario.Kind == Workload::Mixed) {
            const size_t count = _scenario.Kind == Workload::Mixed ? std::min(10u, _scenario.Shapes) : _scenario.Kind == Workload::Burst && frame % 32 != 0 ? 0u
                                                                                                                                                            : _scenario.Changes;
            valid &= batch.CreateShapes.size() == count && batch.RemoveShapes.size() == count;
        }
        if (_scenario.Kind == Workload::Rebind) valid &= batch.MeshStates.size() == _scenario.Changes;
        if (_scenario.Kind == Workload::Level) {
            const size_t streamed = frame % _scenario.StreamPeriod == 0 ? _scenario.StreamCount : 0;
            valid &= batch.CreateShapes.size() == streamed && batch.RemoveShapes.size() == streamed;
            valid &= batch.MeshStates.size() == streamed + _scenario.RebindCount;
            valid &= batch.LocalTransforms.size() == std::min<size_t>(_scenario.Changes, _selection.size()) + _scenario.LightChanges;
        }
        return valid;
    }

    void Apply(uint32_t flight, const FlightSlot& slot) {
        _renderer.ConsumeRenderUpdates(flight, slot.Sequence);
        uint64_t visible = 0;
        if (_scenario.Views != 0) {
            const auto scene = _renderer.GetSceneRT(_sceneId);
            const auto lease = scene->AcquireRead();
            const auto bounds = scene->GetStaticMeshColumns().Bounds;
            for (uint32_t view = 0; view < _scenario.Views; ++view)
                for (const auto& bound : bounds)
                    visible += bound.Max.data()[0] >= -float(view + 1);
            _visible += visible;
        }
        if (_verify) {
            const auto& batch = test::SceneBatch(_renderer, _sceneId, flight);
            const auto scene = _renderer.GetSceneRT(_sceneId);
            _valid = _valid && CheckPayload(batch, slot.Frame) && scene && Validate(*scene, slot.Expected);
            uint64_t expectedVisible = 0;
            for (uint32_t view = 0; view < _scenario.Views; ++view)
                for (const auto& shape : slot.Expected.Shapes)
                    expectedVisible += shape.BoundsMax.x() >= -float(view + 1);
            _valid = _valid && visible == expectedVisible;
        }
    }

    void Reclaim(uint32_t flight) {
        auto& slot = *_slots[flight];
        if (!slot.Pending) return;
        if (_threaded) slot.Done.acquire();
        test::CompleteFrame(_renderer, flight, false);
        _assets.Pump();
        slot.Pending = false;
    }

    void Drain() {
        for (uint32_t flight = 0; flight < _flights; ++flight) Reclaim(flight);
    }

    void Worker() {
        if (_gate && !_initialGate.try_acquire_for(std::chrono::seconds(10))) _gateTimedOut = true;
        for (uint64_t ticket = 0;; ++ticket) {
            _ready.acquire();
            const auto flight = uint32_t(ticket % _flights);
            auto& slot = *_slots[flight];
            if (slot.Stop) break;
            Apply(flight, slot);
            slot.Done.release();
        }
    }

    void Stop() {
        Drain();
        if (_worker.joinable()) {
            _slots[_ticket % _flights]->Stop = true;
            _ready.release();
            _worker.join();
        }
    }

    Scenario _scenario;
    uint32_t _flights;
    bool _threaded, _verify, _gate, _gateTimedOut{false}, _valid{true};
    uint32_t _publishedWhileGated{0};
    uint64_t _ticket{0}, _visible{0};
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
    vector<unique_ptr<FlightSlot>> _slots;
    std::counting_semaphore<8> _ready{0};
    std::binary_semaphore _initialGate{0};
    std::thread _worker;
};

SceneSyncWorkload::SceneSyncWorkload(Scenario scenario, uint32_t flights, bool threaded, bool verify, bool gate)
    : _impl(make_unique<Impl>(std::move(scenario), flights, threaded, verify, gate)) {}
SceneSyncWorkload::~SceneSyncWorkload() = default;
void SceneSyncWorkload::RunFrames(size_t count) { _impl->RunFrames(count); }
bool SceneSyncWorkload::FinishAndValidate() { return _impl->FinishAndValidate(); }
uint32_t SceneSyncWorkload::PublishedWhileGated() const { return _impl->PublishedWhileGated(); }
uint64_t SceneSyncWorkload::Visible() const { return _impl->Visible(); }

}  // namespace radray::test
