#pragma once

#include <optional>
#include <limits>

#include <radray/types.h>
#include <radray/nullable.h>
#include <radray/render/common.h>

namespace radray {

class GpuRuntime;
class GpuSubmitContext;
class GpuPresentSurface;
class GpuTask;

struct GpuRuntimeDescriptor {
    render::RenderBackend Backend{render::RenderBackend::D3D12};
    bool EnableDebugValidation{false};
};

// ---------------------------------------------------------------------------
// GpuPresentSurfaceDescriptor
// 注意：这里只保留平台原生窗口句柄，不直接依赖 window 模块类型。
// 调用方必须保证句柄在 surface 生命周期内持续有效。
// ---------------------------------------------------------------------------
struct GpuPresentSurfaceDescriptor {
    const void* NativeWindowHandle{nullptr};
    uint32_t Width{0};
    uint32_t Height{0};
    uint32_t BackBufferCount{3};
    uint32_t FlightFrameCount{2};
    render::TextureFormat Format{render::TextureFormat::BGRA8_UNORM};
    render::PresentMode PresentMode{render::PresentMode::FIFO};
};

struct GpuResourceCreationInfo {
    // Runtime 层分配的稳定资源标识。
    // - 由 GpuRuntime 在创建资源时生成
    // - 调用方不应自行构造或修改
    // - 生命周期语义由 runtime 决定；当资源失效后，该 Handle 也随之失效
    uint64_t Handle;

    // 后端 RHI 层对象的穿透指针。
    // - 该指针不转移所有权，只是一个 borrowed handle
    // - 具体指向什么对象由后端自行定义，例如某个 render 后端里的 Buffer/Texture 实现对象
    // - 其生命周期与 Handle 一致；当资源失效后，该指针也随之失效
    // - 调用方不应假设不同后端下它具有统一的具体类型
    void* NativeHandle;

    constexpr auto IsValid() const noexcept { return Handle != std::numeric_limits<uint64_t>::max(); }

    constexpr void Invalidate() noexcept {
        Handle = std::numeric_limits<uint64_t>::max();
        NativeHandle = nullptr;
    }

    constexpr static GpuResourceCreationInfo Invalid() noexcept {
        return {std::numeric_limits<uint64_t>::max(), nullptr};
    }
};

struct GpuBufferCreationInfo : public GpuResourceCreationInfo {
    size_t ElementStride;
    size_t SizeInBytes;

    constexpr static GpuBufferCreationInfo Invalid() noexcept {
        return {GpuResourceCreationInfo::Invalid(), 0, 0};
    }
};

class GpuBufferView {
public:
private:
    uint64_t _handle;
    void* _nativeHandle;
    size_t _offset;
    size_t _sizeInBytes;
};

// ---------------------------------------------------------------------------
// GpuPresentSurface
// 管理 swapchain / present surface。对象由 GpuRuntime 创建并拥有底层 GPU 对象。
// surface 自身可长期存在；Acquire/Present 语义附着在某个 GpuSubmitContext 上。
// 也就是说，present 不是独立提交，而是“当前提交批次”的一部分。
// ---------------------------------------------------------------------------
class GpuPresentSurface {
public:
    bool IsValid() const noexcept;
    void Destroy() noexcept;

    uint32_t GetWidth() const noexcept;
    uint32_t GetHeight() const noexcept;
    render::TextureFormat GetFormat() const noexcept;

    // 阻塞直到拿到可用 back buffer，或 surface 进入不可恢复状态。
    // Acquire 的结果挂入 ctx，作为本次提交批次的 present 输入。
    bool Acquire(GpuSubmitContext& ctx) noexcept;

    // 非阻塞尝试获取。若当前没有可用帧槽，则立即失败。
    // 成功时同样把 acquire 结果挂入 ctx，而不是立即触发 present。
    bool TryAcquire(GpuSubmitContext& ctx) noexcept;

    // 向 ctx 注册“本次提交完成后需要 present 此 surface”的意图。
    // 真正的 Present 发生在 runtime->Submit(ctx) 执行该批次之后。
    void Present(GpuSubmitContext& ctx) noexcept;

private:
    GpuRuntime* _runtime{nullptr};

    friend class GpuRuntime;
};

// ---------------------------------------------------------------------------
// GpuTask
// 表示一次已经提交到 GPU 队列上的执行批次。
// 它不是 command buffer，也不是资源；它是一个通用的“任务 / 依赖令牌”概念。
//
// 语义约束：
// - 一个 GpuTask 对应一次 GpuRuntime::Submit(ctx)
// - 一个 GpuTask 只对应一个队列上的一次提交
// - 一个 GpuTask 表示“该提交批次执行完成”，不额外承诺 scanout / 显示完成
// ---------------------------------------------------------------------------
class GpuTask {
public:
    bool IsValid() const noexcept;

    bool IsCompleted() const noexcept;

    void Wait() noexcept;

private:
    friend class GpuRuntime;
};

// ---------------------------------------------------------------------------
// GpuSubmitContext
// 表示一次 GPU 提交批次的 CPU 侧构建上下文。
//
// 语义约束：
// - 一个 GpuSubmitContext 只对应一次 Submit()
// - 一个 GpuSubmitContext 只绑定一个具体队列
// - 一个 GpuSubmitContext 可以聚合多条命令录制结果，统一提交到该队列
// - GpuSubmitContext 本身不是 command buffer，而是“提交批次”
// - Submit() 后该对象即被消费，不能复用
// ---------------------------------------------------------------------------
class GpuSubmitContext {
public:
    GpuBufferView CreateTempBuffer(uint64_t sizeInBytes) noexcept;

private:
    GpuRuntime* _runtime{nullptr};

    friend class GpuRuntime;
    friend class GpuPresentSurface;
};

// ---------------------------------------------------------------------------
// GpuRuntime
// 负责创建提交上下文、surface，以及把一次提交批次转换成一个 GpuTask。
// ---------------------------------------------------------------------------
class GpuRuntime {
public:
    bool IsValid() const noexcept;

    void Destroy() noexcept;

    Nullable<unique_ptr<GpuPresentSurface>> CreatePresentSurface(const GpuPresentSurfaceDescriptor& desc) noexcept;

    GpuBufferCreationInfo CreateBuffer(const render::BufferDescriptor& desc) noexcept;

    // 为指定队列开始构建一次新的提交批次。
    // 返回的 ctx 语义上绑定该队列，后续 Submit(ctx) 也只会提交到该队列。
    Nullable<unique_ptr<GpuSubmitContext>> BeginSubmission(render::QueueType type) noexcept;

    // 提交并消费 ctx。
    // 返回的 GpuTask 表示“这整个提交批次”的执行状态，而不是某条单独命令的状态。
    GpuTask Submit(unique_ptr<GpuSubmitContext> ctx) noexcept;

    static Nullable<unique_ptr<GpuRuntime>> Create(const GpuRuntimeDescriptor& desc) noexcept;
};

}  // namespace radray
