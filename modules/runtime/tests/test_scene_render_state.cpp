#include "upload_test_support.h"
#include "runtime_test_support.h"
#include "gpu_test_fixture.h"

#include <gtest/gtest.h>
#include <radray/runtime/components/static_mesh_component.h>
#include <radray/runtime/components/point_light_component.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_framework/light_scene_proxy.h>
#include <radray/runtime/render_framework/primitive_history.h>
#include <radray/runtime/render_framework/scene.h>

namespace radray {
namespace {

class TransformObserver final : public SceneComponent {
public:
    Eigen::Matrix4f Observed{Eigen::Matrix4f::Identity()};

protected:
    void OnTransformChanged() override { Observed = GetWorldMatrix(); }
};

TEST(SceneTransformTest, AncestorChangesReparentingAndDestructionReachDescendants) {
    SceneComponent root;
    TransformObserver child, grandchild;
    child.AttachTo(&root);
    grandchild.AttachTo(&child);
    const auto check = [&] {
        EXPECT_TRUE(child.Observed.isApprox(child.GetWorldMatrix()));
        EXPECT_TRUE(grandchild.Observed.isApprox(grandchild.GetWorldMatrix()));
    };
    root.SetRelativeLocation({7, 2, 3});
    check();
    root.SetRelativeRotation(Eigen::Quaternionf{Eigen::AngleAxisf{0.6f, Eigen::Vector3f::UnitY()}});
    check();
    root.SetRelativeScale({2, 3, 4});
    check();
    root.SetWorldLocation({-3, 2, 1});
    check();
    root.SetWorldRotation(Eigen::Quaternionf{Eigen::AngleAxisf{0.2f, Eigen::Vector3f::UnitZ()}});
    check();
    auto replacement = make_unique<SceneComponent>();
    replacement->SetRelativeLocation({-5, 1, 2});
    child.AttachTo(replacement.get());
    check();
    child.DetachFromParent();
    check();
    child.AttachTo(replacement.get());
    replacement.reset();
    EXPECT_FALSE(child.GetAttachParent());
    check();
}

TEST(SceneTransformTest, AttachingAnAncestorBelowItsDescendantIsRejected) {
    SceneComponent root, child, grandchild;
    child.AttachTo(&root);
    grandchild.AttachTo(&child);
    root.AttachTo(&grandchild);
    EXPECT_FALSE(root.GetAttachParent());
    EXPECT_EQ(child.GetAttachParent().Get(), &root);
    EXPECT_EQ(grandchild.GetAttachParent().Get(), &child);
}

enum class SceneScenario { ParentLight,
                           LoadingMesh,
                           MotionHistory,
                           DrainUploads };
class SceneStateApp final : public Application {
public:
    explicit SceneStateApp(SceneScenario scenario) : _scenario(scenario) {}
    bool Checked{false};

protected:
    void OnInit() override {
        if (_scenario == SceneScenario::ParentLight) {
            auto* actor = GetWorld()->SpawnActor<Actor>();
            auto* parent = actor->AddComponent<SceneComponent>();
            auto* light = actor->AddComponent<PointLightComponent>();
            light->AttachTo(parent);
            parent->SetRelativeLocation({7, 0, 0});
            LightRenderParameters params;
            light->GetSceneProxy()->GetLightRenderParameters(params);
            EXPECT_TRUE(params.WorldPosition.isApprox(light->GetWorldLocation()));
            Checked = true;
            test::CloseMainWindow(*this);
            return;
        }
        _mesh = GetAssetManager()->Load<StaticMesh>({test::kUploadTestId,
                                                     LoadStaticMesh(GetGpuSystem()->GetFrameUploadScheduler(), test::MakeUploadTestMesh()), "scene state mesh"});
        if (_scenario == SceneScenario::DrainUploads) {
            GetGpuSystem()->BeginFrameRecord(0, {}, {}, false);
            GetGpuSystem()->EndFrameRecordAndSubmit(0);
            GetGpuSystem()->WaitAndCleanupCompletedFlights();
            GetAssetManager()->Pump();
            EXPECT_TRUE(_mesh.IsReady());
            Checked = true;
            test::CloseMainWindow(*this);
            return;
        }
        _component = GetWorld()->SpawnActor<Actor>()->AddComponent<StaticMeshComponent>();
        _component->SetStaticMesh(_mesh);
        EXPECT_EQ(_component->GetSceneProxy(), nullptr);
    }
    void OnUpdate(const AppUpdateContext&) override {
        if ((_scenario != SceneScenario::LoadingMesh && _scenario != SceneScenario::MotionHistory) || Checked) return;
        if (_mesh.IsReady()) {
            GetWorld()->Tick(0);
            EXPECT_TRUE(_component->ShouldCreateRenderState());
            EXPECT_NE(_component->GetSceneProxy(), nullptr);
            auto* proxy = _component->GetSceneProxy();
            GetWorld()->Tick(0);
            EXPECT_EQ(_component->GetSceneProxy(), proxy);
            if (_scenario == SceneScenario::MotionHistory) CheckMotionHistory();
            _component->SetStaticMesh({});
            EXPECT_EQ(_component->GetSceneProxy(), nullptr);
            Checked = true;
            test::CloseMainWindow(*this);
        } else if (++_updates > 50) {
            ADD_FAILURE() << "Mesh did not become ready";
            test::CloseMainWindow(*this);
        }
    }
    void OnShutdown() override { _mesh.Reset(); }

private:
    void CheckMotionHistory() {
        auto* scene = GetWorld()->GetScene();
        auto* proxy = _component->GetSceneProxy();
        ASSERT_NE(proxy, nullptr);
        const auto generation = proxy->GetGeneration();
        RenderSceneSnapshot snapshot;
        vector<StreamingAssetRefAny> retained;
        PrimitiveHistory history;
        const auto snapshotNow = [&] {
            retained.clear();
            EXPECT_TRUE(BuildRenderSceneSnapshot(*scene, snapshot, retained));
        };
        snapshotNow();
        const Eigen::Matrix4f original = snapshot.Primitives.front().LocalToWorld;
        ASSERT_TRUE(history.Prepare(snapshot, 1));
        ASSERT_TRUE(history.Commit(1));
        _component->SetRelativeLocation({2, 3, 4});
        snapshotNow();
        EXPECT_EQ(_component->GetSceneProxy(), proxy);
        EXPECT_EQ(snapshot.Primitives.front().Generation, generation);
        auto motion = history.Lookup(snapshot.Primitives.front());
        EXPECT_TRUE(motion.Valid);
        EXPECT_TRUE(motion.PreviousLocalToWorld.isApprox(original));

        SceneComponent parent;
        _component->AttachTo(&parent);
        parent.SetRelativeLocation({7, 0, 0});
        _component->SetRelativeRotation(Eigen::Quaternionf{Eigen::AngleAxisf{0.5f, Eigen::Vector3f::UnitY()}});
        _component->SetRelativeScale({2, 3, 4});
        snapshotNow();
        EXPECT_EQ(snapshot.Primitives.front().Generation, generation);
        EXPECT_TRUE(snapshot.Primitives.front().LocalToWorld.isApprox(_component->GetWorldMatrix()));
        EXPECT_TRUE(history.Lookup(snapshot.Primitives.front()).Valid);
        EXPECT_TRUE(history.Lookup(snapshot.Primitives.front()).PreviousLocalToWorld.isApprox(original));
        EXPECT_TRUE(snapshot.Primitives.front().WorldBounds.Min.isApprox(
            TransformBounds(proxy->GetLocalBounds(), _component->GetWorldMatrix()).Min));
        proxy->ResetMotion();
        snapshotNow();
        EXPECT_EQ(snapshot.Primitives.front().Generation, generation);
        EXPECT_FALSE(history.Lookup(snapshot.Primitives.front()).Valid);
        ASSERT_TRUE(history.Prepare(snapshot, 2));
        ASSERT_TRUE(history.Commit(2));
        _component->MarkRenderStateDirty();
        snapshotNow();
        EXPECT_NE(snapshot.Primitives.front().Generation, generation);
        EXPECT_FALSE(history.Lookup(snapshot.Primitives.front()).Valid);

        vector<StaticMeshComponent*> components;
        vector<uint64_t> generations;
        auto* actor = GetWorld()->SpawnActor<Actor>();
        for (uint32_t i = 0; i < 1000; ++i) {
            auto* component = actor->AddComponent<StaticMeshComponent>();
            component->SetStaticMesh(_mesh);
            components.push_back(component);
            generations.push_back(component->GetSceneProxy()->GetGeneration());
        }
        const size_t registrations = scene->Primitives().size();
        PrimitiveSceneProxy before;
        for (uint32_t i = 0; i < components.size(); ++i) {
            components[i]->SetRelativeLocation({float(i), 2, 3});
            components[i]->SetRelativeScale({2, 2, 2});
            EXPECT_EQ(components[i]->GetSceneProxy()->GetGeneration(), generations[i]);
            EXPECT_TRUE(components[i]->GetSceneProxy()->GetLocalToWorld().isApprox(components[i]->GetWorldMatrix()));
        }
        PrimitiveSceneProxy after;
        EXPECT_EQ(after.GetGeneration(), before.GetGeneration() + 1);
        EXPECT_EQ(scene->Primitives().size(), registrations);
    }

    SceneScenario _scenario;
    uint32_t _updates{0};
    StreamingAssetRef<StaticMesh> _mesh;
    Nullable<StaticMeshComponent*> _component{nullptr};
};

class SceneRenderStateTest : public testing::TestWithParam<render::RenderBackend> {
protected:
    void Run(SceneScenario scenario) {
        {
            render::test::DeviceContext device;
            if (!render::test::TryCreateDevice(GetParam(), device)) GTEST_SKIP() << "Backend unavailable";
        }
        test::RuntimeLogCapture logs;
        SceneStateApp app(scenario);
        EXPECT_EQ(app.Run({.Backend = GetParam(), .EnableValidation = true, .Multithreaded = scenario == SceneScenario::LoadingMesh, .WindowTitle = "Scene state test", .WindowWidth = 96, .WindowHeight = 96, .BackBufferFormat = render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::FIFO}), 0);
        EXPECT_TRUE(app.Checked);
        EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
    }
};
TEST_P(SceneRenderStateTest, ParentMovementRefreshesLightProxy) { Run(SceneScenario::ParentLight); }
TEST_P(SceneRenderStateTest, MeshAssignedWhileLoadingCreatesProxyWhenReady) { Run(SceneScenario::LoadingMesh); }
TEST_P(SceneRenderStateTest, RealStaticMeshTransformsPreserveTemporalIdentity) { Run(SceneScenario::MotionHistory); }
TEST_P(SceneRenderStateTest, WaitAndCleanupDrainsPendingUploads) { Run(SceneScenario::DrainUploads); }
INSTANTIATE_TEST_SUITE_P(Backends, SceneRenderStateTest, testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));

}  // namespace
}  // namespace radray
