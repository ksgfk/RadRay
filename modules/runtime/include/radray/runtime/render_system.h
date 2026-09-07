#pragma once

#include <span>
#include <atomic>
#include <thread>
#include <string_view>

#include <radray/nullable.h>
#include <radray/render/backend/pipeline_layout_types.h>
#include <radray/runtime_type.h>
#include <radray/runtime/gpu_resource.h>
#include <radray/render/render_pass_registry.h>
#include <radray/runtime/render_framework/render_pipeline.h>
#include <radray/runtime/render_framework/render_graph_runtime.h>
#include <radray/runtime/render_framework/scene.h>
#include <radray/runtime/shader_program_request.h>
#include <radray/runtime/service_traits.h>
#include <radray/shader/shader_compiler_contract.h>
#include <radray/types.h>

namespace radray {

class Application;
class GpuSystem;
class AppFrameContext;
class ShaderProgram;
class ShaderProgramCache;
class PresentationAdapter;
struct AppFrameTarget;

/// runtime 侧的渲染协调器。【拥有"怎么画", 不拥有帧时序】—— device / queue / flight /
/// uploader / 延迟销毁都属 GpuSystem, 本类只借用。
/// 职责划分见 docs/architecture/render-framework.md。
class RenderSystem {
public:
    explicit RenderSystem(Application* app) noexcept;
    RenderSystem(const RenderSystem&) = delete;
    RenderSystem(RenderSystem&&) = delete;
    RenderSystem& operator=(const RenderSystem&) = delete;
    RenderSystem& operator=(RenderSystem&&) = delete;
    ~RenderSystem() noexcept;

    /// 装配阶段调用并创建 RenderPassRegistry。
    [[nodiscard]] ServiceStatus OnInitialize();
    /// Requires GPU idle and released World/scene users; also accepts partial initialization.
    void OnShutdown() noexcept;
    void SetGpuSystem(Nullable<GpuSystem*> gpu) noexcept { _gpuSystem = gpu; }

    /// Game thread. First installation after loading drains outstanding fallback frames through the runner.
    /// An installed pipeline can only be replaced before the first PrepareFrame/Render or at GPU-idle OnShutdown.
    /// Other replacements fail without releasing the installed pipeline.
    bool SetPipeline(unique_ptr<RenderPipeline> pipeline) noexcept;

    /// Game thread; the runner has made this flight writable after GPU completion.
    void BeginUpdateForFlight(uint32_t flightIndex);
    void PrepareFrame(const AppUpdateContext& ctx);
    void Render(AppFrameContext& ctx);

    Scene* AllocateScene();
    void ReleaseScene(Scene* scene) noexcept;
    void ReleaseAllScenes() noexcept;

    RenderPipeline* GetPipeline() const noexcept { return _pipeline.get(); }
    /// RenderPass / Framebuffer 复用缓存。OnInitialize 之前或 device 缺失时为空。
    render::RenderPassRegistry* GetRenderPassRegistry() const noexcept { return _renderPassRegistry.get(); }
    RenderOutputRegistry& GetOutputs() noexcept { return _outputs; }
    const RenderGraphExecutionReport& GetGraphReport(uint32_t flight) const { return _graphReports[flight]; }
    const RenderFramePlan& GetFramePlan(uint32_t flight) const { return _framePlans[flight]; }
    const RenderResourcePoolStats& GetPoolStats(uint32_t flight) const { return _graphRuntime->GetPoolStats(flight); }
    /// Render thread or global render idle; history is shared across flights.
    ViewStateStats GetViewStateStats() const { return _viewStates ? _viewStates->GetStats() : ViewStateStats{}; }

    Nullable<ShaderProgram*> GetOrCreateShaderProgram(const ShaderProgramRequest& request);
    Nullable<ShaderProgram*> GetOrCreateShaderProgram(std::span<const byte> artifact,
                                                      const shader::GpuArtifactHash& expectedIdentity,
                                                      const render::ShaderProgramLayoutRecipe& recipe = {});

    size_t GetShaderProgramCacheSize() const noexcept;

    /// 编译产物缓存条目数。与 program 数不同: 同一个 artifact 可以服务多个 layout recipe。
    size_t GetShaderArtifactCacheSize() const noexcept;
    /// Explicit source revision; old programs stay owned until shutdown.
    bool InvalidateShaderSource(std::string_view sourceName);

private:
    friend class Application;
    friend class ImGuiSystem;
    void TransitionSurface(AppFrameContext& ctx, RenderSurfaceFrame& target, render::TextureStates state);
    void ClearTarget(AppFrameContext& ctx, RenderSurfaceFrame& target);

    Application* _app{nullptr};
    Nullable<GpuSystem*> _gpuSystem{nullptr};
    const std::thread::id _gameThread{std::this_thread::get_id()};
    unique_ptr<render::RenderPassRegistry> _renderPassRegistry;
    RenderOutputRegistry _outputs;
    vector<RenderFramePlan> _framePlans;
    vector<vector<RenderOutputInfo>> _frameOutputInfos;
    vector<RenderGraphExecutionReport> _graphReports;
    unique_ptr<RenderGraphRuntime> _graphRuntime;
    unique_ptr<ViewStateRegistry> _viewStates;
    uint64_t _frameSerial{0};
    unique_ptr<ShaderProgramCache> _shaderCache;
    unique_ptr<PresentationAdapter> _presentation;
    unique_ptr<RenderPipeline> _pipeline;
    std::atomic_bool _pipelineStarted{false};
    bool _pipelineShutdownIdle{false};
    vector<unique_ptr<Scene>> _scenes;
    // Only the game thread touches these refs; shutdown releases them after GPU idle.
    vector<vector<StreamingAssetRefAny>> _retainedAssets;
};

template <>
struct ServiceTraits<RenderSystem> {
    static constexpr std::string_view Name{"RenderSystem"};
    using Dependencies = TypeList<Required<GpuSystem>>;
    static void Inject(RenderSystem& self, GpuSystem& gpu) noexcept { self.SetGpuSystem(&gpu); }
    static ServiceStatus Initialize(RenderSystem& self) { return self.OnInitialize(); }
    static void Shutdown(RenderSystem& self) noexcept { self.OnShutdown(); }
    static void Unwire(RenderSystem& self) noexcept { self.SetGpuSystem(nullptr); }
};

template <>
struct RuntimeTypeTrait<RenderSystem> {
    static constexpr RuntimeTypeId value{0x241d4e78, 0x8f4e, 0x4d1c, 0xa8, 0xb9, 0x55, 0x09, 0x61, 0x6a, 0x90, 0x24};
};

}  // namespace radray
