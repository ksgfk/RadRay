#include <radray/runtime/readback_queue.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <numeric>
#include <optional>
#include <utility>

#include <radray/gpu_impl/validation.h>

namespace radray {
namespace {
std::optional<ReadbackTexturePlan> SourcePlan(const RDGBuilder& graph, RDGTextureHandle source, uint32_t mip, uint32_t layer) {
    const auto desc = graph.GetTextureDescriptor(source);
    if (!desc || !desc->Usage.HasFlag(gpu::TextureUse::CopySource) || desc->SampleCount != 1 || mip >= desc->MipLevels || mip >= 32) return std::nullopt;
    const bool volume = desc->Dim == gpu::TextureDimension::Dim3D;
    if (layer >= (volume ? 1u : desc->DepthOrArraySize)) return std::nullopt;
    return ReadbackTexturePlan{std::max(1u, desc->Width >> mip), std::max(1u, desc->Height >> mip), desc->Format,
                               volume ? std::max(1u, desc->DepthOrArraySize >> mip) : 1u};
}
}

AsyncReadbackQueue::AsyncReadbackQueue(gpu::Device& device, size_t maxActiveRequests) noexcept : _device(device), _maximum(maxActiveRequests) {}
bool AsyncReadbackQueue::HasRequest(uint64_t id) const noexcept { return _requests.contains(id); }
size_t AsyncReadbackQueue::GetActiveRequestCount() const noexcept { return _requests.size(); }

bool AsyncReadbackQueue::ReserveTexture(uint64_t id, const ReadbackTexturePlan& plan) {
    if (HasRequest(id) || _requests.size() >= _maximum || !plan.Width || !plan.Height || !plan.Depth || gpu::IsDepthStencilFormat(plan.Format)) return false;
    const auto block = gpu::GetTextureFormatBlockInfo(plan.Format);
    if (!block.BytesPerBlock) return false;
    const uint64_t rowBytes = uint64_t{block.Columns(plan.Width)} * block.BytesPerBlock;
    const uint64_t rows = uint64_t{block.Rows(plan.Height)} * plan.Depth;
    const auto alignment = std::lcm<uint64_t>(block.BytesPerBlock, std::max(1u, _device.GetDetail().TextureDataPitchAlignment));
    const auto pitch = ((rowBytes + alignment - 1) / alignment) * alignment;
    if (pitch > std::numeric_limits<uint32_t>::max() || rows > std::numeric_limits<size_t>::max() / pitch) return false;
    const uint64_t size = pitch * rows;
    const auto limit = _device.GetCapabilities().Limits.MaxBufferSize;
    if (limit && size > limit) return false;
    Storage staging;
    auto best = _free.end();
    for (auto it = _free.begin(); it != _free.end(); ++it) {
        const auto available = it->Buffer->GetDesc().Size;
        if (available >= size && (best == _free.end() || available < best->Buffer->GetDesc().Size)) best = it;
    }
    if (best != _free.end()) {
        staging = std::move(*best);
        _free.erase(best);
    } else {
        auto buffer = _device.CreateBuffer({.Size = size, .Memory = gpu::MemoryType::ReadBack, .Usage = gpu::BufferUse::CopyDestination | gpu::BufferUse::MapRead});
        if (!buffer) return false;
        staging.Buffer = buffer.Release();
        if (!_free.empty()) _free.pop_back();
    }
    _requests.emplace(id, Request{{false, plan, rowBytes * rows, rowBytes}, std::move(staging), pitch, rows});
    return true;
}

bool AsyncReadbackQueue::EnqueueReserved(RDGBuilder& graph, uint64_t id, RDGTextureHandle source, uint32_t mip, uint32_t layer) {
    const auto plan = SourcePlan(graph, source, mip, layer);
    const auto found = _requests.find(id);
    if (!plan || found == _requests.end() || found->second.Info.Texture != *plan) return false;
    return EnqueueReserved(graph, id, source, {mip, layer, 0, 0, plan->Width, plan->Height, 0, plan->Depth});
}

bool AsyncReadbackQueue::EnqueueReserved(RDGBuilder& graph, uint64_t id, RDGTextureHandle source, const gpu::TextureRegion2D& region) {
    const auto found = _requests.find(id);
    if (found == _requests.end() || found->second.Progress != State::Reserved || (_pendingGraph && _pendingGraph.Get() != &graph)) return false;
    auto& request = found->second;
    const auto desc = graph.GetTextureDescriptor(source);
    if (!desc || request.Info.Texture != ReadbackTexturePlan{region.Width, region.Height, desc->Format, region.Depth}) return false;
    const gpu::TextureBufferCopyRegion transfer{region, 0, static_cast<uint32_t>(request.RowPitch)};
    if (!gpu::ValidateTextureReadbackRegion(*desc, request.Staging.Buffer->GetDesc(), transfer, _device.GetDetail()).first) return false;
    const auto dst = graph.ImportBuffer(request.Staging.Buffer.get(), request.Staging.LastState, gpu::BufferStates{gpu::BufferState::HostRead});
    struct Copy {
        shared_ptr<gpu::Buffer> Buffer;
        RDGTextureHandle Source;
        gpu::TextureBufferCopyRegion Region;
    };
    graph.AddCopyPass("TextureReadback", Copy{request.Staging.Buffer, source, transfer},
        +[](const Copy& data, RDGCopyPassContext& context) noexcept {
            return context.GetCommandBuffer().CopyTextureToBufferRegion({&context.Resolve(data.Source), data.Buffer.get(), data.Region});
        }).ReadTexture(source).WriteBuffer(dst);
    graph.MarkOutput(dst);
    request.Progress = State::Enqueued;
    _pendingGraph = &graph;
    return true;
}

bool AsyncReadbackQueue::EnqueueTexture(RDGBuilder& graph, uint64_t id, RDGTextureHandle source, uint32_t mip, uint32_t layer) {
    const auto plan = SourcePlan(graph, source, mip, layer);
    return plan && EnqueueTexture(graph, id, source, {mip, layer, 0, 0, plan->Width, plan->Height, 0, plan->Depth});
}

bool AsyncReadbackQueue::EnqueueTexture(RDGBuilder& graph, uint64_t id, RDGTextureHandle source, const gpu::TextureRegion2D& region) {
    const auto desc = graph.GetTextureDescriptor(source);
    if (!desc || !gpu::ValidateTextureReadbackRegion(*desc, region).first ||
        !ReserveTexture(id, {region.Width, region.Height, desc->Format, region.Depth})) return false;
    if (EnqueueReserved(graph, id, source, region)) return true;
    auto found = _requests.find(id);
    _free.push_back(std::move(found->second.Staging));
    _requests.erase(found);
    return false;
}

void AsyncReadbackQueue::CancelUnsubmitted() noexcept {
    for (auto it = _requests.begin(); it != _requests.end();) {
        if (it->second.Progress == State::Submitted) {
            ++it;
        } else {
            _free.push_back(std::move(it->second.Staging));
            it = _requests.erase(it);
        }
    }
    _pendingGraph = nullptr;
}

void AsyncReadbackQueue::MarkSubmitted(uint64_t value) noexcept {
    for (auto& [id, request] : _requests) {
        if (request.Progress != State::Enqueued) continue;
        request.Progress = State::Submitted;
        request.Fence = value;
        request.Staging.LastState = gpu::BufferState::HostRead;
    }
    _pendingGraph = nullptr;
}

bool AsyncReadbackQueue::Poll(uint64_t id, uint64_t completed, ReadbackResultInfo& info, Nullable<byte*> destination, size_t capacity) {
    const auto found = _requests.find(id);
    if (found == _requests.end()) return false;
    auto& request = found->second;
    if (destination && capacity < request.Info.PackedByteSize) return false;
    auto result = request.Info;
    result.Ready = request.Progress == State::Submitted && completed >= request.Fence;
    if (result.Ready && destination) {
        auto& buffer = *request.Staging.Buffer;
        const auto size = request.RowPitch * request.Rows;
        const auto* data = static_cast<const byte*>(buffer.Map(0, size));
        if (!data) return false;
        buffer.InvalidateMappedRange({0, size});
        for (uint64_t row = 0; row < request.Rows; ++row) {
            std::memcpy(destination.Get() + row * result.PackedRowPitch, data + row * request.RowPitch, result.PackedRowPitch);
        }
        buffer.Unmap();
        _free.push_back(std::move(request.Staging));
        _requests.erase(found);
    }
    info = result;
    return true;
}

}  // namespace radray
