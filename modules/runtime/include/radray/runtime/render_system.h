#pragma once

#include <span>
#include <string_view>

#include <radray/nullable.h>
#include <radray/render/render_pass_registry.h>
#include <radray/runtime/shader_program.h>
#include <radray/runtime/render_framework/scene.h>
#include <radray/runtime_type.h>
#include <radray/types.h>

namespace radray {

class Application;
class GpuSystem;
class ShaderProgramCache;
class World;
struct AppUpdateContext;
struct FlightCompletion;

/// Persistent scene, per-flight updates and render caches. GPU flight ownership stays in GpuSystem.
/// Contract: docs/architecture/render-framework.md
class RenderSystem {
public:
    explicit RenderSystem(Application* app, uint32_t flightCount);
    RenderSystem(const RenderSystem&) = delete;
    RenderSystem(RenderSystem&&) = delete;
    RenderSystem& operator=(const RenderSystem&) = delete;
    RenderSystem& operator=(RenderSystem&&) = delete;
    ~RenderSystem() noexcept;

    [[nodiscard]] bool OnInitialize();
    /// GT, requires RT stopped, GPU idle and completions consumed; accepts partial initialization.
    void OnShutdown() noexcept;
    void SetGpuSystem(Nullable<GpuSystem*> gpu) noexcept { _gpuSystem = gpu; }
    render::RenderPassRegistry* GetRenderPassRegistry() const noexcept { return _renderPassRegistry.get(); }

    /// GT, borrow only while the runner owns the writable slot, before PrepareFrameGT.
    SceneUpdateBatch& GetFrameUpdateBatchGT(uint32_t flightIndex);
    /// GT collection hook after World::Tick.
    void PrepareFrameGT(World& world, const AppUpdateContext& ctx);
    /// RT, every published flight, including skipped draws; no GPU/World access.
    void ConsumeRenderUpdates(uint32_t flightIndex);
    /// GT, clear the matching batch after real fence completion; slot ownership stays with the runner.
    void OnFlightCompletedGT(const FlightCompletion& completion);
    /// GT, clear an unpublished batch in a slot owned by the caller.
    void AbandonUnpublishedFrameGT(uint32_t flightIndex);
    /// GT, after RT stops and real completions have been consumed.
    void AbandonUnpublishedFramesGT();

    /// RT only, or after RT stops. This reference is not a per-flight snapshot.
    const Scene& GetScene() const noexcept { return _scene; }

    Nullable<ShaderProgram*> GetOrCreateShaderProgram(const ShaderProgramRequest& request);
    Nullable<ShaderProgram*> GetOrCreateShaderProgram(
        std::span<const byte> artifact,
        const shader::GpuArtifactHash& expectedIdentity,
        const render::ShaderProgramLayoutRecipe& recipe = {});
    size_t GetShaderProgramCacheSize() const noexcept;
    size_t GetShaderArtifactCacheSize() const noexcept;
    bool InvalidateShaderSource(std::string_view sourceName);

private:
    SceneUpdateBatch& GetFrameUpdateBatch(uint32_t flightIndex);

    Application* _app;
    Nullable<GpuSystem*> _gpuSystem{nullptr};
    unique_ptr<render::RenderPassRegistry> _renderPassRegistry;
    unique_ptr<ShaderProgramCache> _shaderCache;
    vector<SceneUpdateBatch> _frameUpdates;
    Scene _scene;
};

template <>
struct RuntimeTypeTrait<RenderSystem> {
    static constexpr RuntimeTypeId value{0x241d4e78, 0x8f4e, 0x4d1c, 0xa8, 0xb9, 0x55, 0x09, 0x61, 0x6a, 0x90, 0x24};
};

}  // namespace radray
