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
        if (_scenario != SceneScenario::LoadingMesh || Checked) return;
        if (_mesh.IsReady()) {
            GetWorld()->Tick(0);
            EXPECT_TRUE(_component->ShouldCreateRenderState());
            EXPECT_NE(_component->GetSceneProxy(), nullptr);
            auto* proxy = _component->GetSceneProxy();
            GetWorld()->Tick(0);
            EXPECT_EQ(_component->GetSceneProxy(), proxy);
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
TEST_P(SceneRenderStateTest, WaitAndCleanupDrainsPendingUploads) { Run(SceneScenario::DrainUploads); }
INSTANTIATE_TEST_SUITE_P(Backends, SceneRenderStateTest, testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));

}  // namespace
}  // namespace radray
