#pragma once

#include <limits>
#include <span>
#include <string_view>
#include <type_traits>

#include <radray/enum_flags.h>
#include <radray/runtime/gpu_system.h>

namespace radray {

struct RGHandle {
    uint32_t Id{0};

    constexpr bool IsValid() const noexcept { return Id != 0; }

    constexpr void Invalidate() noexcept { Id = 0; }

    constexpr static RGHandle Invalid() noexcept { return RGHandle{0}; }

    constexpr auto operator<=>(const RGHandle& other) const noexcept = default;
};

struct RGTextureHandle : public RGHandle {};
struct RGBufferHandle : public RGHandle {};
struct RGPassHandle : public RGHandle {};

enum class RGTextureAspect : uint8_t {
    Auto = 0,
    Color = 1 << 0,
    Depth = 1 << 1,
    Stencil = 1 << 2,
};

enum class RGStage : uint32_t {
    None = 0,
    DrawIndirect = 1 << 0,
    VertexInput = 1 << 1,
    VertexShader = 1 << 2,
    PixelShader = 1 << 3,
    EarlyDepthTest = 1 << 4,
    LateDepthTest = 1 << 5,
    ColorAttachmentOutput = 1 << 6,
    ComputeShader = 1 << 7,
    Copy = 1 << 8,
    Resolve = 1 << 9,
    AccelerationStructureBuild = 1 << 10,
    RayTracingShader = 1 << 11,
    Present = 1 << 12,

    AllGraphics = DrawIndirect | VertexInput | VertexShader | PixelShader | EarlyDepthTest | LateDepthTest | ColorAttachmentOutput,
    AllShaders = VertexShader | PixelShader | ComputeShader | RayTracingShader,
};

enum class RGTextureAccess : uint32_t {
    None = 0,
    SampledRead = 1 << 0,
    InputAttachmentRead = 1 << 1,
    StorageRead = 1 << 2,
    StorageWrite = 1 << 3,
    ColorAttachmentRead = 1 << 4,
    ColorAttachmentWrite = 1 << 5,
    DepthStencilRead = 1 << 6,
    DepthStencilWrite = 1 << 7,
    CopyRead = 1 << 8,
    CopyWrite = 1 << 9,
    ResolveRead = 1 << 10,
    ResolveWrite = 1 << 11,
    Present = 1 << 12,
};

enum class RGBufferAccess : uint32_t {
    None = 0,
    ConstantRead = 1 << 0,
    VertexRead = 1 << 1,
    IndexRead = 1 << 2,
    IndirectRead = 1 << 3,
    ShaderRead = 1 << 4,
    ShaderWrite = 1 << 5,
    CopyRead = 1 << 6,
    CopyWrite = 1 << 7,
    AccelerationStructureRead = 1 << 8,
    AccelerationStructureBuildInputRead = 1 << 9,
    AccelerationStructureBuildScratchRead = 1 << 10,
    AccelerationStructureBuildScratchWrite = 1 << 11,
    ShaderTableRead = 1 << 12,
};

enum class RGPassFlag : uint32_t {
    None = 0,
    NeverCull = 1 << 0,
    NeverMerge = 1 << 1,
    NeverParallel = 1 << 2,
    AsyncCompute = 1 << 3,
};

template <>
struct is_flags<RGTextureAspect> : public std::true_type {};

template <>
struct is_flags<RGStage> : public std::true_type {};

template <>
struct is_flags<RGTextureAccess> : public std::true_type {};

template <>
struct is_flags<RGBufferAccess> : public std::true_type {};

template <>
struct is_flags<RGPassFlag> : public std::true_type {};

using RGTextureAspects = EnumFlags<RGTextureAspect>;
using RGStages = EnumFlags<RGStage>;
using RGTextureAccesses = EnumFlags<RGTextureAccess>;
using RGBufferAccesses = EnumFlags<RGBufferAccess>;
using RGPassFlags = EnumFlags<RGPassFlag>;

struct RGTextureSubresourceRange {
    static constexpr uint32_t All = std::numeric_limits<uint32_t>::max();

    uint32_t BaseArrayLayer{0};
    uint32_t ArrayLayerCount{All};
    uint32_t BaseMipLevel{0};
    uint32_t MipLevelCount{All};
    RGTextureAspects Aspects{};

    static constexpr RGTextureSubresourceRange AllSub() noexcept {
        return RGTextureSubresourceRange{};
    }

    constexpr render::SubresourceRange NativeRange() const noexcept {
        return render::SubresourceRange{
            BaseArrayLayer,
            ArrayLayerCount,
            BaseMipLevel,
            MipLevelCount,
        };
    }
};

struct RGBufferSlice {
    static constexpr uint64_t All = std::numeric_limits<uint64_t>::max();

    uint64_t Offset{0};
    uint64_t Size{All};

    static constexpr RGBufferSlice Whole() noexcept {
        return RGBufferSlice{};
    }

    constexpr render::BufferRange NativeRange() const noexcept {
        return render::BufferRange{
            Offset,
            Size,
        };
    }
};

struct RGTextureBindingView {
    RGTextureSubresourceRange Range{RGTextureSubresourceRange::AllSub()};
};

struct RGBufferBindingView {
    RGBufferSlice Range{RGBufferSlice::Whole()};
    uint32_t Stride{0};
    render::TextureFormat Format{render::TextureFormat::UNKNOWN};
};

struct RGTextureUseDesc {
    RGTextureAccesses Access{RGTextureAccess::SampledRead};
    RGStages Stages{};
    RGTextureBindingView View{};
};

struct RGBufferUseDesc {
    RGBufferAccesses Access{RGBufferAccess::ShaderRead};
    RGStages Stages{};
    RGBufferBindingView View{};
};

struct RGTextureBinding {
    RGTextureHandle Handle{};
    RGTextureBindingView View{};
};

struct RGBufferBinding {
    RGBufferHandle Handle{};
    RGBufferBindingView View{};
};

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
    render::TextureState InitialState{render::TextureState::UNKNOWN};
    RGTextureBindingView InitialView{};
};

struct RGImportedBufferDesc {
    GpuBufferHandle Buffer{};
    render::BufferDescriptor NativeDesc{};
    render::BufferState InitialState{render::BufferState::UNKNOWN};
    RGBufferBindingView InitialView{};
};

struct RGColorAttachmentOps {
    render::LoadAction Load{render::LoadAction::Load};
    render::StoreAction Store{render::StoreAction::Store};
    render::ColorClearValue ClearValue{};
};

struct RGDepthAttachmentOps {
    render::LoadAction DepthLoad{render::LoadAction::Load};
    render::StoreAction DepthStore{render::StoreAction::Store};
    render::LoadAction StencilLoad{render::LoadAction::Load};
    render::StoreAction StencilStore{render::StoreAction::Store};
    render::DepthStencilClearValue ClearValue{1.0f, 0};
    bool ReadOnlyDepth{false};
    bool ReadOnlyStencil{false};
};

struct RGColorAttachmentDesc {
    RGTextureHandle Handle{};
    RGTextureBindingView View{};
    RGColorAttachmentOps Ops{};
};

struct RGDepthAttachmentDesc {
    RGTextureHandle Handle{};
    RGTextureBindingView View{};
    RGDepthAttachmentOps Ops{};
};

struct RGResolveTargetDesc {
    RGTextureHandle Handle{};
    RGTextureBindingView View{};
};

struct RGCopyTextureDesc {
    RGTextureHandle Src{};
    RGTextureBindingView SrcView{};
    RGTextureHandle Dst{};
    RGTextureBindingView DstView{};
};

struct RGCopyBufferDesc {
    RGBufferHandle Src{};
    RGBufferSlice SrcRange{RGBufferSlice::Whole()};
    RGBufferHandle Dst{};
    RGBufferSlice DstRange{RGBufferSlice::Whole()};
};

struct RGCopyBufferToTextureDesc {
    RGBufferHandle Src{};
    RGBufferSlice SrcRange{RGBufferSlice::Whole()};
    RGTextureHandle Dst{};
    RGTextureBindingView DstView{};
};

struct RGCopyTextureToBufferDesc {
    RGTextureHandle Src{};
    RGTextureBindingView SrcView{};
    RGBufferHandle Dst{};
    RGBufferSlice DstRange{RGBufferSlice::Whole()};
};

struct RGResolveTextureDesc {
    RGTextureHandle Src{};
    RGTextureBindingView SrcView{};
    RGTextureHandle Dst{};
    RGTextureBindingView DstView{};
};

class RGPassContextBase {
public:
    GpuAsyncContext& RuntimeContext() noexcept;

    render::Texture* GetTexture(RGTextureHandle handle) noexcept;
    render::Buffer* GetBuffer(RGBufferHandle handle) noexcept;
    render::TextureView* GetTextureView(const RGTextureBinding& binding) noexcept;
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
using RGRasterShaderExecuteFn = void (*)(RGRasterShaderPassContext& ctx, const PassData& passData);

template <typename PassData>
using RGComputeShaderExecuteFn = void (*)(RGComputeShaderPassContext& ctx, const PassData& passData);

class RGRasterAttachmentBuilderBase {
public:
    void SetColorAttachment(uint32_t slot, const RGColorAttachmentDesc& desc);
    void SetDepthAttachment(const RGDepthAttachmentDesc& desc);
    void SetResolveTarget(uint32_t slot, const RGResolveTargetDesc& desc);
};

class RGPassBuilderBase {
public:
    RGTextureHandle UseTexture(RGTextureHandle handle, const RGTextureUseDesc& desc);
    RGBufferHandle UseBuffer(RGBufferHandle handle, const RGBufferUseDesc& desc);

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

    void Bind(std::string_view slotName, const RGTextureBinding& binding);
    void Bind(std::string_view slotName, const RGBufferBinding& binding);
    void BindArray(std::string_view slotName, std::span<const RGTextureBinding> bindings);
    void BindArray(std::string_view slotName, std::span<const RGBufferBinding> bindings);
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
    void CopyTexture(const RGCopyTextureDesc& desc);
    void CopyBuffer(const RGCopyBufferDesc& desc);
    void CopyBufferToTexture(const RGCopyBufferToTextureDesc& desc);
    void CopyTextureToBuffer(const RGCopyTextureToBufferDesc& desc);
    void ResolveTexture(const RGResolveTextureDesc& desc);

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

    void ExtractTexture(RGTextureHandle handle, GpuTextureHandle* outTexture, const RGTextureBindingView& view, render::TextureState finalState);
    void ExtractBuffer(RGBufferHandle handle, GpuBufferHandle* outBuffer, const RGBufferBindingView& view, render::BufferState finalState);

    void SetTextureAccessFinal(RGTextureHandle handle, const RGTextureBindingView& view, render::TextureState access);
    void SetBufferAccessFinal(RGBufferHandle handle, const RGBufferBindingView& view, render::BufferState access);

    void UseExternalAccessMode(RGTextureHandle handle, const RGTextureBindingView& view, render::TextureState readOnlyAccess);
    void UseExternalAccessMode(RGBufferHandle handle, const RGBufferBindingView& view, render::BufferState readOnlyAccess);

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
