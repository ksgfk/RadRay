#pragma once

#include <span>
#include <string_view>
#include <thread>

#include <radray/nullable.h>
#include <radray/render/render_pass_registry.h>
#include <radray/runtime/shader_program.h>
#include <radray/runtime/render_scene/render_scene.h>
#include <radray/runtime/render_scene/scene_writer.h>
#include <radray/runtime_type.h>
#include <radray/types.h>

namespace radray {

class Application;
class GpuSystem;
class ShaderProgramCache;
class WorldRenderBridge;
struct FlightCompletion;

/// Scene delivery and shared render services. GT and RT registries communicate only through sealed flights.
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
    /// GT: Worlds disconnected, RT stopped, GPU idle, completions consumed; accepts partial initialization.
    void OnShutdown() noexcept;
    void SetGpuSystem(Nullable<GpuSystem*> gpu) noexcept { _gpuSystem = gpu; }
    render::RenderPassRegistry* GetRenderPassRegistry() const noexcept { return _renderPassRegistry.get(); }

    SceneId CreateSceneGT();
    /// Empty for stale, closing, or World-owned scenes.
    Nullable<SceneWriter*> GetSceneWriterGT(SceneId id) noexcept;
    void DestroySceneGT(SceneId id);
    /// GT: runner owns the writable flight; collects all writers once before publication.
    void SealFrameGT(uint32_t flightIndex);
    void PublishFrameGT(uint32_t flightIndex);
    /// RT: exactly once per published flight, including skipped draws; no World access.
    void ConsumeRenderUpdates(uint32_t flightIndex, uint64_t frameSerial);
    uint64_t GetUpdateSequence(uint32_t flightIndex) const;
    uint64_t GetFrameSerial(uint32_t flightIndex) const;
    void BeginStoppingGT() noexcept;
    /// GT: releases retired owners and closing identities after a real fence completion.
    void OnFlightCompletedGT(const FlightCompletion& completion);
    /// Shutdown only: RT stopped, GPU idle and real completions consumed.
    void AbandonUnpublishedFrameGT(uint32_t flightIndex);
    void AbandonUnpublishedFramesGT();
    /// RT only, or after RT stops. Borrow ends at the next Apply or scene destruction.
    Nullable<const RenderScene*> GetSceneRT(SceneId id) const noexcept;
    /// RT only, or inspection after RT stops. Borrow expires at flight completion.
    std::span<const SceneFrameUpdate> GetFrameUpdatesRT(uint32_t flightIndex) const;

    Nullable<ShaderProgram*> GetOrCreateShaderProgram(const ShaderProgramRequest& request);
    Nullable<ShaderProgram*> GetOrCreateShaderProgram(
        std::span<const byte> artifact,
        const shader::GpuArtifactHash& expectedIdentity,
        const render::ShaderProgramLayoutRecipe& recipe = {});
    size_t GetShaderProgramCacheSize() const noexcept;
    size_t GetShaderArtifactCacheSize() const noexcept;
    bool InvalidateShaderSource(std::string_view sourceName);

private:
    friend class WorldRenderBridge;
    void CheckCanModifyGT() const;
    void SetCollecting(bool collecting);
    SceneWriter& ClaimSceneWriterGT(SceneId id);
    void ReleaseSceneWriterGT(SceneId id);

    struct SceneRecord {
        unique_ptr<SceneWriter> Writer;
        bool CreatePending{true};
        bool DestroyPending{false};
        bool DestroySealed{false};
    };
    struct SceneSlotRT {
        uint32_t Generation{0};
        unique_ptr<RenderScene> Scene;
    };
    enum class SceneFlightPhase : uint8_t {
        Writable,
        Sealed,
        Published,
        Consumed
    };
    struct FrameUpdates {
        vector<SceneFrameUpdate> Scenes;
        size_t Count{0};
        SceneFlightPhase Phase{SceneFlightPhase::Writable};
        uint64_t UpdateSequence{0};
        uint64_t FrameSerial{0};
    };
    FrameUpdates& GetFrameUpdates(uint32_t flightIndex);

    Application* _app;
    Nullable<GpuSystem*> _gpuSystem{nullptr};
    unique_ptr<render::RenderPassRegistry> _renderPassRegistry;
    unique_ptr<ShaderProgramCache> _shaderCache;
    vector<FrameUpdates> _frameUpdates;
    SparseSet<unique_ptr<SceneRecord>> _scenesGT;
    vector<SceneId> _sceneIdsGT;
    vector<SceneSlotRT> _scenesRT;
    // GT owns seal, publish, completion and stopping; RT owns consumption.
    uint64_t _nextUpdateSequence{1};
    uint64_t _lastPublishedSequence{0};
    uint64_t _lastConsumedSequence{0};
    uint64_t _lastFrameSerial{0};
    bool _stopping{false};
    bool _collecting{false};
    std::thread::id _ownerThread{std::this_thread::get_id()};
};

template <>
struct RuntimeTypeTrait<RenderSystem> {
    static constexpr RuntimeTypeId value{0x241d4e78, 0x8f4e, 0x4d1c, 0xa8, 0xb9, 0x55, 0x09, 0x61, 0x6a, 0x90, 0x24};
};

}  // namespace radray
