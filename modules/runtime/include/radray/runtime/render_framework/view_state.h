#pragma once

#include <radray/runtime/render_framework/render_graph.h>
#include <radray/runtime/render_framework/render_view.h>
#include <radray/runtime/render_framework/primitive_history.h>

namespace radray {

enum class ViewHistoryInvalidationReason : uint8_t { None,
                                                     FirstFrame,
                                                     CameraCut,
                                                     ExtentChanged,
                                                     FormatChanged,
                                                     SampleCountChanged,
                                                     Explicit,
                                                     Inactive };
using ViewHistoryKey = string;
enum class HistoryCommitMode : uint8_t { Independent,
                                         WithView };
struct HistoryTextureRequest {
    ViewHistoryKey Key;
    string DebugName;
    RuntimeTextureDesc Desc;
    uint32_t BufferCount{2};
    HistoryCommitMode CommitMode{HistoryCommitMode::Independent};
};
struct HistoryWriteToken {
    ViewStateId View;
    ViewHistoryKey Key;
    uint64_t Generation{0}, FrameSerial{0};
    uint32_t Index{0};
    HistoryCommitMode CommitMode{HistoryCommitMode::Independent};
};
struct HistoryTexturePair {
    Nullable<RenderExternalTexture*> Previous{nullptr}, Current{nullptr};
    bool PreviousValid{false};
    HistoryWriteToken CommitToken;
};
struct ViewStateStats {
    uint32_t ActiveViews{0}, HistoryGenerations{0}, RetiredGenerations{0};
    uint64_t TexturesCreated{0}, GenerationsDestroyed{0};
};

class ViewStateRegistry {
public:
    ViewStateRegistry(render::Device& device, render::RenderPassRegistry& registry, uint32_t flightCount, uint64_t inactiveFrames = 120);
    ~ViewStateRegistry();
    ViewStateRegistry(const ViewStateRegistry&) = delete;
    ViewStateRegistry& operator=(const ViewStateRegistry&) = delete;
    /// Render thread, after this flight's previous GPU work has completed.
    void BeginFlight(uint32_t flight, uint64_t serial);
    void Resolve(ResolvedRenderView& view, const ResolvedRenderViewFamily& family);
    bool CommitView(ViewStateId id);
    bool CommitViewWithHistory(ViewStateId id, std::span<const HistoryWriteToken> tokens);
    bool PreparePrimitiveHistory(ResolvedRenderView& view, const RenderSceneSnapshot& snapshot);
    PrimitiveMotionData GetPrimitiveMotion(ViewStateId id, const RenderPrimitiveData& primitive) const noexcept;
    uint64_t GetCommittedSerial(ViewStateId id) const noexcept;
    uint64_t GetPrimitiveCommittedSerial(ViewStateId id) const noexcept;
    void InvalidateTemporal(ViewStateId id, ViewHistoryInvalidationReason reason = ViewHistoryInvalidationReason::Explicit);
    void Invalidate(ViewStateId id, ViewHistoryInvalidationReason reason = ViewHistoryInvalidationReason::Explicit);
    HistoryTexturePair AcquireHistoryTexture(const ResolvedRenderView& view, const ResolvedRenderViewFamily& family,
                                             const HistoryTextureRequest& request, string& reason);
    bool CommitHistory(const HistoryWriteToken& token);
    ViewHistoryInvalidationReason GetInvalidationReason(ViewStateId id) const;
    ViewStateStats GetStats() const;
    void Clear();

private:
    struct Impl;
    unique_ptr<Impl> _impl;
};

}  // namespace radray
