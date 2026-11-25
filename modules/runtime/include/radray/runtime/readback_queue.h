#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string_view>
#include <unordered_map>

#include <radray/memory.h>

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

enum class ReadbackStatus : uint8_t { Pending, Ready, SourceOutOfRange, DestinationTooSmall, InvalidRequest, DeviceLost };
std::string_view format_as(ReadbackStatus status) noexcept;

struct ReadbackResultInfo {
    bool Ready{};
    ReadbackTexturePlan Texture;
    uint64_t PackedByteSize{}, PackedRowPitch{};
};

// 一条队列对应一条提交时间线，同时至多有一张待提交图。队列须存活到图停止执行且已提交读回完成。
class AsyncReadbackQueue final {
public:
    explicit AsyncReadbackQueue(gpu::Device& device, size_t maxActiveRequests = 4096) noexcept;
    AsyncReadbackQueue(const AsyncReadbackQueue&) = delete;
    AsyncReadbackQueue& operator=(const AsyncReadbackQueue&) = delete;

    bool HasRequest(uint64_t requestId) const noexcept;
    size_t GetActiveRequestCount() const noexcept;
    bool ReserveBuffer(uint64_t requestId, uint64_t size);
    bool EnqueueReservedBuffer(RDGBuilder& graph, uint64_t requestId, RDGBufferHandle source, uint64_t offset = 0);
    bool EnqueueBuffer(RDGBuilder& graph, uint64_t requestId, RDGBufferHandle source, uint64_t offset, uint64_t size);
    bool ReserveTexture(uint64_t requestId, const ReadbackTexturePlan& plan);
    bool EnqueueReserved(RDGBuilder& graph, uint64_t requestId, RDGTextureHandle source, uint32_t mipLevel = 0, uint32_t arrayLayer = 0);
    bool EnqueueTexture(RDGBuilder& graph, uint64_t requestId, RDGTextureHandle source, uint32_t mipLevel = 0, uint32_t arrayLayer = 0);
    bool EnqueueReserved(RDGBuilder& graph, uint64_t requestId, RDGTextureHandle source, const gpu::TextureRegion2D& region);
    bool EnqueueTexture(RDGBuilder& graph, uint64_t requestId, RDGTextureHandle source, const gpu::TextureRegion2D& region);
    void CancelUnsubmitted() noexcept;
    void MarkSubmitted(uint64_t frameFenceValue) noexcept;
    ReadbackStatus Poll(uint64_t requestId, uint64_t completedFenceValue, ReadbackResultInfo& outInfo, Nullable<byte*> destination = nullptr, size_t capacity = 0);

private:
    enum class State { Reserved, Enqueued, Submitted };
    struct Storage {
        unique_ptr<gpu::Buffer> Buffer;
        Nullable<const byte*> Mapped{nullptr};
        gpu::BufferStates LastState{gpu::BufferState::Undefined};
    };
    struct Request {
        ReadbackResultInfo Info;
        Storage Staging;
        uint64_t RowPitch{}, Rows{}, Fence{};
        State Progress{State::Reserved};
        bool SourceInBounds{true};
    };
    bool Reserve(uint64_t id, const ReadbackResultInfo& info, uint64_t pitch, uint64_t rows);
    bool SetSourceInBounds(uint64_t epoch, uint64_t id, bool inBounds) noexcept;
    void ReleaseReservation(uint64_t id) noexcept;
    gpu::Device& _device;
    size_t _maximum;
    uint64_t _batchEpoch{0};
    HeapMemoryResource _heap;
    std::pmr::unsynchronized_pool_resource _requestMemory{&_heap};
    std::pmr::unordered_map<uint64_t, Request> _requests{&_requestMemory};
    vector<Storage> _free;
    Nullable<RDGBuilder*> _pendingGraph{nullptr};
};

}  // namespace radray
