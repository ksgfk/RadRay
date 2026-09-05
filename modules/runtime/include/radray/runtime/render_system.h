#pragma once

#include <span>
#include <string_view>

#include <radray/nullable.h>
#include <radray/render/backend/pipeline_layout_types.h>
#include <radray/runtime_type.h>
#include <radray/runtime/gpu_resource.h>
#include <radray/render/render_pass_registry.h>
#include <radray/runtime/render_framework/render_pipeline.h>
#include <radray/runtime/render_framework/render_graph_runtime.h>
#include <radray/runtime/render_framework/scene.h>
#include <radray/runtime/shader_jit.h>
#include <radray/runtime/service_traits.h>
#include <radray/shader/shader_compiler_contract.h>
#include <radray/types.h>

namespace radray {

class Application;
class GpuSystem;
class AppFrameContext;
class ShaderProgram;
struct AppFrameTarget;

/// 一个 shader program 请求。它显式拥有决定身份的全部输入: 逻辑源名、结构化 defines、keyword
/// assignments、完整 compile policy 与按 target 分开的 layout recipe。discovery 与 compile 都由同一个
/// 请求驱动, 因此两者不会在不同 policy 下看到不同的 contract。
struct ShaderProgramRequest {
    string SourceName;
    vector<shader::Define> Defines{};
    vector<shader::KeywordAssignment> Assignments{};
    shader::CompilePolicy Policy{};
    /// 只影响 program/layout 身份, 不参与 compiler artifact 身份: 换掉非当前 backend 的 recipe 既不会
    /// 重新编译, 也不会新建 program。
    render::ShaderProgramLayoutRecipe LayoutRecipe{};
};

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

    void SetPipeline(unique_ptr<RenderPipeline> pipeline) noexcept;

    /// Game thread; the runner has made this flight writable after GPU completion.
    void BeginUpdateForFlight(uint32_t flightIndex);
    void PrepareFrame(const AppUpdateContext& ctx);
    void Render(AppFrameContext& ctx);

    Scene* AllocateScene();
    void ReleaseScene(Scene* scene) noexcept;
    void ReleaseAllScenes() noexcept;

    Application* GetApplication() const noexcept { return _app; }
    RenderPipeline* GetPipeline() const noexcept { return _pipeline.get(); }
    /// RenderPass / Framebuffer 复用缓存。OnInitialize 之前或 device 缺失时为空。
    render::RenderPassRegistry* GetRenderPassRegistry() const noexcept { return _renderPassRegistry.get(); }
    RenderOutputRegistry& GetOutputs() noexcept { return _outputs; }
    const RenderGraphExecutionReport& GetGraphReport(uint32_t flight) const { return _graphReports[flight]; }
    const RenderFramePlan& GetFramePlan(uint32_t flight) const { return _framePlans[flight]; }
    const RenderResourcePoolStats& GetPoolStats(uint32_t flight) const { return _graphRuntime->GetPoolStats(flight); }

    Nullable<ShaderProgram*> GetOrCreateShaderProgram(const ShaderProgramRequest& request);

    size_t GetShaderProgramCacheSize() const noexcept { return _shaderPrograms.size(); }

    /// 编译产物缓存条目数。与 program 数不同: 同一个 artifact 可以服务多个 layout recipe。
    size_t GetShaderArtifactCacheSize() const noexcept { return _shaderArtifacts.size(); }

private:
    struct ProgramText {
        string Name;
        string Value;

        friend bool operator==(const ProgramText&, const ProgramText&) = default;
    };

    /// compiler artifact 身份: source、结构化 defines、canonical assignments、完整 policy、target 与
    /// toolchain。layout recipe 不在其中, 因为它不改变编译产物。
    struct ArtifactKey {
        string SourceName;
        vector<ProgramText> Defines{};
        vector<ProgramText> Assignments{};
        shader::CompilePolicy Policy{};
        shader::ShaderTarget Target{shader::ShaderTarget::DXIL};
        shader::Hash128 Toolchain{};

        friend bool operator==(const ArtifactKey&, const ArtifactKey&) = default;
    };

    struct ArtifactKeyHash {
        size_t operator()(const ArtifactKey& value) const noexcept;
    };

    /// 失败按完整 key 记成显式失败, 而不是留一个空 program: 空条目分不清"还没编译"和"编译失败",
    /// 会让一次失败永久污染这个 key。
    struct ArtifactRecord {
        bool Failed{false};
        uint64_t Identity{0};
        ShaderJitArtifact Artifact{};
    };

    /// program/layout 身份: artifact 身份 + 当前 backend 的 canonical resolved layout hash。
    struct ProgramKey {
        uint64_t ArtifactIdentity{0};
        render::ResolvedLayoutHash LayoutHash{};

        friend bool operator==(const ProgramKey&, const ProgramKey&) = default;
    };

    struct ProgramKeyHash {
        size_t operator()(const ProgramKey& value) const noexcept;
    };

    struct ProgramRecord {
        bool Failed{false};
        unique_ptr<ShaderProgram> Program{};
    };

    Nullable<const ArtifactRecord*> GetOrCompileArtifact(
        const ShaderProgramRequest& request,
        ArtifactKey key);

    void TransitionSurface(AppFrameContext& ctx, RenderSurfaceFrame& target, render::TextureStates state);
    void ClearTarget(AppFrameContext& ctx, RenderSurfaceFrame& target);

    Application* _app{nullptr};
    Nullable<GpuSystem*> _gpuSystem;
    unique_ptr<render::RenderPassRegistry> _renderPassRegistry;
    RenderOutputRegistry _outputs;
    vector<RenderFramePlan> _framePlans;
    vector<RenderGraphExecutionReport> _graphReports;
    unique_ptr<RenderGraphRuntime> _graphRuntime;
    unique_ptr<ViewStateRegistry> _viewStates;
    uint64_t _frameSerial{0};
    unique_ptr<ShaderJit> _shaderJit;
    unordered_map<ArtifactKey, ArtifactRecord, ArtifactKeyHash> _shaderArtifacts;
    unordered_map<ProgramKey, ProgramRecord, ProgramKeyHash> _shaderPrograms;
    uint64_t _nextArtifactIdentity{1};
    unique_ptr<RenderPipeline> _pipeline;
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
