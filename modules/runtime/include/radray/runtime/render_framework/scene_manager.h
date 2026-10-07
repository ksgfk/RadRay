#pragma once

#include <span>
#include <thread>

#include <radray/nullable.h>
#include <radray/runtime/render_framework/render_scene.h>
#include <radray/runtime/render_framework/scene_gpu.h>
#include <radray/runtime/render_framework/scene_view.h>
#include <radray/runtime/render_framework/scene_writer.h>
#include <radray/types.h>

namespace radray {

class Application;
class WorldRenderBridge;
struct FlightCompletion;

/// Scene delivery and lifetime management. GT and RT registries communicate only through sealed flights.
/// Contract: docs/architecture/render-framework.md
class SceneManager {
public:
    explicit SceneManager(uint32_t flightCount);
    SceneManager(const SceneManager&) = delete;
    SceneManager(SceneManager&&) = delete;
    SceneManager& operator=(const SceneManager&) = delete;
    SceneManager& operator=(SceneManager&&) = delete;
    ~SceneManager() noexcept;

    /// GT: Worlds disconnected, RT stopped, GPU idle, completions consumed; accepts partial initialization.
    void OnShutdown() noexcept;
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
    std::optional<SceneGpuView> PrepareSceneGpuRT(SceneId id, AppFrameContext& frame);
    std::optional<SceneGpuView> PrepareSceneGpuRT(SceneId id, PipelineContext& context, vector<render::CommandBuffer*>& outCommands);
    std::span<const SceneViewRequest> GetFrameViewsRT(uint32_t flightIndex) const;
    /// RT only, or inspection after RT stops. Borrow expires at flight completion.
    std::span<const SceneFrameUpdate> GetFrameUpdatesRT(uint32_t flightIndex) const;

private:
    friend class Application;
    friend class WorldRenderBridge;
    void CheckCanModifyGT() const;
    void SetCollecting(bool collecting);
    SceneWriter& ClaimSceneWriterGT(SceneId id);
    void ReleaseSceneWriterGT(SceneId id);

    struct SceneRecord {
        unique_ptr<SceneWriter> Writer;
        bool CreatePending{true};
        bool DestroySealed{false};
    };
    struct SceneSlotRT {
        uint32_t Generation{0};
        unique_ptr<RenderScene> Scene;
        unique_ptr<SceneGpuData> Gpu;
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
        vector<SceneId> PreparedScenes;
        vector<SceneViewRequest> Views;
        vector<unique_ptr<SceneGpuData>> RetiredGpu;
        uint64_t CompletedSerial{0};
        bool CompletedExecuted{false};
    };
    FrameUpdates& GetFrameUpdates(uint32_t flightIndex);
    Nullable<SceneSlotRT*> PrepareSceneSlotRT(SceneId id, uint32_t flightIndex, uint64_t frameSerial, render::Device* device);

    vector<FrameUpdates> _frameUpdates;
    SparseSet<SceneRecord> _scenesGT;
    vector<SceneSlotRT> _scenesRT;
    SceneApplyChanges _applyChanges;
    // GT owns seal, publish, completion and stopping; RT owns consumption.
    uint64_t _nextUpdateSequence{1};
    uint64_t _lastPublishedSequence{0};
    uint64_t _lastConsumedSequence{0};
    uint64_t _lastFrameSerial{0};
    bool _stopping{false};
    bool _collecting{false};
    std::thread::id _ownerThread{std::this_thread::get_id()};
};

}  // namespace radray
