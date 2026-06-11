#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <span>

#include <radray/types.h>
#include <radray/render/common.h>
#include <radray/render/gpu_resource.h>
#include <radray/runtime/asset_manager.h>

namespace radray::render {
class Dxc;
}

namespace radray {

class Application;
class AppWindow;
class AppWindowSystem;
class AppFrameContext;
class ResourceUploader;
class StaticMesh;
class Material;

/// 一次 shader 编译请求。Name 作为缓存身份(同一逻辑 shader 的不同入口共享 Name)。
/// 对应 UE5 FShaderType + 编译环境的极简化:这里只承载 HLSL 源 + 入口 + 阶段。
struct ShaderCompileDescriptor {
    std::string_view Name{};
    std::string_view Source{};
    std::string_view EntryPoint{};
    render::ShaderStage Stage{render::ShaderStage::UNKNOWN};
};

struct AppRenderSystemDescriptor {
    render::Device* Device;
    uint32_t MainQueueIndex;
    uint32_t BackBufferCount{3};
    uint32_t FlightDataCount{2};
};

/// 图形 PSO 缓存。对应 UE5 中 Material×VertexFactory×RenderTarget 才确定一个
/// 完整 shader 实例(PSO)的概念:同一 Material 用于不同顶点布局或不同 RT 格式时
/// 需要不同 PSO,故按这三者组合做 key 缓存。
///
/// 渲染目标格式由调用方提供(BasePass 用 backbuffer + depth 格式)。
class PSOCache {
public:
    explicit PSOCache(render::Device* device) noexcept : _device(device) {}
    PSOCache(const PSOCache&) = delete;
    PSOCache& operator=(const PSOCache&) = delete;
    ~PSOCache() noexcept = default;

    /// RT 格式描述:一组 color 格式 + 一个 depth 格式(UNKNOWN 表示无 depth)。
    struct RenderTargetFormats {
        vector<render::TextureFormat> ColorFormats{};
        render::TextureFormat DepthFormat{render::TextureFormat::UNKNOWN};
    };

    /// 取或建 PSO。material 提供 shader/rootsig/渲染状态,vertexLayout 提供 input layout,
    /// rtFormats 提供 RT 格式。命中缓存直接返回,返回的指针由 PSOCache 拥有。
    render::GraphicsPipelineState* GetOrCreate(
        const Material& material,
        const render::VertexBufferLayout& vertexLayout,
        const RenderTargetFormats& rtFormats);

    void Clear() noexcept { _cache.clear(); }

private:
    render::Device* _device;
    unordered_map<string, unique_ptr<render::GraphicsPipelineState>> _cache;
};

/// AcquireWindow 成功返回的轻量视图。重量级的 SwapChainFrame / sync object
/// 留在 runtime 的 per-flight FlightSlot 里，应用只拿到 backbuffer + view。
/// 【不暴露同步对象】sync object 是提交细节，由 runtime 独占。
struct AppFrameTarget {
    AppWindow* Window{nullptr};
    render::Texture* BackBuffer{nullptr};
    render::TextureView* BackBufferView{nullptr};
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

    /// 一帧开头：取/建该 flight 的 CommandBuffer 并 Begin()，清空上帧 acquire 的目标。
    /// 返回供应用在 Render 中使用的帧上下文。
    AppFrameContext BeginFrameRecord(
        uint32_t flightIndex,
        std::chrono::duration<float> deltaTime,
        std::chrono::duration<float> lastFrameLatency,
        bool isInModalLoop);

    /// 一帧收尾：uploader.EndFlight → CmdBuffer.End → 聚合 sync object → Submit
    /// → 写 flight.Signal → Present 全部 target。ManualSubmit 下仅 End，跳过提交/呈现。
    void EndFrameRecordAndSubmit(uint32_t flightIndex);

    /// 编译（含反射）并创建一个 Shader，按 Name|EntryPoint|Stage|Backend 缓存。
    /// 命中缓存直接返回。返回的指针由 AppRenderSystem 拥有，调用方勿释放。
    /// Vulkan 后端自动编译为 SPIR-V 并走 spirv-cross 反射，D3D12 走 DXIL。
    /// 对应 UE5 FShaderMap 对 FShader 的缓存职责（最小化版，未引入独立 ShaderManager）。
    Nullable<render::Shader*> GetOrCompileShader(const ShaderCompileDescriptor& desc);

    /// 从磁盘读取 HLSL 文件并编译。Name 缺省用文件路径。
    Nullable<render::Shader*> GetOrCompileShaderFromFile(
        const std::filesystem::path& path,
        std::string_view entryPoint,
        render::ShaderStage stage,
        std::string_view name = {});

    /// 取或建 RootSignature。【按 binding layout 共享】: RootSignature 由参与的 shader
    /// 反射出的绑定布局决定，同一组 shader 共享一个 RootSignature，不再每个
    /// Material 独立创建。返回的指针由 AppRenderSystem 拥有。
    /// 对应 UE5 中 RootSignature 按 shader 绑定布局去重共享的思路。
    Nullable<render::RootSignature*> GetOrCreateRootSignature(std::span<render::Shader*> shaders);

    /// 引擎级 PSO 缓存。按 (Material, 顶点签名, RT 格式) 组合缓存 GraphicsPipelineState。
    PSOCache& GetPSOCache() noexcept { return *_psoCache; }

public:
    struct QueueFrameTrack {
        render::CommandQueue* Queue{nullptr};
        unique_ptr<render::Fence> Fence;
        uint64_t NextFenceValue{1};
    };

    /// runtime 拥有的 per-flight 槽位。代表流水线一条槽位在不同阶段的完整状态，
    /// 按所有权/阶段分三组，跨阶段的访问时序由 runner 的信号量 + retire 锁保证：
    ///  - 录制态：渲染线程（单线程模式即主线程）在 BeginFrameRecord→Render→
    ///    EndFrameRecordAndSubmit 期间独占；
    ///  - 计时态：游戏线程在帧开头写 FrameStartTime / 清 WaitForDestroy；
    ///  - 提交态:Signal 由 EndFrameRecordAndSubmit 写、retire/CompleteFlight 读后清。
    struct FlightSlot {
        struct AcquiredTarget {
            AppWindow* Window{nullptr};
            render::SwapChainFrame Frame;
        };

        // —— 录制态（渲染线程独占）。CmdBuffer 池化复用，
        //    Targets 收集本帧 acquire 的全部窗口以支持多窗口/多 viewport。
        unique_ptr<render::CommandBuffer> CmdBuffer;
        vector<AcquiredTarget> Targets;
        bool ManualSubmit{false};
        bool Recording{false};

        // —— 计时态（游戏线程写）。
        std::chrono::steady_clock::time_point FrameStartTime{};
        vector<unique_ptr<render::RenderBase>> WaitForDestroy;

        // —— 提交态（渲染线程写，retire 经 _retireMutex 读后清）。
        FenceSignal Signal;
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
    vector<FlightSlot> _flights;
    unique_ptr<class ResourceUploader> _uploader;

    // —— Shader 编译与缓存(惰性创建 Dxc)。key = "Name|Entry|Stage|Backend"。
    shared_ptr<render::Dxc> _dxc;
    unordered_map<string, unique_ptr<render::Shader>> _shaderCache;
    // —— RootSignature 缓存。按参与 shader 集合(决定绑定布局)去重共享。
    unordered_map<string, unique_ptr<render::RootSignature>> _rootSigCache;
    unique_ptr<PSOCache> _psoCache;
};

/// Render 回调的唯一入参，封装一帧录制 API。
/// 生命周期仅限本次 Render 调用；runtime 在 BeginFrameRecord 构造并传入。
class AppFrameContext {
public:
    AppFrameContext(
        AppRenderSystem* renderSystem,
        uint32_t flightIndex,
        std::chrono::duration<float> deltaTime,
        std::chrono::duration<float> lastFrameLatency,
        bool isInModalLoop) noexcept
        : _renderSystem(renderSystem),
          _flightIndex(flightIndex),
          _deltaTime(deltaTime),
          _lastFrameLatency(lastFrameLatency),
          _isInModalLoop(isInModalLoop) {}

    uint32_t FlightIndex() const noexcept { return _flightIndex; }
    std::chrono::duration<float> DeltaTime() const noexcept { return _deltaTime; }
    std::chrono::duration<float> LastFrameLatency() const noexcept { return _lastFrameLatency; }
    bool IsInModalLoop() const noexcept { return _isInModalLoop; }

    /// runtime 已 Begin() 的主 command buffer，应用所有录制（含 backbuffer barrier）的落点。
    render::CommandBuffer* GetCommandBuffer() const noexcept;

    /// 按需获取窗口呈现目标。内部 AcquireNextSwapChainFrame：
    /// RequireRecreate/RetryLater/Error/最小化 → nullopt（应用跳过该窗口）。
    /// 成功时把 SwapChainFrame 收进本帧 FlightSlot，返回 backbuffer + view。
    /// 【不录任何 barrier】backbuffer 初始翻转与 →Present 收尾全部由应用显式录。
    std::optional<AppFrameTarget> AcquireWindow(AppWindow* window);

    /// 绑定当前 flight 的上传器；EndFlight/CollectFlight 完全由 runtime 掌管。
    ResourceUploader& GetUploader() const noexcept;

    /// 逃生舱：直接拿底层对象自行处理（建资源、自定义 compute、readback、甚至自行 submit）。
    render::Device* GetDevice() const noexcept;
    render::CommandQueue* GetMainQueue() const noexcept;
    AppRenderSystem* GetRenderSystem() const noexcept { return _renderSystem; }

    /// 声明本帧应用自管提交，runtime 跳过 Submit/Present（仍 End cmd buffer 与回收 flight）。
    void SetManualSubmit() noexcept;

private:
    AppRenderSystem* _renderSystem;
    uint32_t _flightIndex;
    std::chrono::duration<float> _deltaTime;
    std::chrono::duration<float> _lastFrameLatency;
    bool _isInModalLoop;
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
