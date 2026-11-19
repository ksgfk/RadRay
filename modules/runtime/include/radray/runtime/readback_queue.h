#pragma once

#include <cstddef>
#include <cstdint>

#include <radray/gpu/types.h>
#include <radray/nullable.h>
#include <radray/runtime/rdg.h>
#include <radray/types.h>

namespace radray {

struct ReadbackTexturePlan {
    uint32_t Width{}, Height{};
    gpu::TextureFormat Format{gpu::TextureFormat::UNKNOWN};
    uint32_t Depth{1};
    friend bool operator==(const ReadbackTexturePlan&, const ReadbackTexturePlan&) = default;
};

struct ReadbackResultInfo {
    bool Ready{};
    ReadbackTexturePlan Texture;
    uint64_t PackedByteSize{}, PackedRowPitch{};
};

// A queue uses one submission fence and has at most one graph awaiting submission.
class AsyncReadbackQueue final {
public:
    explicit AsyncReadbackQueue(gpu::Device& device, size_t maxActiveRequests = 4096) noexcept;
    AsyncReadbackQueue(const AsyncReadbackQueue&) = delete;
    AsyncReadbackQueue& operator=(const AsyncReadbackQueue&) = delete;

    bool HasRequest(uint64_t requestId) const noexcept;
    size_t GetActiveRequestCount() const noexcept;
    bool ReserveTexture(uint64_t requestId, const ReadbackTexturePlan& plan);
    bool EnqueueReserved(RDGBuilder& graph, uint64_t requestId, RDGTextureHandle source, uint32_t mipLevel = 0, uint32_t arrayLayer = 0);
    bool EnqueueTexture(RDGBuilder& graph, uint64_t requestId, RDGTextureHandle source, uint32_t mipLevel = 0, uint32_t arrayLayer = 0);
    bool EnqueueReserved(RDGBuilder& graph, uint64_t requestId, RDGTextureHandle source, const gpu::TextureRegion2D& region);
    bool EnqueueTexture(RDGBuilder& graph, uint64_t requestId, RDGTextureHandle source, const gpu::TextureRegion2D& region);
    void CancelUnsubmitted() noexcept;
    void MarkSubmitted(uint64_t frameFenceValue) noexcept;
    bool Poll(uint64_t requestId, uint64_t completedFenceValue, ReadbackResultInfo& outInfo, Nullable<byte*> destination = nullptr, size_t capacity = 0);

private:
    enum class State { Reserved, Enqueued, Submitted };
    struct Storage {
        shared_ptr<gpu::Buffer> Buffer;
        gpu::BufferStates LastState{gpu::BufferState::Undefined};
    };
    struct Request {
        ReadbackResultInfo Info;
        Storage Staging;
        uint64_t RowPitch{}, Rows{}, Fence{};
        State Progress{State::Reserved};
    };
    gpu::Device& _device;
    size_t _maximum;
    unordered_map<uint64_t, Request> _requests;
    vector<Storage> _free;
    Nullable<RDGBuilder*> _pendingGraph{nullptr};
};

}  // namespace radray
