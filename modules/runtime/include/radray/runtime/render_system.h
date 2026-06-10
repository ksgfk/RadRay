#pragma once

#include <chrono>
#include <optional>
#include <span>

#include <radray/types.h>
#include <radray/render/common.h>
#include <radray/render/gpu_resource.h>
#include <radray/runtime/asset_manager.h>

namespace radray {

class Application;
class AppWindowSystem;
class StaticMesh;

struct AppRenderSystemDescriptor {
    render::Device* Device;
    uint32_t MainQueueIndex;
    uint32_t BackBufferCount{3};
    uint32_t FlightDataCount{2};
};

class AppRenderSystem {
public:
    struct FenceSignal {
        render::Fence* Fence{nullptr};
        uint64_t Value{0};

        static constexpr FenceSignal Invalid() noexcept { return FenceSignal{}; }

        constexpr bool IsValid() const noexcept { return Fence != nullptr; }
    };

    AppRenderSystem(Application* app, const AppRenderSystemDescriptor& desc);
    AppRenderSystem(const AppRenderSystem&) = delete;
    AppRenderSystem(AppRenderSystem&&) = delete;
    AppRenderSystem& operator=(const AppRenderSystem&) = delete;
    AppRenderSystem& operator=(AppRenderSystem&&) = delete;
    ~AppRenderSystem() noexcept;

    bool CompleteFlight(uint32_t flightIndex);
    void WaitAndCleanupCompletedFlights();

public:
    struct QueueFrameTrack {
        render::CommandQueue* Queue{nullptr};
        unique_ptr<render::Fence> Fence;
        uint64_t NextFenceValue{1};
    };

    struct FlightData {
        FenceSignal Signal;
        std::chrono::steady_clock::time_point FrameStartTime{};
        vector<unique_ptr<render::RenderBase>> WaitForDestroy;
    };

    Application* _app;
    AppWindowSystem* _windowSystem{nullptr};
    render::Device* _device;
    render::CommandQueue* _mainQueue;
    const uint32_t _backBufferCount;
    const uint32_t _flightDataCount;
    QueueFrameTrack _mainQueueTrack;
    uint64_t _nowFrameIndex{0};
    std::chrono::duration<float> _lastFrameLatency{};
    vector<FlightData> _flight;
    unique_ptr<class ResourceUploader> _uploader;
};

/// 描述一次 buffer 上传操作。
struct BufferUploadRequest {
    std::span<const byte> SrcData;
    render::Buffer* DstBuffer;
    uint64_t DstOffset{0};
    render::BufferStates Before{render::BufferState::Common};
    render::BufferStates After{render::BufferState::Common};
};

/// 描述一次 texture 上传操作。
struct TextureUploadRequest {
    std::span<const byte> SrcData;
    render::Texture* DstTexture;
    render::SubresourceRange DstRange;
    uint64_t SrcRowPitch{0};
    render::TextureStates Before{render::TextureState::Undefined};
    render::TextureStates After{render::TextureState::ShaderRead};
};

/// Upload heap staging buffer 池。按 flight index 管理回收。
class StagingBufferPool {
public:
    struct Allocation {
        render::Buffer* Buffer{nullptr};
        void* MappedPtr{nullptr};
        uint64_t Offset{0};
        uint64_t Size{0};
    };

    explicit StagingBufferPool(render::Device* device, uint32_t flightCount) noexcept;
    ~StagingBufferPool() noexcept;
    StagingBufferPool(const StagingBufferPool&) = delete;
    StagingBufferPool& operator=(const StagingBufferPool&) = delete;

    /// 从 Upload heap 分配一块 staging 内存。
    Allocation Allocate(uint64_t size);

    /// 刷新并解除 staging 内存映射。
    void FlushAndUnmap(const Allocation& allocation);

    /// 将当前所有活跃 staging buffer 移入指定 flight 的 pending 列表。
    void RetireToFlight(uint32_t flightIndex);

    /// 回收指定 flight 的 staging buffer 到 free list。
    void CollectFlight(uint32_t flightIndex);

private:
    struct ActiveBuffer {
        unique_ptr<render::Buffer> Buffer;
        bool IsMapped{false};
        uint64_t MappedSize{0};
    };

    render::Device* _device;
    vector<ActiveBuffer> _active;
    vector<vector<unique_ptr<render::Buffer>>> _pending;
    vector<unique_ptr<render::Buffer>> _freeList;
};

/// 资源上传器。录制 copy 命令到外部传入的 CommandBuffer，
/// 管理 staging 生命周期，持有 AssetRef 保活。
///
/// 使用流程：
/// 1. 调用方 cmdBuffer->Begin()
/// 2. 调用 UploadBuffer / UploadTexture（可多次）
/// 3. 调用 EndFlight(flightIndex) 将 staging + AssetRef 绑定到该 flight
/// 4. 调用方 cmdBuffer->End() → Submit → signal fence
/// 5. CompleteFlight 后调用 CollectFlight(flightIndex) 回收
class ResourceUploader {
public:
    ResourceUploader(render::Device* device, uint32_t flightCount);
    ~ResourceUploader() noexcept;
    ResourceUploader(const ResourceUploader&) = delete;
    ResourceUploader& operator=(const ResourceUploader&) = delete;

    /// 录制 buffer 上传命令到 cmdBuffer。
    void UploadBuffer(
        render::CommandBuffer* cmdBuffer,
        const BufferUploadRequest& request,
        AssetRefAny assetRef = nullptr);

    /// 录制 texture 上传命令到 cmdBuffer。
    void UploadTexture(
        render::CommandBuffer* cmdBuffer,
        const TextureUploadRequest& request,
        AssetRefAny assetRef = nullptr);

    /// 创建 StaticMesh 的 device-local buffer，并录制上传命令。
    std::optional<render::RenderMesh> UploadMesh(
        render::CommandBuffer* cmdBuffer,
        StaticMesh* mesh,
        AssetRefAny assetRef = nullptr);

    /// 帧末：将本帧 staging 和 AssetRef 移入指定 flight 等待 GPU 完成。
    void EndFlight(uint32_t flightIndex);

    /// CompleteFlight 后：回收 staging buffer，释放 AssetRef。
    void CollectFlight(uint32_t flightIndex);

private:
    render::Device* _device;
    StagingBufferPool _stagingPool;
    vector<AssetRefAny> _currentRefs;
    vector<vector<AssetRefAny>> _pendingRefs;
};

}  // namespace radray
