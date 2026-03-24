#pragma once

#include <span>
#include <string_view>
#include <type_traits>

#include <radray/enum_flags.h>
#include <radray/runtime/gpu_system.h>

namespace radray {

struct RGTextureHandle {
    uint32_t Id{0};
};

struct RGBufferHandle {
    uint32_t Id{0};
};

struct RGPassHandle {
    uint32_t Id{0};
};

enum class RGAccess : uint16_t {
    Unknown = 0,
    Sampled,
    StorageRead,
    StorageReadWrite,
    ColorAttachment,
    DepthStencilRead,
    DepthStencilReadWrite,
    CopySource,
    CopyDest,
    Vertex,
    Index,
    Indirect,
    Present,
    AccelerationStructureRead,
};

enum class RGPassFlag : uint32_t {
    None = 0,
    NeverCull = 1 << 0,
    NeverMerge = 1 << 1,
    NeverParallel = 1 << 2,
    AsyncCompute = 1 << 3,
};

enum class RGLoadOp : uint8_t {
    Load,
    Clear,
    DontCare,
};

enum class RGStoreOp : uint8_t {
    Store,
    DontCare,
};

template <>
struct is_flags<RGPassFlag> : public std::true_type {};

using RGPassFlags = EnumFlags<RGPassFlag>;

struct RGTextureDesc {
    render::TextureDescriptor NativeDesc{};
    bool ForceNonTransient{false};
};

struct RGBufferDesc {
    render::BufferDescriptor NativeDesc{};
    bool ForceNonTransient{false};
};

struct RGImportedTextureDesc {
    GpuTextureHandle Texture{};
    GpuTextureViewHandle DefaultView{};
    render::TextureDescriptor NativeDesc{};
    RGAccess InitialAccess{RGAccess::Unknown};
};

struct RGImportedBufferDesc {
    GpuBufferHandle Buffer{};
    render::BufferDescriptor NativeDesc{};
    RGAccess InitialAccess{RGAccess::Unknown};
};

struct RGColorAttachmentOps {
    RGLoadOp Load{RGLoadOp::Load};
    RGStoreOp Store{RGStoreOp::Store};
    render::ColorClearValue ClearValue{};
};

struct RGDepthAttachmentOps {
    RGLoadOp DepthLoad{RGLoadOp::Load};
    RGStoreOp DepthStore{RGStoreOp::Store};
    RGLoadOp StencilLoad{RGLoadOp::Load};
    RGStoreOp StencilStore{RGStoreOp::Store};
    render::DepthStencilClearValue ClearValue{1.0f, 0};
    bool ReadOnlyDepth{false};
    bool ReadOnlyStencil{false};
};

struct RGTextureBindingView {
    render::SubresourceRange Range{render::SubresourceRange::AllSub()};
};

struct RGBufferBindingView {
    render::BufferRange Range{};
    uint32_t Stride{0};
    render::TextureFormat Format{render::TextureFormat::UNKNOWN};
};

class RGPassContextBase {
public:
    GpuAsyncContext& RuntimeContext() noexcept;

    render::Texture* GetTexture(RGTextureHandle handle) noexcept;
    render::TextureView* GetTextureView(RGTextureHandle handle) noexcept;
    render::Buffer* GetBuffer(RGBufferHandle handle) noexcept;
};

class RGShaderPassContextBase : public RGPassContextBase {
public:
    render::RootSignature* GetRootSignature() noexcept;
    const render::BindingLayout& GetBindingLayout() const noexcept;
    render::DescriptorSet* GetDescriptorSet(render::DescriptorSetIndex set) noexcept;
};

class RGRasterPassContext : public RGPassContextBase {
public:
    render::GraphicsCommandEncoder* Cmd() noexcept;
};

class RGComputePassContext : public RGPassContextBase {
public:
    render::ComputeCommandEncoder* Cmd() noexcept;
};

class RGCopyPassContext : public RGPassContextBase {
public:
    render::CommandBuffer* Cmd() noexcept;
};

class RGUnsafePassContext : public RGPassContextBase {
public:
    render::CommandBuffer* Cmd() noexcept;
};

class RGRasterShaderPassContext : public RGShaderPassContextBase {
public:
    render::GraphicsCommandEncoder* Cmd() noexcept;
};

class RGComputeShaderPassContext : public RGShaderPassContextBase {
public:
    render::ComputeCommandEncoder* Cmd() noexcept;
};

template <typename PassData>
using RGRasterExecuteFn = void (*)(RGRasterPassContext& ctx, const PassData& passData);

template <typename PassData>
using RGComputeExecuteFn = void (*)(RGComputePassContext& ctx, const PassData& passData);

template <typename PassData>
using RGCopyExecuteFn = void (*)(RGCopyPassContext& ctx, const PassData& passData);

template <typename PassData>
using RGUnsafeExecuteFn = void (*)(RGUnsafePassContext& ctx, const PassData& passData);

template <typename PassData>
using RGRasterShaderExecuteFn = void (*)(RGRasterShaderPassContext& ctx, const PassData& passData);

template <typename PassData>
using RGComputeShaderExecuteFn = void (*)(RGComputeShaderPassContext& ctx, const PassData& passData);

class RGRasterAttachmentBuilderBase {
public:
    void SetColorAttachment(uint32_t slot, RGTextureHandle handle, const RGColorAttachmentOps& ops = {});
    void SetDepthAttachment(RGTextureHandle handle, const RGDepthAttachmentOps& ops = {});
    void SetResolveTarget(uint32_t slot, RGTextureHandle handle);
};

class RGPassBuilderBase {
public:
    RGTextureHandle ReadTexture(RGTextureHandle handle);
    RGTextureHandle WriteTexture(RGTextureHandle handle);
    RGTextureHandle ReadWriteTexture(RGTextureHandle handle);

    RGBufferHandle ReadBuffer(RGBufferHandle handle);
    RGBufferHandle WriteBuffer(RGBufferHandle handle);
    RGBufferHandle ReadWriteBuffer(RGBufferHandle handle);

    RGBufferHandle ReadVertexBuffer(RGBufferHandle handle);
    RGBufferHandle ReadIndexBuffer(RGBufferHandle handle);
    RGBufferHandle ReadIndirectBuffer(RGBufferHandle handle);

    RGTextureHandle CreateTransientTexture(std::string_view name, const RGTextureDesc& desc);
    RGBufferHandle CreateTransientBuffer(std::string_view name, const RGBufferDesc& desc);

    void AllowPassCulling(bool value);
};

class RGShaderPassBuilderBase {
public:
    render::RootSignature* GetRootSignature() const noexcept;
    const render::BindingLayout& GetBindingLayout() const noexcept;

    RGTextureHandle CreateTransientTexture(std::string_view name, const RGTextureDesc& desc);
    RGBufferHandle CreateTransientBuffer(std::string_view name, const RGBufferDesc& desc);

    void AllowPassCulling(bool value);

    void Bind(std::string_view slotName, RGTextureHandle handle);
    void Bind(std::string_view slotName, RGTextureHandle handle, const RGTextureBindingView& view);
    void Bind(std::string_view slotName, RGBufferHandle handle);
    void Bind(std::string_view slotName, RGBufferHandle handle, const RGBufferBindingView& view);

    void BindArray(std::string_view slotName, std::span<const RGTextureHandle> handles);
    void BindArray(std::string_view slotName, std::span<const RGBufferHandle> handles);

    void BindSampler(std::string_view slotName, GpuSamplerHandle handle);
    void SetPushConstants(std::string_view slotName, const void* data, uint32_t size);
};

class RGRasterPassBuilder : public RGPassBuilderBase, public RGRasterAttachmentBuilderBase {
public:
    template <typename PassData, typename ExecuteFn>
    void SetExecute(ExecuteFn&& fn);
};

class RGComputePassBuilder : public RGPassBuilderBase {
public:
    template <typename PassData, typename ExecuteFn>
    void SetExecute(ExecuteFn&& fn);
};

class RGCopyPassBuilder : public RGPassBuilderBase {
public:
    void CopyTexture(RGTextureHandle src, RGTextureHandle dst);
    void CopyBuffer(RGBufferHandle src, RGBufferHandle dst);
    void CopyBufferToTexture(RGBufferHandle src, RGTextureHandle dst);
    void CopyTextureToBuffer(RGTextureHandle src, RGBufferHandle dst);
    void ResolveTexture(RGTextureHandle src, RGTextureHandle dst);

    template <typename PassData, typename ExecuteFn>
    void SetExecute(ExecuteFn&& fn);
};

class RGUnsafePassBuilder : public RGPassBuilderBase {
public:
    template <typename PassData, typename ExecuteFn>
    void SetExecute(ExecuteFn&& fn);
};

class RGRasterShaderPassBuilder : public RGShaderPassBuilderBase, public RGRasterAttachmentBuilderBase {
public:
    template <typename PassData, typename ExecuteFn>
    void SetExecute(ExecuteFn&& fn);
};

class RGComputeShaderPassBuilder : public RGShaderPassBuilderBase {
public:
    template <typename PassData, typename ExecuteFn>
    void SetExecute(ExecuteFn&& fn);
};

class RenderGraph {
public:
    void Reset() noexcept;

    RGTextureHandle ImportTexture(std::string_view name, const RGImportedTextureDesc& desc);
    RGBufferHandle ImportBuffer(std::string_view name, const RGImportedBufferDesc& desc);

    RGTextureHandle CreateTexture(std::string_view name, const RGTextureDesc& desc);
    RGBufferHandle CreateBuffer(std::string_view name, const RGBufferDesc& desc);

    void ExtractTexture(
        RGTextureHandle handle,
        GpuTextureHandle* outTexture,
        RGAccess finalAccess = RGAccess::Sampled);
    void ExtractBuffer(
        RGBufferHandle handle,
        GpuBufferHandle* outBuffer,
        RGAccess finalAccess = RGAccess::Sampled);

    void SetTextureAccessFinal(RGTextureHandle handle, RGAccess access);
    void SetBufferAccessFinal(RGBufferHandle handle, RGAccess access);

    void UseExternalAccessMode(RGTextureHandle handle, RGAccess readOnlyAccess);
    void UseExternalAccessMode(RGBufferHandle handle, RGAccess readOnlyAccess);

    void UseInternalAccessMode(RGTextureHandle handle);
    void UseInternalAccessMode(RGBufferHandle handle);

    template <typename PassData, typename SetupFn>
    RGPassHandle AddRasterPass(
        std::string_view name,
        SetupFn&& setup,
        RGPassFlags flags = RGPassFlag::None);

    template <typename PassData, typename SetupFn>
    RGPassHandle AddComputePass(
        std::string_view name,
        SetupFn&& setup,
        RGPassFlags flags = RGPassFlag::None);

    template <typename PassData, typename SetupFn>
    RGPassHandle AddCopyPass(
        std::string_view name,
        SetupFn&& setup,
        RGPassFlags flags = RGPassFlag::None);

    template <typename PassData, typename SetupFn>
    RGPassHandle AddUnsafePass(
        std::string_view name,
        SetupFn&& setup,
        RGPassFlags flags = RGPassFlag::NeverCull);

    template <typename PassData, typename SetupFn>
    RGPassHandle AddRasterShaderPass(
        std::string_view name,
        render::RootSignature* rootSignature,
        SetupFn&& setup,
        RGPassFlags flags = RGPassFlag::None);

    template <typename PassData, typename SetupFn>
    RGPassHandle AddComputeShaderPass(
        std::string_view name,
        render::RootSignature* rootSignature,
        SetupFn&& setup,
        RGPassFlags flags = RGPassFlag::None);

    bool Compile(Nullable<string*> reason = nullptr);
    bool Execute(GpuAsyncContext& context, Nullable<string*> reason = nullptr);
};

}  // namespace radray
