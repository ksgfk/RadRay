// Tick-driven gameplay frames through S1 and scene delivery. docs/architecture/render-framework.md
#include "scene_test_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <span>

#include <radray/runtime/asset_manager.h>
#include <radray/runtime/components/camera_component.h>
#include <radray/runtime/components/directional_light_component.h>
#include <radray/runtime/components/point_light_component.h>
#include <radray/runtime/components/spot_light_component.h>
#include <radray/runtime/components/static_mesh_component.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/static_mesh.h>

namespace radray {
namespace {

using Eigen::Matrix4f;
using Eigen::Vector3f;

constexpr float kStep = 1.0f;

unique_ptr<StaticMesh> MakeMesh() {
    const array<float, 9> positions{0, 0, 0, 1, 0, 0, 0, 1, 0};
    const array<uint32_t, 3> indices{0, 1, 2};
    MeshResource mesh;
    mesh.Bins.emplace_back(std::as_bytes(std::span{positions}));
    mesh.Bins.emplace_back(std::as_bytes(std::span{indices}));
    MeshPrimitive primitive;
    primitive.VertexCount = 3;
    primitive.VertexBuffers.push_back({"POSITION", 0, 0, VertexDataType::FLOAT, 3, 0, 12});
    primitive.IndexBuffer = {1, 3, 0, 4};
    mesh.Primitives.push_back(std::move(primitive));
    return make_unique<StaticMesh>(std::move(mesh), vector<StaticMeshSection>{}, Vector3f::Zero(), Vector3f::Ones(), GpuMesh{});
}

Vector3f Translation(const Matrix4f& matrix) { return matrix.col(3).head<3>(); }

bool ContainsId(const vector<ShapeId>& ids, ShapeId id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

bool ContainsTransform(const SceneUpdateBatch& batch, ShapeId id) {
    return std::any_of(batch.Transforms.begin(), batch.Transforms.end(), [&](const ShapeTransformUpdate& update) { return update.Id == id; });
}

void ExpectMotionOnly(const SceneUpdateBatch& batch, std::initializer_list<ShapeId> ids) {
    EXPECT_EQ(batch.Transforms.size(), ids.size());
    for (const ShapeId id : ids) EXPECT_TRUE(ContainsTransform(batch, id));
    EXPECT_TRUE(batch.CreateShapes.empty());
    EXPECT_TRUE(batch.RemoveShapes.empty());
    EXPECT_TRUE(batch.MeshStates.empty());
    EXPECT_FALSE(batch.LightsChanged);
    EXPECT_TRUE(batch.Lights.Empty());
}

struct MeshSample {
    bool Found{false};
    Matrix4f Matrix{Matrix4f::Identity()};
    Vector3f BoundsMin{Vector3f::Zero()};
    Vector3f BoundsMax{Vector3f::Zero()};
    bool ReverseCulling{false};
};

MeshSample SampleMesh(const RenderScene& scene, ShapeId id) {
    auto lease = scene.AcquireRead();
    MeshSample sample;
    if (const auto mesh = scene.GetStaticMesh(id)) {
        sample.Found = true;
        sample.Matrix = mesh->LocalToWorld;
        sample.BoundsMin = mesh->WorldBoundsMin;
        sample.BoundsMax = mesh->WorldBoundsMax;
        sample.ReverseCulling = mesh->ReverseCulling;
    }
    return sample;
}

size_t MeshCount(const RenderScene& scene) {
    auto lease = scene.AcquireRead();
    return scene.GetStaticMeshes().size();
}

size_t LightCount(const RenderScene& scene) {
    auto lease = scene.AcquireRead();
    return scene.GetLights().Count();
}

/// F=2 single-thread runner: RT consumes before GT continues, and a flight completes when its slot is reused.
class Session {
public:
    static constexpr uint32_t kFlights = 2;

    Session() : _renderer(&_app, kFlights) {
        _sceneId = test::ConnectWorld(_world, _renderer);
        _mesh = _assets.AddReady<StaticMesh>(AssetId{1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, MakeMesh());
    }

    ~Session() {
        for (uint32_t flight = 0; flight < kFlights; ++flight) {
            if (_pending[flight]) test::CompleteFrame(_renderer, flight, false);
        }
    }

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    World& GetWorld() noexcept { return _world; }
    const StreamingAssetRef<StaticMesh>& Mesh() const noexcept { return _mesh; }

    const RenderScene& Scene() const {
        const auto scene = _renderer.GetSceneRT(_sceneId);
        EXPECT_TRUE(scene);
        return *scene.Get();
    }

    StaticMeshComponent* AddMesh(Actor& actor, Nullable<SceneComponent*> parent, const Vector3f& location, const Vector3f& scale = Vector3f::Ones()) {
        auto* mesh = actor.AddSceneComponent<StaticMeshComponent>(parent, AttachmentRule::KeepLocal);
        mesh->SetStaticMesh(_mesh);
        mesh->SetRelativeLocation(location);
        mesh->SetRelativeScale(scale);
        return mesh;
    }

    SceneUpdateBatch Advance(bool tick) {
        const uint32_t flight = _cursor % kFlights;
        if (_pending[flight]) {
            test::CompleteFrame(_renderer, flight, false);
            _pending[flight] = false;
        }
        if (tick) _world.Tick(kStep);
        _world.FinalizeWorldGT();
        _world.CollectRenderUpdates();
        _renderer.SealFrameGT(flight);
        EXPECT_EQ(_renderer.GetUpdateSequence(flight), _expectedSequence);
        ++_expectedSequence;
        SceneUpdateBatch batch = test::SceneBatch(_renderer, _sceneId, flight);
        test::ConsumeFrame(_renderer, flight);
        _pending[flight] = true;
        ++_cursor;
        return batch;
    }

private:
    Application _app;
    AssetManager _assets;
    RenderSystem _renderer;
    test::ScopedWorld _world;
    SceneId _sceneId{};
    StreamingAssetRef<StaticMesh> _mesh;
    uint32_t _cursor{0};
    uint64_t _expectedSequence{1};
    bool _pending[kFlights]{};
};

class CountingActor : public Actor {
public:
    int Ticks{0};
    void Tick(float) override { ++Ticks; }
};

class Player : public Actor {
public:
    Player() { SetTickEnabled(true); }
    SceneComponent* Root{nullptr};
    int Ticks{0};

    void Tick(float deltaTime) override {
        ++Ticks;
        const Vector3f location = Root->GetRelativeLocation();
        Root->SetRelativeLocation(location + Vector3f{deltaTime, 0, 0});
    }
};

class Walker : public Actor {
public:
    Walker() { SetTickEnabled(true); }
    SceneComponent* Root{nullptr};
    int Ticks{0};

    void Tick(float deltaTime) override {
        ++Ticks;
        const Vector3f location = Root->GetRelativeLocation();
        Root->SetRelativeLocation(location + Vector3f{deltaTime, 0, 0});
    }
};

class PulsingLight : public PointLightComponent {
public:
    PulsingLight() { SetTickEnabled(true); }
    int Pulses{0};

    void TickComponent(float) override {
        ++Pulses;
        SetIntensity(1.0f + float(Pulses));
    }
};

struct Shot {
    ShapeId Id{};
    vector<float> Samples;
    bool Spawned{false};
};

struct Firefight {
    StreamingAssetRef<StaticMesh> Mesh;
    Shot Shots[2]{};
    ShapeId Puff{};
};

class Projectile : public Actor {
public:
    explicit Projectile(Shot* shot) : _shot(shot) { SetTickEnabled(true); }

    void Tick(float deltaTime) override {
        auto* mesh = FindComponent<StaticMeshComponent>().Get();
        Vector3f location = mesh->GetRelativeLocation();
        location.z() += deltaTime;
        mesh->SetRelativeLocation(location);
        _shot->Samples.push_back(location.z());
        if (_shot->Samples.size() == 3) GetWorld().Get()->DestroyActor(this);
    }

private:
    Shot* _shot;
};

class Gunner : public Actor {
public:
    explicit Gunner(Firefight* fight) : _fight(fight) { SetTickEnabled(true); }
    int Ticks{0};

    void Tick(float) override {
        ++Ticks;
        if (Ticks == 1) {
            SpawnShot(&_fight->Shots[0]);
            SpawnPuff();
        } else if (Ticks == 2)
            SpawnShot(&_fight->Shots[1]);
    }

private:
    void SpawnShot(Shot* shot) {
        auto* actor = GetWorld().Get()->SpawnActor<Projectile>(shot);
        auto* mesh = actor->AddComponent<StaticMeshComponent>();
        mesh->SetStaticMesh(_fight->Mesh);
        shot->Id = mesh->GetShapeId();
        shot->Spawned = true;
    }

    void SpawnPuff() {
        auto* actor = GetWorld().Get()->SpawnActor();
        auto* mesh = actor->AddComponent<StaticMeshComponent>();
        mesh->SetStaticMesh(_fight->Mesh);
        mesh->SetRelativeLocation({50, 50, 50});
        _fight->Puff = mesh->GetShapeId();
        GetWorld().Get()->DestroyActor(actor);
    }

    Firefight* _fight;
};

struct StreamLog {
    struct Event {
        array<ShapeId, 2> Removed{};
        array<ShapeId, 2> Created{};
    };
    StreamingAssetRef<StaticMesh> Mesh;
    vector<StaticMeshComponent*> Props;
    vector<Event> Events;
};

class Streamer : public Actor {
public:
    Streamer() { SetTickEnabled(true); }
    StaticMeshComponent* Body{nullptr};
    StreamLog* Log{nullptr};
    int Ticks{0};

    void Tick(float deltaTime) override {
        ++Ticks;
        const Vector3f location = Body->GetRelativeLocation();
        Body->SetRelativeLocation(location + Vector3f{0, 0, deltaTime});
        if (Ticks % 4 != 0) return;
        StreamLog::Event event;
        for (int i = 0; i < 2; ++i) {
            event.Removed[static_cast<size_t>(i)] = Log->Props[static_cast<size_t>(i)]->GetShapeId();
            GetWorld().Get()->DestroyActor(Log->Props[static_cast<size_t>(i)]->GetOwner().Get());
            auto* actor = GetWorld().Get()->SpawnActor();
            auto* mesh = actor->AddComponent<StaticMeshComponent>();
            mesh->SetStaticMesh(Log->Mesh);
            mesh->SetRelativeLocation({100.0f + float(i), 0, 0});
            event.Created[static_cast<size_t>(i)] = mesh->GetShapeId();
            Log->Props[static_cast<size_t>(i)] = mesh;
        }
        Log->Events.push_back(event);
    }
};

class Gaffer : public Actor {
public:
    Gaffer() { SetTickEnabled(true); }
    PointLightComponent* Key{nullptr};
    SpotLightComponent* Rim{nullptr};
    int Ticks{0};

    void Tick(float) override {
        ++Ticks;
        Key->SetIntensity(2.0f + float(Ticks));
        Key->SetRelativeLocation({float(Ticks), 4, 1});
        if (Ticks == 1) Rim->SetIntensity(0.25f);
    }
};

TEST(FrameScenarios, PatrolPublishesOnlyTheMovingCharacter) {
    Session session;
    auto* dormant = session.GetWorld().SpawnActor<CountingActor>();
    auto* dormantMesh = session.AddMesh(*dormant, nullptr, {0, 2, 5});
    vector<StaticMeshComponent*> scenery;
    for (int i = 0; i < 7; ++i) {
        auto* actor = session.GetWorld().SpawnActor();
        const Vector3f scale = i == 3 ? Vector3f{-1, 1, 1} : Vector3f::Ones();
        scenery.push_back(session.AddMesh(*actor, nullptr, {float(i), 0, 5}, scale));
    }
    auto* player = session.GetWorld().SpawnActor<Player>();
    auto* root = player->AddComponent<SceneComponent>();
    player->RequestSetRootComponent(root);
    player->Root = root;
    auto* body = session.AddMesh(*player, root, Vector3f::Zero());
    auto* weapon = session.AddMesh(*player, body, {0.5f, 1.25f, 0.25f});
    auto* camera = player->AddSceneComponent<CameraComponent>(body, AttachmentRule::KeepLocal);
    camera->SetRelativeLocation({0, 1.5f, 0.25f});
    camera->SetPerspective(Radian(60.0f), 0.1f, 100.0f);
    auto* sunActor = session.GetWorld().SpawnActor();
    auto* sun = sunActor->AddComponent<DirectionalLightComponent>();
    sun->SetIntensity(1.2f);

    const auto initial = session.Advance(false);
    EXPECT_EQ(initial.CreateShapes.size(), 10u);
    EXPECT_EQ(initial.MeshStates.size(), 10u);
    EXPECT_TRUE(initial.LightsChanged);
    EXPECT_EQ(initial.Lights.Count(), 1u);
    EXPECT_EQ(MeshCount(session.Scene()), 10u);
    EXPECT_EQ(LightCount(session.Scene()), 1u);
    const auto mirrored = SampleMesh(session.Scene(), scenery[3]->GetShapeId());
    ASSERT_TRUE(mirrored.Found);
    EXPECT_TRUE(mirrored.ReverseCulling);
    EXPECT_FLOAT_EQ(mirrored.BoundsMin.x(), 2.0f);
    EXPECT_FLOAT_EQ(mirrored.BoundsMax.x(), 3.0f);

    for (int frame = 1; frame <= 3; ++frame) {
        SCOPED_TRACE(frame);
        const auto batch = session.Advance(true);
        ExpectMotionOnly(batch, {body->GetShapeId(), weapon->GetShapeId()});
        EXPECT_EQ(player->Ticks, frame);
        EXPECT_EQ(dormant->Ticks, 0);
        const Vector3f expectedBody{float(frame), 0, 0};
        const Vector3f expectedWeapon{float(frame) + 0.5f, 1.25f, 0.25f};
        EXPECT_TRUE(body->GetWorldLocation().isApprox(expectedBody));
        EXPECT_TRUE(weapon->GetWorldLocation().isApprox(expectedWeapon));
        EXPECT_TRUE(camera->GetEyePosition().isApprox(Vector3f{float(frame), 1.5f, 0.25f}));
        const auto bodySample = SampleMesh(session.Scene(), body->GetShapeId());
        const auto weaponSample = SampleMesh(session.Scene(), weapon->GetShapeId());
        ASSERT_TRUE(bodySample.Found && weaponSample.Found);
        EXPECT_TRUE(bodySample.Matrix.isApprox(body->GetWorldMatrix()));
        EXPECT_TRUE(weaponSample.Matrix.isApprox(weapon->GetWorldMatrix()));
        EXPECT_FALSE(bodySample.ReverseCulling);
        const auto stillMirrored = SampleMesh(session.Scene(), scenery[3]->GetShapeId());
        ASSERT_TRUE(stillMirrored.Found);
        EXPECT_TRUE(stillMirrored.Matrix.isApprox(mirrored.Matrix));
        EXPECT_TRUE(stillMirrored.ReverseCulling);
        EXPECT_TRUE(SampleMesh(session.Scene(), dormantMesh->GetShapeId()).Matrix.isApprox(dormantMesh->GetWorldMatrix()));
    }

    player->SetTickEnabled(false);
    ExpectMotionOnly(session.Advance(true), {});
    ExpectMotionOnly(session.Advance(true), {});
    EXPECT_EQ(player->Ticks, 3);
    EXPECT_EQ(dormant->Ticks, 0);
    EXPECT_TRUE(camera->GetEyePosition().isApprox(Vector3f{3, 1.5f, 0.25f}));

    player->SetTickEnabled(true);
    ExpectMotionOnly(session.Advance(true), {body->GetShapeId(), weapon->GetShapeId()});
    EXPECT_EQ(player->Ticks, 4);
    EXPECT_TRUE(body->GetWorldLocation().isApprox(Vector3f{4, 0, 0}));
    EXPECT_EQ(MeshCount(session.Scene()), 10u);
    EXPECT_EQ(LightCount(session.Scene()), 1u);
    auto lease = session.Scene().AcquireRead();
    const auto publishedSun = session.Scene().GetLights().GetDirectionalLight(sun->GetLightId());
    ASSERT_TRUE(publishedSun);
    EXPECT_FLOAT_EQ(publishedSun->Common.Intensity, 1.2f);
}

TEST(FrameScenarios, FirefightProjectilesTickNextFrameAndCancelSameFramePuffs) {
    Session session;
    Firefight fight;
    fight.Mesh = session.Mesh();
    vector<ShapeId> covers;
    for (int i = 0; i < 4; ++i) {
        auto* actor = session.GetWorld().SpawnActor<CountingActor>();
        covers.push_back(session.AddMesh(*actor, nullptr, {10.0f + float(i), 0, 0})->GetShapeId());
    }
    auto* gunner = session.GetWorld().SpawnActor<Gunner>(&fight);
    const auto gunnerShape = session.AddMesh(*gunner, nullptr, Vector3f::Zero())->GetShapeId();
    auto* lamp = session.GetWorld().SpawnActor<CountingActor>();
    const auto lampShape = session.AddMesh(*lamp, nullptr, {0, 3, 0})->GetShapeId();
    auto* flash = lamp->AddComponent<PulsingLight>();
    flash->SetRelativeLocation({0, 3, 1});
    auto* sun = session.GetWorld().SpawnActor()->AddComponent<DirectionalLightComponent>();
    sun->SetIntensity(1.0f);
    session.Advance(false);
    ASSERT_EQ(MeshCount(session.Scene()), 6u);

    const auto spawned = session.Advance(true);
    EXPECT_EQ(gunner->Ticks, 1);
    EXPECT_EQ(lamp->Ticks, 0);
    EXPECT_EQ(flash->Pulses, 1);
    ASSERT_TRUE(fight.Shots[0].Spawned);
    EXPECT_TRUE(fight.Shots[0].Samples.empty());
    EXPECT_FALSE(fight.Shots[1].Spawned);
    EXPECT_EQ(spawned.CreateShapes.size(), 1u);
    EXPECT_EQ(spawned.CreateShapes[0], fight.Shots[0].Id);
    EXPECT_EQ(spawned.MeshStates.size(), 1u);
    EXPECT_TRUE(Translation(spawned.MeshStates[0].LocalToWorld).isApprox(Vector3f::Zero()));
    EXPECT_TRUE(spawned.Transforms.empty());
    EXPECT_TRUE(spawned.RemoveShapes.empty());
    EXPECT_FALSE(ContainsId(spawned.CreateShapes, fight.Puff));
    EXPECT_FALSE(ContainsId(spawned.RemoveShapes, fight.Puff));
    EXPECT_FALSE(SampleMesh(session.Scene(), fight.Puff).Found);
    ASSERT_TRUE(SampleMesh(session.Scene(), fight.Shots[0].Id).Found);
    EXPECT_TRUE(spawned.LightsChanged);
    EXPECT_EQ(spawned.Lights.Count(), 2u);
    ASSERT_TRUE(spawned.Lights.GetPointLight(flash->GetLightId()));
    EXPECT_FLOAT_EQ(spawned.Lights.GetPointLight(flash->GetLightId())->Common.Intensity, 2.0f);
    ASSERT_TRUE(spawned.Lights.GetDirectionalLight(sun->GetLightId()));
    EXPECT_FLOAT_EQ(spawned.Lights.GetDirectionalLight(sun->GetLightId())->Common.Intensity, 1.0f);
    EXPECT_EQ(MeshCount(session.Scene()), 7u);

    const auto second = session.Advance(true);
    EXPECT_EQ(fight.Shots[0].Samples, (vector<float>{1}));
    ASSERT_TRUE(fight.Shots[1].Spawned);
    EXPECT_TRUE(fight.Shots[1].Samples.empty());
    EXPECT_EQ(second.CreateShapes, (vector<ShapeId>{fight.Shots[1].Id}));
    EXPECT_EQ(second.Transforms.size(), 1u);
    EXPECT_EQ(second.Transforms[0].Id, fight.Shots[0].Id);
    EXPECT_TRUE(Translation(second.Transforms[0].LocalToWorld).isApprox(Vector3f{0, 0, 1}));
    ASSERT_EQ(second.MeshStates.size(), 1u);
    EXPECT_TRUE(Translation(second.MeshStates[0].LocalToWorld).isApprox(Vector3f::Zero()));
    EXPECT_FALSE(ContainsTransform(second, gunnerShape));
    EXPECT_FALSE(ContainsTransform(second, lampShape));

    const auto third = session.Advance(true);
    EXPECT_EQ(fight.Shots[0].Samples, (vector<float>{1, 2}));
    EXPECT_EQ(fight.Shots[1].Samples, (vector<float>{1}));
    EXPECT_TRUE(third.CreateShapes.empty());
    EXPECT_TRUE(third.RemoveShapes.empty());
    EXPECT_EQ(third.Transforms.size(), 2u);

    const auto fourth = session.Advance(true);
    EXPECT_EQ(fight.Shots[0].Samples, (vector<float>{1, 2, 3}));
    EXPECT_EQ(fight.Shots[1].Samples, (vector<float>{1, 2}));
    EXPECT_EQ(fourth.RemoveShapes, (vector<ShapeId>{fight.Shots[0].Id}));
    EXPECT_FALSE(ContainsTransform(fourth, fight.Shots[0].Id));
    EXPECT_EQ(fourth.Transforms.size(), 1u);
    EXPECT_EQ(fourth.Transforms[0].Id, fight.Shots[1].Id);
    EXPECT_FALSE(SampleMesh(session.Scene(), fight.Shots[0].Id).Found);
    EXPECT_TRUE(Translation(SampleMesh(session.Scene(), fight.Shots[1].Id).Matrix).isApprox(Vector3f{0, 0, 2}));

    const auto fifth = session.Advance(true);
    EXPECT_EQ(fight.Shots[1].Samples, (vector<float>{1, 2, 3}));
    EXPECT_EQ(fifth.RemoveShapes, (vector<ShapeId>{fight.Shots[1].Id}));
    EXPECT_TRUE(fifth.Transforms.empty());
    EXPECT_FALSE(SampleMesh(session.Scene(), fight.Shots[1].Id).Found);
    EXPECT_EQ(MeshCount(session.Scene()), 6u);
    EXPECT_EQ(lamp->Ticks, 0);
    EXPECT_EQ(flash->Pulses, 5);
    for (const auto& actor : session.GetWorld().GetActors()) {
        if (const auto* cover = dynamic_cast<const CountingActor*>(actor.get()); cover && cover != lamp)
            EXPECT_EQ(cover->Ticks, 0);
    }
    for (const ShapeId cover : covers) EXPECT_TRUE(SampleMesh(session.Scene(), cover).Found);
    auto lease = session.Scene().AcquireRead();
    ASSERT_TRUE(session.Scene().GetLights().GetDirectionalLight(sun->GetLightId()));
    EXPECT_FLOAT_EQ(session.Scene().GetLights().GetDirectionalLight(sun->GetLightId())->Common.Intensity, 1.0f);
    ASSERT_TRUE(session.Scene().GetLights().GetPointLight(flash->GetLightId()));
    EXPECT_FLOAT_EQ(session.Scene().GetLights().GetPointLight(flash->GetLightId())->Common.Intensity, 6.0f);
}

TEST(FrameScenarios, StreamingSwapReplacesChunksWithoutResendingLights) {
    Session session;
    StreamLog log;
    log.Mesh = session.Mesh();
    for (int i = 0; i < 6; ++i) {
        auto* actor = session.GetWorld().SpawnActor();
        log.Props.push_back(session.AddMesh(*actor, nullptr, {float(i), 0, 0}));
    }
    auto* streamer = session.GetWorld().SpawnActor<Streamer>();
    streamer->Log = &log;
    streamer->Body = session.AddMesh(*streamer, nullptr, Vector3f::Zero());
    auto* sun = session.GetWorld().SpawnActor()->AddComponent<DirectionalLightComponent>();
    sun->SetIntensity(0.8f);
    session.Advance(false);
    const auto survivor = log.Props[4]->GetShapeId();
    const Matrix4f survivorMatrix = log.Props[4]->GetWorldMatrix();

    for (int frame = 1; frame <= 3; ++frame) {
        SCOPED_TRACE(frame);
        const auto batch = session.Advance(true);
        ExpectMotionOnly(batch, {streamer->Body->GetShapeId()});
        EXPECT_TRUE(streamer->Body->GetWorldLocation().isApprox(Vector3f{0, 0, float(frame)}));
    }

    const auto swapped = session.Advance(true);
    ASSERT_EQ(log.Events.size(), 1u);
    const auto& event = log.Events[0];
    EXPECT_EQ(swapped.RemoveShapes.size(), 2u);
    EXPECT_EQ(swapped.CreateShapes.size(), 2u);
    EXPECT_EQ(swapped.MeshStates.size(), 2u);
    EXPECT_EQ(swapped.Transforms.size(), 1u);
    EXPECT_EQ(swapped.Transforms[0].Id, streamer->Body->GetShapeId());
    EXPECT_FALSE(swapped.LightsChanged);
    for (int i = 0; i < 2; ++i) {
        EXPECT_TRUE(ContainsId(swapped.RemoveShapes, event.Removed[static_cast<size_t>(i)]));
        EXPECT_TRUE(ContainsId(swapped.CreateShapes, event.Created[static_cast<size_t>(i)]));
        EXPECT_FALSE(SampleMesh(session.Scene(), event.Removed[static_cast<size_t>(i)]).Found);
        const auto created = SampleMesh(session.Scene(), event.Created[static_cast<size_t>(i)]);
        ASSERT_TRUE(created.Found);
        EXPECT_TRUE(Translation(created.Matrix).isApprox(Vector3f{100.0f + float(i), 0, 0}));
    }
    EXPECT_FALSE(ContainsTransform(swapped, survivor));
    EXPECT_FALSE(ContainsId(swapped.RemoveShapes, survivor));
    const auto survivorSample = SampleMesh(session.Scene(), survivor);
    ASSERT_TRUE(survivorSample.Found);
    EXPECT_TRUE(survivorSample.Matrix.isApprox(survivorMatrix));
    EXPECT_TRUE(streamer->Body->GetWorldLocation().isApprox(Vector3f{0, 0, 4}));
    EXPECT_EQ(MeshCount(session.Scene()), 7u);
    EXPECT_EQ(LightCount(session.Scene()), 1u);
    auto lease = session.Scene().AcquireRead();
    ASSERT_TRUE(session.Scene().GetLights().GetDirectionalLight(sun->GetLightId()));
    EXPECT_FLOAT_EQ(session.Scene().GetLights().GetDirectionalLight(sun->GetLightId())->Common.Intensity, 0.8f);
}

TEST(FrameScenarios, CrowdRootMotionPublishesAttachments) {
    Session session;
    struct Member {
        Walker* Actor{nullptr};
        ShapeId Body{};
        ShapeId Hat{};
        float Origin{0};
    };
    array<Member, 4> crowd{};
    for (int i = 0; i < 4; ++i) {
        auto* walker = session.GetWorld().SpawnActor<Walker>();
        auto* root = walker->AddComponent<SceneComponent>();
        walker->RequestSetRootComponent(root);
        walker->Root = root;
        root->SetRelativeLocation({float(i) * 3.0f, 0, 0});
        auto* body = session.AddMesh(*walker, root, Vector3f::Zero());
        auto* hat = session.AddMesh(*walker, body, {0, 2, 0});
        crowd[static_cast<size_t>(i)] = {walker, body->GetShapeId(), hat->GetShapeId(), float(i) * 3.0f};
    }
    auto* sun = session.GetWorld().SpawnActor()->AddComponent<DirectionalLightComponent>();
    session.Advance(false);

    for (int frame = 1; frame <= 2; ++frame) {
        SCOPED_TRACE(frame);
        const auto batch = session.Advance(true);
        EXPECT_EQ(batch.Transforms.size(), 8u);
        EXPECT_TRUE(batch.MeshStates.empty());
        EXPECT_FALSE(batch.LightsChanged);
        for (const Member& member : crowd) {
            EXPECT_EQ(member.Actor->Ticks, frame);
            EXPECT_TRUE(ContainsTransform(batch, member.Body));
            EXPECT_TRUE(ContainsTransform(batch, member.Hat));
            const Vector3f body{member.Origin + float(frame), 0, 0};
            const Vector3f hat{member.Origin + float(frame), 2, 0};
            const auto bodySample = SampleMesh(session.Scene(), member.Body);
            const auto hatSample = SampleMesh(session.Scene(), member.Hat);
            ASSERT_TRUE(bodySample.Found && hatSample.Found);
            EXPECT_TRUE(Translation(bodySample.Matrix).isApprox(body));
            EXPECT_TRUE(Translation(hatSample.Matrix).isApprox(hat));
        }
    }
    EXPECT_EQ(MeshCount(session.Scene()), 8u);
    EXPECT_EQ(LightCount(session.Scene()), 1u);
    EXPECT_TRUE(sun->GetLightId().IsValid());
}

TEST(FrameScenarios, PauseStillDeliversDestructionAndExternalEdits) {
    Session session;
    auto* player = session.GetWorld().SpawnActor<Player>();
    auto* root = player->AddComponent<SceneComponent>();
    player->Root = root;
    auto* body = session.AddMesh(*player, root, Vector3f::Zero());
    auto* propA = session.AddMesh(*session.GetWorld().SpawnActor(), nullptr, {2, 0, 0});
    auto* propB = session.AddMesh(*session.GetWorld().SpawnActor(), nullptr, {4, 0, 0});
    const ShapeId removed = propA->GetShapeId();
    const ShapeId edited = propB->GetShapeId();
    session.Advance(false);
    session.Advance(true);
    session.Advance(true);
    EXPECT_EQ(player->Ticks, 2);
    const Matrix4f parked = body->GetWorldMatrix();

    session.GetWorld().SetTickEnabled(false);
    ExpectMotionOnly(session.Advance(true), {});
    EXPECT_EQ(player->Ticks, 2);
    EXPECT_TRUE(SampleMesh(session.Scene(), body->GetShapeId()).Matrix.isApprox(parked));

    session.GetWorld().DestroyActor(propA->GetOwner().Get());
    const auto destruction = session.Advance(true);
    EXPECT_EQ(player->Ticks, 2);
    EXPECT_EQ(destruction.RemoveShapes, (vector<ShapeId>{removed}));
    EXPECT_TRUE(destruction.Transforms.empty());
    EXPECT_TRUE(destruction.CreateShapes.empty());
    EXPECT_FALSE(destruction.LightsChanged);
    EXPECT_FALSE(SampleMesh(session.Scene(), removed).Found);
    EXPECT_TRUE(SampleMesh(session.Scene(), body->GetShapeId()).Matrix.isApprox(parked));

    propB->SetRelativeLocation({8, 0, 0});
    const auto edit = session.Advance(true);
    ExpectMotionOnly(edit, {edited});
    EXPECT_EQ(player->Ticks, 2);
    EXPECT_TRUE(Translation(SampleMesh(session.Scene(), edited).Matrix).isApprox(Vector3f{8, 0, 0}));
    EXPECT_TRUE(SampleMesh(session.Scene(), body->GetShapeId()).Matrix.isApprox(parked));

    session.GetWorld().SetTickEnabled(true);
    ExpectMotionOnly(session.Advance(true), {body->GetShapeId()});
    EXPECT_EQ(player->Ticks, 3);
    EXPECT_TRUE(body->GetWorldLocation().isApprox(Vector3f{3, 0, 0}));
    EXPECT_TRUE(Translation(SampleMesh(session.Scene(), edited).Matrix).isApprox(Vector3f{8, 0, 0}));
    EXPECT_EQ(MeshCount(session.Scene()), 2u);
}

TEST(FrameScenarios, CinematicReplacesTheFullLightTable) {
    Session session;
    vector<ShapeId> props;
    vector<Matrix4f> propMatrices;
    for (int i = 0; i < 4; ++i) {
        auto* mesh = session.AddMesh(*session.GetWorld().SpawnActor(), nullptr, {float(i), 1, 0});
        props.push_back(mesh->GetShapeId());
        propMatrices.push_back(mesh->GetWorldMatrix());
    }
    auto* fill = session.GetWorld().SpawnActor()->AddComponent<DirectionalLightComponent>();
    fill->SetIntensity(1.5f);
    auto* gaffer = session.GetWorld().SpawnActor<Gaffer>();
    gaffer->Key = session.GetWorld().SpawnActor()->AddComponent<PointLightComponent>();
    gaffer->Key->SetRelativeLocation({0, 4, 1});
    gaffer->Rim = session.GetWorld().SpawnActor()->AddComponent<SpotLightComponent>();
    gaffer->Rim->SetIntensity(1.0f);
    session.Advance(false);

    for (int frame = 1; frame <= 3; ++frame) {
        SCOPED_TRACE(frame);
        const auto batch = session.Advance(true);
        EXPECT_TRUE(batch.Transforms.empty());
        EXPECT_TRUE(batch.MeshStates.empty());
        EXPECT_TRUE(batch.CreateShapes.empty());
        EXPECT_TRUE(batch.RemoveShapes.empty());
        EXPECT_TRUE(batch.LightsChanged);
        EXPECT_EQ(batch.Lights.Count(), 3u);
        EXPECT_TRUE(batch.Lights.RectLights.Empty());
        const auto* directional = batch.Lights.GetDirectionalLight(fill->GetLightId()).Get();
        const auto* point = batch.Lights.GetPointLight(gaffer->Key->GetLightId()).Get();
        const auto* spot = batch.Lights.GetSpotLight(gaffer->Rim->GetLightId()).Get();
        ASSERT_TRUE(directional && point && spot);
        EXPECT_FLOAT_EQ(directional->Common.Intensity, 1.5f);
        EXPECT_FLOAT_EQ(point->Common.Intensity, 2.0f + float(frame));
        EXPECT_TRUE(point->Point.Position.isApprox(Vector3f{float(frame), 4, 1}));
        EXPECT_FLOAT_EQ(spot->Common.Intensity, 0.25f);
        EXPECT_FLOAT_EQ(spot->InnerConeAngle, gaffer->Rim->GetInnerConeAngle());
        EXPECT_FLOAT_EQ(spot->OuterConeAngle, gaffer->Rim->GetOuterConeAngle());
        for (size_t i = 0; i < props.size(); ++i) {
            const auto sample = SampleMesh(session.Scene(), props[i]);
            ASSERT_TRUE(sample.Found);
            EXPECT_TRUE(sample.Matrix.isApprox(propMatrices[i]));
        }
    }
    EXPECT_EQ(gaffer->Ticks, 3);
    EXPECT_EQ(MeshCount(session.Scene()), 4u);
    EXPECT_EQ(LightCount(session.Scene()), 3u);
}

TEST(FrameScenarios, EditorDragSkipsIdenticalWritesAndIdleTicks) {
    Session session;
    auto* dormant = session.GetWorld().SpawnActor<CountingActor>();
    vector<StaticMeshComponent*> meshes;
    meshes.push_back(session.AddMesh(*dormant, nullptr, {0, 0, 0}));
    for (int i = 1; i < 12; ++i) meshes.push_back(session.AddMesh(*session.GetWorld().SpawnActor(), nullptr, {float(i), 0, 0}));
    auto* dragged = meshes[3];
    auto* sun = session.GetWorld().SpawnActor()->AddComponent<DirectionalLightComponent>();
    sun->SetIntensity(1.1f);
    auto* practical = session.GetWorld().SpawnActor()->AddComponent<PointLightComponent>();
    practical->SetRelativeLocation({1, 2, 3});
    session.Advance(false);
    const Matrix4f untouched = meshes[7]->GetWorldMatrix();

    dragged->SetRelativeLocation({2, 3, 4});
    const auto drag = session.Advance(false);
    ExpectMotionOnly(drag, {dragged->GetShapeId()});
    EXPECT_EQ(dormant->Ticks, 0);
    EXPECT_TRUE(Translation(SampleMesh(session.Scene(), dragged->GetShapeId()).Matrix).isApprox(Vector3f{2, 3, 4}));

    ExpectMotionOnly(session.Advance(false), {});
    EXPECT_TRUE(Translation(SampleMesh(session.Scene(), dragged->GetShapeId()).Matrix).isApprox(Vector3f{2, 3, 4}));
    dragged->SetRelativeLocation({2, 3, 4});
    ExpectMotionOnly(session.Advance(false), {});

    dragged->SetRelativeScale({-1, 1, 1});
    const auto mirror = session.Advance(false);
    ExpectMotionOnly(mirror, {dragged->GetShapeId()});
    const auto mirrored = SampleMesh(session.Scene(), dragged->GetShapeId());
    ASSERT_TRUE(mirrored.Found);
    EXPECT_TRUE(mirrored.ReverseCulling);
    EXPECT_FLOAT_EQ(mirrored.BoundsMin.x(), 1.0f);
    EXPECT_FLOAT_EQ(mirrored.BoundsMax.x(), 2.0f);
    dragged->SetRelativeScale({-1, 1, 1});
    ExpectMotionOnly(session.Advance(false), {});
    EXPECT_TRUE(SampleMesh(session.Scene(), dragged->GetShapeId()).ReverseCulling);

    practical->SetRelativeLocation({5, 6, 7});
    practical->SetIntensity(3.0f);
    const auto relight = session.Advance(false);
    EXPECT_TRUE(relight.Transforms.empty());
    EXPECT_TRUE(relight.MeshStates.empty());
    EXPECT_TRUE(relight.LightsChanged);
    EXPECT_EQ(relight.Lights.Count(), 2u);
    ASSERT_TRUE(relight.Lights.GetDirectionalLight(sun->GetLightId()));
    EXPECT_FLOAT_EQ(relight.Lights.GetDirectionalLight(sun->GetLightId())->Common.Intensity, 1.1f);
    ASSERT_TRUE(relight.Lights.GetPointLight(practical->GetLightId()));
    EXPECT_FLOAT_EQ(relight.Lights.GetPointLight(practical->GetLightId())->Common.Intensity, 3.0f);
    EXPECT_TRUE(relight.Lights.GetPointLight(practical->GetLightId())->Point.Position.isApprox(Vector3f{5, 6, 7}));
    EXPECT_TRUE(SampleMesh(session.Scene(), dragged->GetShapeId()).ReverseCulling);
    EXPECT_TRUE(SampleMesh(session.Scene(), meshes[7]->GetShapeId()).Matrix.isApprox(untouched));

    ExpectMotionOnly(session.Advance(true), {});
    EXPECT_EQ(dormant->Ticks, 0);
    EXPECT_EQ(MeshCount(session.Scene()), 12u);
    EXPECT_EQ(LightCount(session.Scene()), 2u);
    EXPECT_TRUE(SampleMesh(session.Scene(), dragged->GetShapeId()).ReverseCulling);
}

}  // namespace
}  // namespace radray
