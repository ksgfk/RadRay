#pragma once

#include <span>
#include <string_view>

#include <radray/nullable.h>
#include <radray/render/backend/pipeline_layout_types.h>
#include <radray/runtime_type.h>
#include <radray/runtime/gpu_resource.h>
#include <radray/render/render_pass_registry.h>
#include <radray/runtime/render_framework/render_pipeline.h>
#include <radray/runtime/render_framework/scene.h>
#include <radray/shader/shader_compiler_contract.h>
#include <radray/types.h>

namespace radray {

class Application;
class AppFrameContext;
class ShaderJit;
class ShaderProgram;
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
    void OnInitialize();

    void SetPipeline(unique_ptr<RenderPipeline> pipeline) noexcept;

    void Render(AppFrameContext& ctx);

    Scene* AllocateScene();
    void ReleaseScene(Scene* scene) noexcept;
    void ReleaseAllScenes() noexcept;

    Application* GetApplication() const noexcept { return _app; }
    RenderPipeline* GetPipeline() const noexcept { return _pipeline.get(); }
    /// RenderPass / Framebuffer 复用缓存。OnInitialize 之前或 device 缺失时为空。
    render::RenderPassRegistry* GetRenderPassRegistry() const noexcept { return _renderPassRegistry.get(); }

    Nullable<ShaderProgram*> GetOrCreateShaderProgram(
        std::string_view sourceName,
        std::span<const shader::KeywordAssignment> assignments = {},
        const render::ShaderLayoutPolicy& layoutPolicy = {},
        const shader::CompilePolicy& compilePolicy = {});
    size_t GetShaderProgramCacheSize() const noexcept { return _shaderPrograms.size(); }

private:
    struct ProgramAssignment {
        string Name;
        string Value;

        friend bool operator==(const ProgramAssignment&, const ProgramAssignment&) = default;
    };

    struct ProgramKey {
        string SourceName;
        vector<ProgramAssignment> Assignments;

        friend bool operator==(const ProgramKey&, const ProgramKey&) = default;
    };

    struct ProgramKeyHash {
        size_t operator()(const ProgramKey& value) const noexcept;
    };

    void EnsureRenderTargetState(AppFrameContext& ctx, RenderPipelineTarget& target);
    void EnsurePresentState(AppFrameContext& ctx, RenderPipelineTarget& target);

    Application* _app{nullptr};
    unique_ptr<render::RenderPassRegistry> _renderPassRegistry;
    unique_ptr<ShaderJit> _shaderJit;
    unordered_map<ProgramKey, unique_ptr<ShaderProgram>, ProgramKeyHash> _shaderPrograms;
    unique_ptr<RenderPipeline> _pipeline;
    vector<unique_ptr<Scene>> _scenes;
};

template <>
struct RuntimeTypeTrait<RenderSystem> {
    static constexpr RuntimeTypeId value{0x241d4e78, 0x8f4e, 0x4d1c, 0xa8, 0xb9, 0x55, 0x09, 0x61, 0x6a, 0x90, 0x24};
    using Bases = std::tuple<>;
};

}  // namespace radray
