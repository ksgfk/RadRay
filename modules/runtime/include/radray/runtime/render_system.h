#pragma once

#include <string>

#include <radray/runtime_type.h>
#include <radray/runtime/render_framework/render_pipeline.h>
#include <radray/runtime/render_framework/scene.h>
#include <radray/types.h>

namespace radray {

class Application;
class AppFrameContext;
struct AppFrameTarget;

namespace render {
class Dxc;
class ShaderVariantCache;
class GraphicsPipelineStateCache;
}  // namespace render

class SamplerCache;

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

    /// 装配阶段调用 (ServiceRegistry::Initialize)。创建 shader 编译设施:
    /// Dxc + ShaderVariantCache + GraphicsPipelineStateCache,并把 <exe>/shaderlib
    /// 作为默认 include 根。同时实例化默认渲染管线 (ForwardPipeline)。
    void OnInitialize();

    void Render(AppFrameContext& ctx);

    Scene* AllocateScene();
    void ReleaseScene(Scene* scene) noexcept;
    void ReleaseAllScenes() noexcept;

    Application* GetApplication() const noexcept { return _app; }
    render::ShaderVariantCache* GetShaderVariantCache() const noexcept { return _variantCache.get(); }
    render::GraphicsPipelineStateCache* GetGraphicsPipelineStateCache() const noexcept { return _psoCache.get(); }
    SamplerCache* GetSamplerCache() const noexcept { return _samplerCache.get(); }
    /// shader 编译默认 include 根目录 (<exe>/shaderlib)。供构建 ShaderPassDesc::IncludeDirs。
    const string& GetShaderIncludeRoot() const noexcept { return _shaderIncludeRoot; }

private:
    void EnsurePresentState(AppFrameContext& ctx, RenderPipelineTarget& target);

    Application* _app{nullptr};
    unique_ptr<RenderPipeline> _pipeline;
    vector<unique_ptr<Scene>> _scenes;
    shared_ptr<render::Dxc> _dxc;
    unique_ptr<render::ShaderVariantCache> _variantCache;
    unique_ptr<render::GraphicsPipelineStateCache> _psoCache;
    unique_ptr<SamplerCache> _samplerCache;
    string _shaderIncludeRoot;
};

template <>
struct RuntimeTypeTrait<RenderSystem> {
    static constexpr RuntimeTypeId value{0x241d4e78, 0x8f4e, 0x4d1c, 0xa8, 0xb9, 0x55, 0x09, 0x61, 0x6a, 0x90, 0x24};
};

}  // namespace radray
