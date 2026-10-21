#include <radray/runtime/render_framework/renderer_scene.h>

#include <algorithm>
#include <radray/runtime/render_framework/renderer.h>

namespace radray {
RendererScenes::RendererScenes(Renderer& renderer, uint32_t flights) : _renderer(renderer), _flights(flights) {}
RendererScenes::~RendererScenes() noexcept = default;
void RendererScenes::AddRoot(Asset& asset, ResourceId root) {
    if (std::find(asset.Roots.begin(), asset.Roots.end(), root) != asset.Roots.end()) return;
    asset.Roots.push_back(root);
    ++_rootUsers[root];
}
RendererScenes::Exit RendererScenes::Release(RendererSceneKey scene, uint64_t token, Asset& asset) {
    Exit result{scene, token, {}, {}};
    if (asset.Material) {
        result.Material = asset.Material->Material;
        _renderer.UnregisterMaterial(*result.Material);
        for (const auto id : asset.Material->Templates) _renderer.UnregisterTemplate(id);
    }
    for (const auto root : asset.Roots) {
        auto found = _rootUsers.find(root);
        if (--found->second == 0) {
            _rootUsers.erase(found);
            _renderer.RetireResource(root);
            result.RetiringRoots.push_back(root);
        }
    }
    asset = {};
    return result;
}
void RendererScenes::Apply(RendererSceneKey key, const SceneApplyChanges& changes, std::span<const AssetExitRequest> exits, bool destroy) {
    auto& scene = _scenes[key];
    if (scene.Gpu) scene.Gpu->ApplyChanges(changes);
    for (const auto& request : exits) {
        const auto asset = scene.Assets.find(request.Asset);
        if (asset == scene.Assets.end())
            _exits.push_back({key, request.Token, {}, {}});
        else {
            _exits.push_back(Release(key, request.Token, asset->second));
            scene.Assets.erase(asset);
        }
    }
    if (destroy) {
        for (auto& [id, asset] : scene.Assets) _exits.push_back(Release(key, 0, asset));
        _scenes.erase(key);
    }
    PollExits();
}
std::optional<SceneGpuView> RendererScenes::Prepare(RendererSceneKey key, const RenderScene& scene, AppFrameContext& frame, FrameBuilder& builder) {
    auto& entry = _scenes[key];
    if (!entry.Gpu) entry.Gpu = make_unique<SceneGpuData>(_renderer, _flights);
    return entry.Gpu->Prepare(scene, frame, builder);
}
std::optional<SceneGpuView> RendererScenes::Prepare(RendererSceneKey key, const RenderScene& scene, PipelineContext& context) {
    auto& entry = _scenes[key];
    if (!entry.Gpu) entry.Gpu = make_unique<SceneGpuData>(_renderer, _flights);
    return entry.Gpu->Prepare(scene, context);
}
Nullable<const SceneMaterialRegistration*> RendererScenes::Material(RendererSceneKey key, AssetId id, const MaterialRenderData& data) {
    auto& asset = _scenes[key].Assets[id];
    if (asset.Material) return &*asset.Material;
    SceneMaterialRegistration registration;
    for (const auto& definition : data.Passes) {
        const auto compiled = _renderer.RegisterTemplate(definition);
        if (!compiled) {
            for (const auto previous : registration.Templates) _renderer.UnregisterTemplate(previous);
            return nullptr;
        }
        registration.Templates.push_back(*compiled);
    }
    RendererMaterialDescriptor material{registration.Templates, data.Constants, {}, data.Samplers};
    bool valid = true;
    for (const auto* texture : data.Textures) {
        const auto desc = texture->GetDesc();
        const auto root = _renderer.ImportTexture(desc.Target);
        if (!root) {
            valid = false;
            break;
        }
        AddRoot(asset, *root);
        const auto view = _renderer.CreateTextureView(*root, desc);
        if (!view) {
            valid = false;
            break;
        }
        material.Textures.push_back(*view);
    }
    const auto registered = valid ? _renderer.RegisterMaterial(material) : std::nullopt;
    if (!registered) {
        for (const auto previous : registration.Templates) _renderer.UnregisterTemplate(previous);
        return nullptr;
    }
    registration.Material = *registered;
    asset.Material = std::move(registration);
    return &*asset.Material;
}
std::optional<RendererDrawDescriptor> RendererScenes::Geometry(RendererSceneKey key, AssetId id, const GpuMesh::DrawData& geometry) {
    auto& asset = _scenes[key].Assets[id];
    const auto found = asset.Geometry.find(&geometry);
    if (found != asset.Geometry.end()) return found->second;
    RendererDrawDescriptor draw;
    draw.Geometry = geometry.Layout;
    draw.Topology = geometry.Topology;
    for (const auto& binding : geometry.VertexBuffers) {
        const auto root = _renderer.ImportBuffer(binding.View.Target);
        if (!root) return std::nullopt;
        AddRoot(asset, *root);
        draw.Vertices.push_back({binding.Binding, *root, binding.View.Offset, binding.View.Size});
    }
    if (geometry.Ibv.Target) {
        const auto root = _renderer.ImportBuffer(geometry.Ibv.Target);
        if (!root) return std::nullopt;
        AddRoot(asset, *root);
        draw.Indices = {*root, geometry.Ibv.Offset, geometry.Ibv.Stride};
    }
    asset.Geometry.emplace(&geometry, draw);
    return draw;
}
void RendererScenes::PollExits() {
    std::erase_if(_exits, [&](const auto& exit) {
        if (exit.Material && !_renderer.IsMaterialRetired(*exit.Material)) return false;
        for (const auto root : exit.RetiringRoots)
            if (!_renderer.IsResourceRetired(root)) return false;
        if (exit.Token) {
            std::lock_guard lock{_confirmationMutex};
            _confirmations[exit.Scene.Delivery].push_back({exit.Scene.Scene, exit.Token});
        }
        return true;
    });
}
vector<SceneAssetExitConfirmation> RendererScenes::TakeConfirmations(Guid delivery) {
    std::lock_guard lock{_confirmationMutex};
    const auto found = _confirmations.find(delivery);
    if (found == _confirmations.end()) return {};
    auto result = std::move(found->second);
    _confirmations.erase(found);
    return result;
}
void RendererScenes::Clear() {
    for (auto& [key, scene] : _scenes)
        for (auto& [id, asset] : scene.Assets) _exits.push_back(Release(key, 0, asset));
    _scenes.clear();
    PollExits();
}
}  // namespace radray
