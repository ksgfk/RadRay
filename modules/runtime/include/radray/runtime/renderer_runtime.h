#pragma once

#include <optional>

#include <radray/nullable.h>
#include <radray/render/common.h>
#include <radray/render/gpu_resource.h>
#include <radray/types.h>

#include <radray/runtime/frame_snapshot.h>
#include <radray/runtime/render_asset_registry.h>

namespace radray::runtime {

struct PreparedDrawItem {
    MeshHandle Mesh{};
    MaterialHandle Material{};
    uint32_t SubmeshIndex{0};
    Eigen::Matrix4f LocalToWorld{Eigen::Matrix4f::Identity()};
    Eigen::Vector4f Tint{Eigen::Vector4f::Ones()};
    uint32_t SortKeyHigh{0};
    uint32_t SortKeyLow{0};
};

struct PreparedView {
    uint32_t ViewId{0};
    uint32_t CameraId{0};
    uint32_t OutputWidth{0};
    uint32_t OutputHeight{0};
    vector<PreparedDrawItem> DrawItems{};
};

struct FrameInFlight {
    uint32_t FrameIndex{0};
    uint64_t ExpectedFenceValue{0};
};

struct RenderFrameContext {
    uint32_t FrameIndex{0};
    uint32_t BackBufferIndex{0};
    const FrameSnapshot* Snapshot{nullptr};
    vector<PreparedView> PreparedViews{};
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
    RendererRuntime() noexcept;
    ~RendererRuntime() noexcept;

    bool Initialize(const RendererRuntimeCreateDesc& desc, Nullable<string*> reason = nullptr) noexcept;

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
    class Impl;

    unique_ptr<Impl> _impl{};
};

}  // namespace radray::runtime
