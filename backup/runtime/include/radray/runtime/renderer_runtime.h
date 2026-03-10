#pragma once

#include <cstddef>
#include <optional>

#include <radray/nullable.h>
#include <radray/render/common.h>
#include <radray/render/gpu_resource.h>
#include <radray/types.h>

#include <radray/runtime/frame_snapshot.h>
#include <radray/runtime/persistent_resource_registry.h>
#include <radray/runtime/render_asset_registry.h>
#include <radray/runtime/render_graph.h>
#include <radray/runtime/render_prepare.h>

namespace radray::runtime {

class UploadSystem;

struct FrameInFlight {
    uint32_t FrameIndex{0};
    uint64_t ExpectedFenceValue{0};
};

class FrameLinearAllocator {
public:
    void Reset() noexcept;
    void* Allocate(size_t size, size_t alignment = alignof(std::max_align_t)) noexcept;

private:
    vector<byte> _storage{};
    size_t _cursor{0};
};

class DescriptorArena {
public:
    explicit DescriptorArena(render::Device* device = nullptr) noexcept;

    void Reset(render::Device* device = nullptr) noexcept;

    Nullable<render::BufferView*> CreateBufferView(const render::BufferViewDescriptor& desc) noexcept;

    Nullable<render::DescriptorSet*> CreateDescriptorSet(render::RootSignature* rootSig, render::DescriptorSetIndex set) noexcept;

private:
    render::Device* _device{nullptr};
    vector<unique_ptr<render::BufferView>> _bufferViews{};
    vector<unique_ptr<render::DescriptorSet>> _descriptorSets{};
};

struct RenderFrameContext {
    uint32_t FrameIndex{0};
    uint32_t BackBufferIndex{0};
    const FrameSnapshot* Snapshot{nullptr};
    PreparedScene Prepared{};
    FrameLinearAllocator* CPUArena{nullptr};
    render::CBufferArena* UploadArena{nullptr};
    DescriptorArena* Descriptors{nullptr};
    render::Device* Device{nullptr};
    render::CommandBuffer* CommandBuffer{nullptr};
    ImportedTextureDesc SwapchainColor{};
    RgResourceHandle SwapchainColorHandle{};
    ImportedBufferDesc CaptureReadback{};
    RgResourceHandle CaptureReadbackHandle{};
    bool CaptureEnabled{false};
    render::Texture* BackBuffer{nullptr};
    render::TextureView* BackBufferRtv{nullptr};
    RenderAssetRegistry* AssetRegistry{nullptr};
    UploadSystem* Uploads{nullptr};
    PersistentResourceRegistry* PersistentResources{nullptr};
    FrameInFlight* InFlight{nullptr};
};

class UploadSystem {
public:
    UploadSystem() noexcept;
    ~UploadSystem() noexcept;

    bool Initialize(render::Device* device) noexcept;

    void Destroy() noexcept;

    bool ProcessPendingUploads(
        render::CommandBuffer* cmd,
        RenderAssetRegistry& assets,
        FrameInFlight& frame,
        Nullable<string*> reason = nullptr) noexcept;

    void ReleaseFrameResources(FrameInFlight& frame) noexcept;

private:
    class Impl;

    unique_ptr<Impl> _impl{};
};

struct RendererRuntimeCreateDesc {
    shared_ptr<render::Device> Device{};
    const void* NativeHandler{nullptr};
    uint32_t Width{0};
    uint32_t Height{0};
    uint32_t BackBufferCount{3};
    uint32_t FlightFrameCount{2};
    render::TextureFormat Format{render::TextureFormat::BGRA8_UNORM};
    render::PresentMode PresentMode{render::PresentMode::FIFO};
};

class RendererRuntime {
public:
    static Nullable<unique_ptr<RendererRuntime>> Create(
        const RendererRuntimeCreateDesc& desc,
        Nullable<string*> reason = nullptr) noexcept;

    ~RendererRuntime() noexcept;

    void Destroy() noexcept;

    bool IsValid() const noexcept;

    RenderAssetRegistry& Assets() noexcept;

    const RenderAssetRegistry& Assets() const noexcept;

    bool RenderFrame(const FrameSnapshot& snapshot, Nullable<string*> reason = nullptr) noexcept;

    void Resize(uint32_t width, uint32_t height) noexcept;

    void WaitIdle() noexcept;

    void SetCaptureEnabled(bool enabled) noexcept;

    std::optional<uint32_t> ReadCapturedPixel(uint32_t x, uint32_t y, Nullable<string*> reason = nullptr) const noexcept;

private:
    RendererRuntime() noexcept;
    bool InitializeImpl(const RendererRuntimeCreateDesc& desc, Nullable<string*> reason = nullptr) noexcept;

    class Impl;

    unique_ptr<Impl> _impl{};
};

}  // namespace radray::runtime
