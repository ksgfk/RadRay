#pragma once

#include <mutex>
#include <radray/runtime/render_framework/scene_gpu.h>

namespace radray {
class Renderer;
struct RendererSceneKey {
    Guid Delivery;
    SceneId Scene;
    auto operator<=>(const RendererSceneKey&) const = default;
};
struct SceneAssetExitConfirmation {
    SceneId Scene;
    uint64_t Token;
};
struct SceneMaterialRegistration {
    MaterialId Material;
    vector<TemplateId> Templates;
};

/// Renderer-owned scene mirrors and asset imports. Asset owners remain on GT until explicit confirmation.
class RendererScenes {
public:
    RendererScenes(Renderer& renderer, uint32_t flights);
    ~RendererScenes() noexcept;
    void Apply(RendererSceneKey key, const SceneApplyChanges& changes, std::span<const AssetExitRequest> exits, bool destroy);
    std::optional<SceneGpuView> Prepare(RendererSceneKey key, const RenderScene& scene, AppFrameContext& frame, FrameBuilder& builder);
    std::optional<SceneGpuView> Prepare(RendererSceneKey key, const RenderScene& scene, PipelineContext& context);
    Nullable<const SceneMaterialRegistration*> Material(RendererSceneKey key, AssetId asset, const MaterialRenderData& data);
    std::optional<RendererDrawDescriptor> Geometry(RendererSceneKey key, AssetId asset, const GpuMesh::DrawData& geometry);
    void PollExits();
    vector<SceneAssetExitConfirmation> TakeConfirmations(Guid delivery);
    void Clear();

private:
    struct Asset {
        std::optional<SceneMaterialRegistration> Material;
        vector<ResourceId> Roots;
        unordered_map<const GpuMesh::DrawData*, RendererDrawDescriptor> Geometry;
    };
    struct Scene {
        unique_ptr<SceneGpuData> Gpu;
        unordered_map<AssetId, Asset> Assets;
    };
    struct Exit {
        RendererSceneKey Scene;
        uint64_t Token;
        std::optional<MaterialId> Material;
        vector<ResourceId> RetiringRoots;
    };
    void AddRoot(Asset& asset, ResourceId root);
    Exit Release(RendererSceneKey scene, uint64_t token, Asset& asset);
    Renderer& _renderer;
    uint32_t _flights;
    map<RendererSceneKey, Scene> _scenes;
    unordered_map<ResourceId, size_t, RendererHandleHash<RendererResourceTag>> _rootUsers;
    vector<Exit> _exits;
    std::mutex _confirmationMutex;
    unordered_map<Guid, vector<SceneAssetExitConfirmation>> _confirmations;
};
}  // namespace radray
