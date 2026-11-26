#pragma once

#include <cstdint>
#include <mutex>
#include <utility>

// GPU 资源所有权、上传与持久状态契约见 docs/architecture/runtime.md。

#include <radray/gpu/types.h>
#include <radray/nullable.h>
#include <radray/types.h>
#include <radray/intrusive_ptr.h>

namespace radray {

class RDGBuilder;
class RDGBufferHandle;
class RDGTextureHandle;

enum class GpuBufferMemory : uint8_t {
    DeviceLocal,
    DeviceUploadWithFallback,
    DeviceUploadStrict,
    HostUpload,
};

bool CanAllocateDeviceUpload(const gpu::Device& device, uint64_t size) noexcept;

struct GpuBufferDescriptor {
    uint64_t Size{0};
    gpu::BufferUses Usage{gpu::BufferUse::Resource};
    GpuBufferMemory Memory{GpuBufferMemory::DeviceLocal};
};

class GpuBuffer final : public ThreadSafeRefCountedBase<GpuBuffer> {
public:
    // device 须晚于缓冲销毁。
    // 调用方负责持有缓冲直到全部 GPU 使用和命令存储引用结束。
    static Nullable<IntrusivePtr<GpuBuffer>> Create(gpu::Device& device, const GpuBufferDescriptor& desc);
    ~GpuBuffer() noexcept override;

    gpu::Buffer* GetBuffer() const noexcept;
    const GpuBufferDescriptor& GetDesc() const noexcept;
    // 已提交工作的整资源终态；首次提交前为创建初态，不表示 GPU 已完成或内容已初始化。
    gpu::BufferStates GetSubmittedState() const noexcept;
    // 仅更新元数据；调用方须在实际提交后按队列顺序发布与命令一致的终态，不发出屏障或等待。
    void CommitSubmittedState(gpu::BufferStates state) noexcept;

    bool IsHostVisible() const noexcept;
    gpu::BufferMemoryProperties GetMemoryProperties() const noexcept;
    Nullable<byte*> GetMappedData() const noexcept;
    uint64_t GetSliceAlignment() const noexcept;
    // Returns zero on overflow. Reusing a slice requires GPU completion.
    uint64_t AlignSliceSize(uint64_t size) const noexcept;
    // The submitting thread flushes after all writers have finished.
    bool FlushMappedRange(gpu::BufferRange range) noexcept;

private:
    GpuBuffer(const GpuBufferDescriptor& desc, unique_ptr<gpu::Buffer> buffer, Nullable<byte*> mapped, uint64_t alignment) noexcept;
    GpuBufferDescriptor _desc;
    unique_ptr<gpu::Buffer> _buffer;
    Nullable<byte*> _mapped;
    uint64_t _alignment;
    gpu::BufferStates _submittedState;
};

struct GpuTextureDescriptor {
    gpu::TextureDimension Dim{gpu::TextureDimension::Dim2D};
    uint32_t Width{0}, Height{0};
    uint32_t ArrayLayers{1}, MipLevels{1};
    gpu::TextureFormat Format{gpu::TextureFormat::UNKNOWN};
    gpu::TextureUses Usage{gpu::TextureUse::UNKNOWN};
};

struct GpuTextureViewKey {
    gpu::TextureDimension Dim{gpu::TextureDimension::UNKNOWN};
    gpu::TextureFormat Format{gpu::TextureFormat::UNKNOWN};
    gpu::SubresourceRange Range{gpu::SubresourceRange::AllSub()};
    gpu::TextureViewUsage Usage{gpu::TextureViewUsage::Resource};

    friend bool operator==(const GpuTextureViewKey&, const GpuTextureViewKey&) noexcept = default;
};

class GpuTexture final : public ThreadSafeRefCountedBase<GpuTexture> {
public:
    // device 必须晚于纹理销毁；调用方负责持有纹理直到全部 GPU 使用和命令存储引用结束。
    static Nullable<IntrusivePtr<GpuTexture>> Create(gpu::Device& device, const GpuTextureDescriptor& desc);
    ~GpuTexture() noexcept override;

    gpu::Texture* GetTexture() const noexcept;
    Nullable<gpu::TextureView*> GetDefaultView() const noexcept;
    // Returned views are borrowed from this texture. Use the device that created it.
    Nullable<gpu::TextureView*> GetOrCreateView(gpu::Device& device, const GpuTextureViewKey& key);
    const GpuTextureDescriptor& GetDesc() const noexcept;
    // 已提交工作的整资源终态；首次提交前为创建初态，不表示 GPU 已完成或内容已初始化。
    gpu::TextureState GetSubmittedState() const noexcept;
    // 仅更新元数据；全部子资源须处于此交接状态，调用方在实际提交后按队列顺序发布。
    void CommitSubmittedState(gpu::TextureState state) noexcept;

private:
    GpuTexture(const GpuTextureDescriptor& desc, unique_ptr<gpu::Texture> texture, unique_ptr<gpu::TextureView> view) noexcept;

    GpuTextureDescriptor _desc;
    unique_ptr<gpu::Texture> _texture;
    unique_ptr<gpu::TextureView> _defaultView;
    std::mutex _viewMutex;
    vector<std::pair<GpuTextureViewKey, unique_ptr<gpu::TextureView>>> _viewCache;
    gpu::TextureState _submittedState{gpu::TextureState::Undefined};
};

// Borrowed 由外部保活到命令存储重置；Owned 在登记时去重持有 wrapper 引用。
// 设备缓冲跨图边界为 Common，可映射缓冲为 HostWrite；可采样纹理为 ShaderRead，其余纹理保持最后使用状态。
// 导入读取已提交初态快照；此前相关提交须已发布，后续不得插入使快照失效的提交。
RDGBufferHandle ImportPersistentBorrowed(RDGBuilder& graph, GpuBuffer& buffer);
RDGTextureHandle ImportPersistentBorrowed(RDGBuilder& graph, GpuTexture& texture);
RDGBufferHandle ImportPersistentOwned(RDGBuilder& graph, const IntrusivePtr<GpuBuffer>& buffer);
RDGTextureHandle ImportPersistentOwned(RDGBuilder& graph, const IntrusivePtr<GpuTexture>& texture);

}  // namespace radray
