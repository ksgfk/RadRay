#pragma once

#include <radray/runtime_type.h>
#include <radray/runtime/gpu_resource.h>
#include <radray/runtime/render_framework/render_pipeline.h>
#include <radray/runtime/render_framework/scene.h>
#include <radray/types.h>

namespace radray {

class Application;
class AppFrameContext;
struct AppFrameTarget;
class ShaderArtifactResolver;

namespace shader {
class Dxc;
}  // namespace shader

/// Runtime-side renderer coordinator.
///
/// This fills the role that UE's RendererModule plays for scene ownership in a
/// single-module runtime: it creates, tracks, and releases render scenes.
class RenderSystem {
public:
    explicit RenderSystem(Application* app) noexcept;
    RenderSystem(const RenderSystem&) = delete;
    RenderSystem(RenderSystem&&) = delete;
    RenderSystem& operator=(const RenderSystem&) = delete;
    RenderSystem& operator=(RenderSystem&&) = delete;
    ~RenderSystem() noexcept;

    /// 装配阶段调用。创建 runtime shader/PSO libraries；启用 JIT 时同时创建 DXC。
    void OnInitialize();

    void Render(AppFrameContext& ctx);

    Scene* AllocateScene();
    void ReleaseScene(Scene* scene) noexcept;
    void ReleaseAllScenes() noexcept;

    Application* GetApplication() const noexcept { return _app; }
    RenderPipeline* GetPipeline() const noexcept { return _pipeline.get(); }
    /// 当前管线的标准材质工厂 (把中性材质描述翻译成本管线材质)。无管线/不支持时返回 null。
    Nullable<IStandardMaterialFactory*> GetStandardMaterialFactory() noexcept;
    SamplerCache* GetSamplerCache() const noexcept { return _samplerCache.get(); }
    /// JIT shader 编译根目录 (<exe>/shaderlib)；关闭 JIT 时为空。
    const string& GetShaderIncludeRoot() const noexcept { return _shaderIncludeRoot; }
    ShaderArtifactResolver* GetShaderArtifactResolver() const noexcept { return _shaderResolver.get(); }

private:
    void EnsureRenderTargetState(AppFrameContext& ctx, RenderPipelineTarget& target);
    void EnsurePresentState(AppFrameContext& ctx, RenderPipelineTarget& target);

    Application* _app{nullptr};
    unique_ptr<RenderPipeline> _pipeline;
    vector<unique_ptr<Scene>> _scenes;
    shared_ptr<render::Dxc> _dxc;
    unique_ptr<ShaderArtifactResolver> _shaderResolver;
    unique_ptr<SamplerCache> _samplerCache;
    string _shaderIncludeRoot;
};

template <>
struct RuntimeTypeTrait<RenderSystem> {
    static constexpr RuntimeTypeId value{0x241d4e78, 0x8f4e, 0x4d1c, 0xa8, 0xb9, 0x55, 0x09, 0x61, 0x6a, 0x90, 0x24};
    using Bases = std::tuple<>;
};

}  // namespace radray
