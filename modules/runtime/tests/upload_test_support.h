#pragma once

#include "graph_compile_device.h"
#include <radray/runtime/static_mesh.h>

namespace radray::test {

class UploadTestBuffer final : public render::Buffer {
public:
    UploadTestBuffer(render::Device* device, render::BufferDescriptor desc, int& live)
        : _device(device), _desc(desc), _bytes(desc.Size), _live(live) {
        if (_desc.Memory == render::MemoryType::Device) ++_live;
    }
    ~UploadTestBuffer() override {
        if (_desc.Memory == render::MemoryType::Device) --_live;
    }
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}
    void* Map(uint64_t offset, uint64_t) noexcept override { return _bytes.data() + offset; }
    void Unmap() noexcept override {}
    void FlushMappedRange(render::BufferRange) noexcept override {}
    void InvalidateMappedRange(render::BufferRange) noexcept override {}
    render::BufferDescriptor GetDesc() const noexcept override { return _desc; }
    render::Device* GetDevice() const noexcept override { return _device; }

private:
    render::Device* _device;
    render::BufferDescriptor _desc;
    vector<byte> _bytes;
    int& _live;
};
class UploadTestTexture final : public render::Texture {
public:
    UploadTestTexture(render::TextureDescriptor desc, int& live) : _desc(desc), _live(live) { ++_live; }
    ~UploadTestTexture() override { --_live; }
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}
    render::TextureDescriptor GetDesc() const noexcept override { return _desc; }

private:
    render::TextureDescriptor _desc;
    int& _live;
};
class UploadTestTextureView final : public render::TextureView {
public:
    UploadTestTextureView(render::TextureViewDescriptor desc, int& live) : _desc(desc), _live(live) { ++_live; }
    ~UploadTestTextureView() override { --_live; }
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}
    render::TextureViewDescriptor GetDesc() const noexcept override { return _desc; }

private:
    render::TextureViewDescriptor _desc;
    int& _live;
};
class UploadTestDevice final : public GraphCompileDevice {
public:
    int LiveDeviceBuffers{0}, DeviceAllocations{0}, FailDeviceAllocation{0};
    int LiveTextures{0}, LiveTextureViews{0};
    Nullable<unique_ptr<render::Buffer>> CreateBuffer(const render::BufferDescriptor& desc) noexcept override {
        if (desc.Memory == render::MemoryType::Device && ++DeviceAllocations == FailDeviceAllocation) return nullptr;
        return unique_ptr<render::Buffer>{new UploadTestBuffer(this, desc, LiveDeviceBuffers)};
    }
    Nullable<unique_ptr<render::Texture>> CreateTexture(const render::TextureDescriptor& desc) noexcept override {
        return unique_ptr<render::Texture>{new UploadTestTexture(desc, LiveTextures)};
    }
    Nullable<unique_ptr<render::TextureView>> CreateTextureView(const render::TextureViewDescriptor& desc) noexcept override {
        return unique_ptr<render::TextureView>{new UploadTestTextureView(desc, LiveTextureViews)};
    }
};
class UploadTestCommand final : public render::CommandBuffer {
public:
    uint32_t Copies{0};
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}
    void Begin() noexcept override {}
    void End() noexcept override {}
    void PushDebugGroup(std::string_view) noexcept override {}
    void PopDebugGroup() noexcept override {}
    void ResourceBarrier(std::span<const render::ResourceBarrierDescriptor>) noexcept override {}
    Nullable<unique_ptr<render::GraphicsCommandEncoder>> BeginRenderPass(const render::RenderPassBeginDescriptor&) noexcept override { return nullptr; }
    void EndRenderPass(unique_ptr<render::GraphicsCommandEncoder>) noexcept override {}
    Nullable<unique_ptr<render::ComputeCommandEncoder>> BeginComputePass() noexcept override { return nullptr; }
    void EndComputePass(unique_ptr<render::ComputeCommandEncoder>) noexcept override {}
    void CopyBufferToBuffer(render::Buffer*, uint64_t, render::Buffer*, uint64_t, uint64_t) noexcept override { ++Copies; }
    void CopyBufferToTexture(render::Texture*, render::SubresourceRange, render::Buffer*, uint64_t) noexcept override { ++Copies; }
    void CopyTextureToBuffer(render::Buffer*, uint64_t, render::Texture*, render::SubresourceRange) noexcept override { ++Copies; }
    void CopyTextureToTexture(const render::TextureCopyDescriptor&) noexcept override { ++Copies; }
    void ResolveTexture(const render::TextureResolveDescriptor&) noexcept override {}
    void ResetQueryPool(render::QueryPool*, uint32_t, uint32_t) noexcept override {}
    void WriteTimestamp(const render::QueryTimestampDescriptor&) noexcept override {}
    void ResolveQueryData(const render::QueryResolveDescriptor&) noexcept override {}
};
inline MeshResource MakeUploadTestMesh() {
    const array<float, 9> vertices{-1, -1, 0, 1, -1, 0, 0, 1, 0};
    const array<uint32_t, 3> indices{0, 1, 2};
    MeshResource mesh;
    mesh.Name = "upload-test-mesh";
    mesh.Bins.emplace_back(std::as_bytes(std::span{vertices}));
    mesh.Bins.emplace_back(std::as_bytes(std::span{indices}));
    MeshPrimitive primitive;
    primitive.VertexCount = 3;
    primitive.Topology = PrimitiveTopology::TriangleList;
    primitive.VertexBuffers.push_back({string{VertexSemantics::POSITION}, 0, 0, VertexDataType::FLOAT, 3, 0, 12});
    primitive.IndexBuffer = {1, 3, 0, 4};
    mesh.Primitives.push_back(std::move(primitive));
    return mesh;
}
constexpr AssetId kUploadTestId{0xaabbccdd, 0x1122, 0x3344, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc};

}  // namespace radray::test
