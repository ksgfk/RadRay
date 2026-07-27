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

namespace render {
class Dxc;
}  // namespace render

/// runtime 侧的渲染协调器。职责边界:
/// - 拥有"怎么画"的一切:RenderPipeline、Scene 列表、shader JIT 工具链(DXC / include root)
///   以及按描述符复用的 RenderPass / Framebuffer 缓存(RenderPassRegistry)。
/// - 每帧把 GpuSystem 交出的 AppFrameContext 翻成一组 RenderPipelineTarget,
///   驱动 pipeline 录制,并负责 backbuffer 的 RenderTarget/Present 状态收尾。
/// - 不拥有 device / queue / flight / uploader / 延迟销毁 —— 那些属于 GpuSystem;
///   本类只通过 Application 借用它们,所有 GPU 对象的生命周期仍由 GpuSystem 兜底。
class RenderSystem {
public:
    explicit RenderSystem(Application* app) noexcept;
    RenderSystem(const RenderSystem&) = delete;
    RenderSystem(RenderSystem&&) = delete;
    RenderSystem& operator=(const RenderSystem&) = delete;
    RenderSystem& operator=(RenderSystem&&) = delete;
    ~RenderSystem() noexcept;

    /// 装配阶段调用。创建 RenderPassRegistry 与 runtime shader/PSO libraries；
    /// 启用 JIT 时同时创建 DXC。
    void OnInitialize();

    void Render(AppFrameContext& ctx);

    Scene* AllocateScene();
    void ReleaseScene(Scene* scene) noexcept;
    void ReleaseAllScenes() noexcept;

    Application* GetApplication() const noexcept { return _app; }
    RenderPipeline* GetPipeline() const noexcept { return _pipeline.get(); }
    /// RenderPass / Framebuffer 复用缓存。OnInitialize 之前或 device 缺失时为空。
    RenderPassRegistry* GetRenderPassRegistry() const noexcept { return _renderPassRegistry.get(); }
    /// JIT shader 编译根目录 (<exe>/shaderlib)；关闭 JIT 时为空。
    const string& GetShaderIncludeRoot() const noexcept { return _shaderIncludeRoot; }
    /// JIT 编译器。关闭 JIT 或 DXC 创建失败时为空, 此时只能走 AOT 产物。
    const shared_ptr<render::Dxc>& GetDxc() const noexcept { return _dxc; }

private:
    void EnsureRenderTargetState(AppFrameContext& ctx, RenderPipelineTarget& target);
    void EnsurePresentState(AppFrameContext& ctx, RenderPipelineTarget& target);

    Application* _app{nullptr};
    unique_ptr<RenderPassRegistry> _renderPassRegistry;
    unique_ptr<RenderPipeline> _pipeline;
    vector<unique_ptr<Scene>> _scenes;
    shared_ptr<render::Dxc> _dxc;
    string _shaderIncludeRoot;
};

template <>
struct RuntimeTypeTrait<RenderSystem> {
    static constexpr RuntimeTypeId value{0x241d4e78, 0x8f4e, 0x4d1c, 0xa8, 0xb9, 0x55, 0x09, 0x61, 0x6a, 0x90, 0x24};
    using Bases = std::tuple<>;
};

}  // namespace radray
