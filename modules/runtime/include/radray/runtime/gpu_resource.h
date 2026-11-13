#pragma once

// GPU 资源所有权、上传与持久状态契约见 docs/architecture/runtime.md。

#include <radray/intrusive_ptr.h>
#include <radray/runtime/rdg.h>

namespace radray {

struct GpuBufferDescriptor {
    uint64_t Size{0};
    render::BufferUses Usage{render::BufferUse::Resource};
};

class GpuBuffer final : public RefCountedBase<GpuBuffer> {
public:
    // 固定 Device 内存，自动增加 CopyDestination；device 须晚于缓冲销毁。
    // 调用方负责持有缓冲直到全部 GPU 使用和命令存储引用结束。
    static Nullable<IntrusivePtr<GpuBuffer>> Create(render::Device& device, const GpuBufferDescriptor& desc);
    ~GpuBuffer() noexcept override;

    render::Buffer* GetBuffer() const noexcept;
    const GpuBufferDescriptor& GetDesc() const noexcept;
    // 已提交工作的整资源终态；首次提交前为创建初态，不表示 GPU 已完成或内容已初始化。
    render::BufferStates GetSubmittedState() const noexcept;
    // 仅更新元数据；调用方须在实际提交后按队列顺序发布与命令一致的终态，不发出屏障或等待。
    void CommitSubmittedState(render::BufferStates state) noexcept;

private:
    GpuBuffer(const GpuBufferDescriptor& desc, unique_ptr<render::Buffer> buffer) noexcept;
    GpuBufferDescriptor _desc;
    unique_ptr<render::Buffer> _buffer;
    render::BufferStates _submittedState{render::BufferState::Common};
};

struct GpuTextureDescriptor {
    render::TextureDimension Dim{render::TextureDimension::Dim2D};
    uint32_t Width{0}, Height{0};
    uint32_t ArrayLayers{1}, MipLevels{1};
    render::TextureFormat Format{render::TextureFormat::UNKNOWN};
};

class GpuTexture final : public RefCountedBase<GpuTexture> {
public:
    // device 必须晚于纹理销毁；调用方负责持有纹理直到全部 GPU 使用和命令存储引用结束。
    static Nullable<IntrusivePtr<GpuTexture>> Create(render::Device& device, const GpuTextureDescriptor& desc);
    ~GpuTexture() noexcept override;

    render::Texture* GetTexture() const noexcept;
    render::TextureView* GetDefaultView() const noexcept;
    const GpuTextureDescriptor& GetDesc() const noexcept;
    // 已提交工作的整资源终态；首次提交前为创建初态，不表示 GPU 已完成或内容已初始化。
    render::TextureState GetSubmittedState() const noexcept;
    // 仅更新元数据；全部子资源须处于此交接状态，调用方在实际提交后按队列顺序发布。
    void CommitSubmittedState(render::TextureState state) noexcept;

private:
    GpuTexture(const GpuTextureDescriptor& desc, unique_ptr<render::Texture> texture, unique_ptr<render::TextureView> view) noexcept;

    GpuTextureDescriptor _desc;
    unique_ptr<render::Texture> _texture;
    unique_ptr<render::TextureView> _defaultView;
    render::TextureState _submittedState{render::TextureState::Undefined};
};

// Borrowed 由外部保活到命令存储重置；Owned 在登记时去重持有 wrapper 引用。
// 缓冲跨图边界为 Common，纹理为 ShaderRead；读取区域须由上传或普通 Pass 初始化。
// 导入读取已提交初态快照；此前相关提交须已发布，后续不得插入使快照失效的提交。
RDGBufferHandle ImportPersistentBorrowed(RDGBuilder& graph, GpuBuffer& buffer);
RDGTextureHandle ImportPersistentBorrowed(RDGBuilder& graph, GpuTexture& texture);
RDGBufferHandle ImportPersistentOwned(RDGBuilder& graph, const IntrusivePtr<GpuBuffer>& buffer);
RDGTextureHandle ImportPersistentOwned(RDGBuilder& graph, const IntrusivePtr<GpuTexture>& texture);

}  // namespace radray
