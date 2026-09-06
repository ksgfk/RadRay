#pragma once

#include <radray/render/rhi.h>

namespace radray::test {

class FailingGraphCommand final : public render::CommandBuffer {
public:
    explicit FailingGraphCommand(render::CommandBuffer& command) : _command(command) {}
    bool IsValid() const noexcept override { return _command.IsValid(); }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view name) noexcept override { _command.SetDebugName(name); }
    void Begin() noexcept override { _command.Begin(); }
    void End() noexcept override { _command.End(); }
    void PushDebugGroup(std::string_view name) noexcept override {
        ++RecordingCalls;
        _command.PushDebugGroup(name);
    }
    void PopDebugGroup() noexcept override { _command.PopDebugGroup(); }
    void ResourceBarrier(std::span<const render::ResourceBarrierDescriptor> barriers) noexcept override {
        ++RecordingCalls;
        _command.ResourceBarrier(barriers);
    }
    Nullable<unique_ptr<render::GraphicsCommandEncoder>> BeginRenderPass(const render::RenderPassBeginDescriptor& desc) noexcept override { return PassesBeforeFailure && PassesBeforeFailure-- ? _command.BeginRenderPass(desc) : nullptr; }
    void EndRenderPass(unique_ptr<render::GraphicsCommandEncoder> encoder) noexcept override { _command.EndRenderPass(std::move(encoder)); }
    Nullable<unique_ptr<render::ComputeCommandEncoder>> BeginComputePass() noexcept override { return PassesBeforeFailure && PassesBeforeFailure-- ? _command.BeginComputePass() : nullptr; }
    void EndComputePass(unique_ptr<render::ComputeCommandEncoder> encoder) noexcept override { _command.EndComputePass(std::move(encoder)); }
    void CopyBufferToBuffer(render::Buffer* dst, uint64_t dstOffset, render::Buffer* src, uint64_t srcOffset, uint64_t size) noexcept override { _command.CopyBufferToBuffer(dst, dstOffset, src, srcOffset, size); }
    void CopyBufferToTexture(render::Texture* dst, render::SubresourceRange range, render::Buffer* src, uint64_t offset) noexcept override { _command.CopyBufferToTexture(dst, range, src, offset); }
    void CopyTextureToBuffer(render::Buffer* dst, uint64_t offset, render::Texture* src, render::SubresourceRange range) noexcept override { _command.CopyTextureToBuffer(dst, offset, src, range); }
    void CopyTextureToTexture(const render::TextureCopyDescriptor& desc) noexcept override { _command.CopyTextureToTexture(desc); }
    void ResolveTexture(const render::TextureResolveDescriptor& desc) noexcept override { _command.ResolveTexture(desc); }
    void ResetQueryPool(render::QueryPool* pool, uint32_t first, uint32_t count) noexcept override { _command.ResetQueryPool(pool, first, count); }
    void WriteTimestamp(const render::QueryTimestampDescriptor& desc) noexcept override { _command.WriteTimestamp(desc); }
    void ResolveQueryData(const render::QueryResolveDescriptor& desc) noexcept override { _command.ResolveQueryData(desc); }

    uint32_t RecordingCalls{0};
    uint32_t PassesBeforeFailure{0};

private:
    render::CommandBuffer& _command;
};

}  // namespace radray::test
