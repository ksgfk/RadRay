#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

#include <radray/nullable.h>
#include <radray/render/common.h>
#include <radray/runtime/gpu_system.h>
#include <radray/types.h>

namespace radray {

class Application;
class AppFrameContext;
class CameraComponent;
class Scene;
class IStandardMaterialFactory;
struct MaterialRenderSnapshot;

/// 渲染队列排序值 (对应 Unity 的 Material.renderQueue)。
/// 数值越小越先绘制; >= Transparent 的走 back-to-front 半透明排序。
enum class RenderQueue : int32_t {
    Background = 1000,
    Geometry = 2000,
    AlphaTest = 2450,
    GeometryLast = 2500,  // 不透明与半透明的分界
    Transparent = 3000,
    Overlay = 4000,
};

/// 材质对 PSO 固定功能状态 (blend / zwrite / cull) 的覆盖 (对应 Unity ShaderLab 的
/// [_SrcBlend] [_DstBlend] / ZWrite [_ZWrite] / Cull [_Cull])。
///
/// 语义: 这些状态属 PSO 固定功能段, 不影响 shader 编译产物 (bytecode), 因此不该烘进
/// ShaderPassDesc / 变体, 而应由材质在 PSO 构建时覆盖。这样同一份 shader + 同一 keyword 表
/// 只需一个 ShaderAsset, opaque / transparent / 双面 等差异全部落在材质侧。
///
/// 三态覆盖: 各字段为 nullopt / OverrideBlend=false 时【沿用 ShaderPassDesc 的 pass 基线】,
/// 否则用给定值覆盖。Blend 用 (OverrideBlend + optional<BlendState>) 表达三态:
/// - OverrideBlend=false: 不覆盖, 沿用 pass 基线;
/// - OverrideBlend=true 且 Blend 有值: 覆盖为开启, 用给定 BlendState;
/// - OverrideBlend=true 且 Blend 为 nullopt: 覆盖为【强制关闭】混合 (opaque 显式关基线里的 blend)。
struct MaterialRenderState {
    std::optional<render::CullMode> Cull{};
    std::optional<bool> DepthWrite{};
    bool OverrideBlend{false};
    std::optional<render::BlendState> Blend{};

    friend bool operator==(const MaterialRenderState&, const MaterialRenderState&) = default;
};

/// SRP-style injection point for logical rendering passes.
enum class RenderPassEvent : int32_t {
    BeforeRendering = 0,
    BeforeRenderingShadows = 50,
    AfterRenderingShadows = 100,
    BeforeRenderingPrePasses = 150,
    AfterRenderingPrePasses = 200,
    BeforeRenderingOpaques = 250,
    AfterRenderingOpaques = 300,
    BeforeRenderingSkybox = 350,
    AfterRenderingSkybox = 400,
    BeforeRenderingTransparents = 450,
    AfterRenderingTransparents = 500,
    BeforeRenderingPostProcessing = 550,
    AfterRenderingPostProcessing = 600,
    AfterRendering = 1000,
};

struct RenderPipelineTarget {
    AppFrameTarget Target;
    render::TextureStates State{render::TextureState::Undefined};
    bool ContentDrawn{false};
};

/// Per-frame pipeline state passed through the high-level render pipeline.
struct RenderPipelineContext {
    RenderPipelineContext(Application* app, AppFrameContext& frame, std::span<RenderPipelineTarget> targets) noexcept;

    Application* App{nullptr};
    AppFrameContext& Frame;
    std::span<RenderPipelineTarget> Targets;
};

/// One camera render request. A camera list can contain cameras for different
/// scenes and viewport targets; the initial runtime can still populate only one.
struct RenderCamera {
    RenderCamera(Scene* scene, CameraComponent* camera, Nullable<AppFrameTarget*> target = nullptr) noexcept;

    Scene* RenderScene;
    CameraComponent* ViewCamera;
    Nullable<AppFrameTarget*> Target{nullptr};
};

class RenderCameraList {
public:
    void Add(RenderCamera camera);
    void Add(Scene* scene, CameraComponent* camera, Nullable<AppFrameTarget*> target = nullptr);
    void Clear() noexcept;

    bool Empty() const noexcept;
    std::size_t Size() const noexcept;

    std::span<RenderCamera> Cameras() noexcept;
    std::span<const RenderCamera> Cameras() const noexcept;

private:
    vector<RenderCamera> _cameras;
};

class RenderPipelinePass {
public:
    explicit RenderPipelinePass(RenderPassEvent event = RenderPassEvent::AfterRendering) noexcept;
    RenderPipelinePass(const RenderPipelinePass&) = delete;
    RenderPipelinePass(RenderPipelinePass&&) = delete;
    RenderPipelinePass& operator=(const RenderPipelinePass&) = delete;
    RenderPipelinePass& operator=(RenderPipelinePass&&) = delete;
    virtual ~RenderPipelinePass() noexcept;

    RenderPassEvent GetRenderPassEvent() const noexcept;
    void SetRenderPassEvent(RenderPassEvent event) noexcept;

    virtual void Setup(RenderPipelineContext& ctx, const RenderCamera& camera);
    virtual void Execute(RenderPipelineContext& ctx, const RenderCamera& camera);
    virtual void Cleanup(RenderPipelineContext& ctx, const RenderCamera& camera);

private:
    RenderPassEvent _event{RenderPassEvent::AfterRendering};
};

class RenderPipeline {
public:
    RenderPipeline() noexcept = default;
    RenderPipeline(const RenderPipeline&) = delete;
    RenderPipeline(RenderPipeline&&) = delete;
    RenderPipeline& operator=(const RenderPipeline&) = delete;
    RenderPipeline& operator=(RenderPipeline&&) = delete;
    virtual ~RenderPipeline() noexcept;

    /// Public frame entry points. Unlike Unity SRP, these are not called by the
    /// engine with a ready-made List<Camera>; our RenderSystem/SceneRenderer owns
    /// viewport targeting and lets the pipeline build or consume RenderCameraList.
    void BeginFrame(RenderPipelineContext& ctx);
    void BuildCameraList(RenderPipelineContext& ctx, RenderCameraList& cameras);
    void Render(RenderPipelineContext& ctx, const RenderCameraList& cameras);
    void EndFrame(RenderPipelineContext& ctx);

    void EnqueuePass(RenderPipelinePass* pass);
    void ClearPasses() noexcept;

    std::span<RenderPipelinePass*> ActivePasses() noexcept;
    std::span<RenderPipelinePass* const> ActivePasses() const noexcept;

    /// 本管线的标准材质工厂 (把中性材质描述翻译成本管线材质)。
    /// 返回 null 表示该管线不支持标准材质导入。由管线持有, 生命周期 == 管线。
    virtual Nullable<IStandardMaterialFactory*> GetStandardMaterialFactory() noexcept { return nullptr; }
    virtual shared_ptr<const MaterialRenderSnapshot> GetErrorMaterial() noexcept { return nullptr; }

protected:
    /// Override points for concrete pipelines. They mirror SRP's frame/camera
    /// phases, but do not directly wrap ScriptableRenderContext.Submit; command
    /// submission remains owned by our GpuSystem/RenderSystem frame flow.
    virtual void OnBeginFrame(RenderPipelineContext& ctx);
    virtual void OnBuildCameraList(RenderPipelineContext& ctx, RenderCameraList& cameras);
    virtual void OnRender(RenderPipelineContext& ctx, const RenderCameraList& cameras);
    virtual void OnEndFrame(RenderPipelineContext& ctx);

    virtual void OnRenderCamera(RenderPipelineContext& ctx, const RenderCamera& camera);
    virtual void OnSetupCamera(RenderPipelineContext& ctx, const RenderCamera& camera);
    virtual void OnSetupCulling(RenderPipelineContext& ctx, const RenderCamera& camera);
    virtual void OnSetupLights(RenderPipelineContext& ctx, const RenderCamera& camera);
    virtual void OnAddRenderPasses(RenderPipelineContext& ctx, const RenderCamera& camera);
    virtual void OnExecutePasses(RenderPipelineContext& ctx, const RenderCamera& camera);
    virtual void OnFinishCamera(RenderPipelineContext& ctx, const RenderCamera& camera);

    void PrepareTargets(RenderPipelineContext& ctx);
    void TransitionTarget(RenderPipelineContext& ctx, RenderPipelineTarget& target, render::TextureStates after);
    bool RenderTargetContent(RenderPipelineContext& ctx, RenderPipelineTarget& target);
    void ClearTarget(RenderPipelineContext& ctx, RenderPipelineTarget& target, std::string_view name);

private:
    vector<RenderPipelinePass*> _activePasses;
};

}  // namespace radray
