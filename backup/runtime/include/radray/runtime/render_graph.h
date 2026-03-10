#pragma once

#include <functional>
#include <optional>
#include <string_view>

#include <radray/nullable.h>
#include <radray/render/common.h>

#include <radray/runtime/persistent_resource_registry.h>

namespace radray::runtime {

struct RenderFrameContext;
class RenderGraphPassContext;

struct RgResourceHandle {
    uint32_t Id{0};

    constexpr bool IsValid() const noexcept { return Id != 0; }
    static constexpr RgResourceHandle Invalid() noexcept { return RgResourceHandle{}; }
    friend auto operator<=>(const RgResourceHandle& lhs, const RgResourceHandle& rhs) noexcept = default;
};

struct RgPassHandle {
    uint32_t Id{0};

    constexpr bool IsValid() const noexcept { return Id != 0; }
    static constexpr RgPassHandle Invalid() noexcept { return RgPassHandle{}; }
    friend auto operator<=>(const RgPassHandle& lhs, const RgPassHandle& rhs) noexcept = default;
};

enum class RgPassType : uint32_t {
    Raster = 0,
    Copy,
};

enum class RgResourceLifetime : uint32_t {
    External = 0,
    Transient,
    Persistent,
};

enum class RgResourceKind : uint32_t {
    Texture = 0,
    Buffer,
};

using RenderGraphExecuteFn = std::function<bool(RenderGraphPassContext&, Nullable<string*>)>;

struct ImportedTextureDesc {
    render::Texture* Texture{nullptr};
    render::TextureView* DefaultView{nullptr};
    render::TextureDescriptor Desc{};
    render::TextureState InitialState{render::TextureState::UNKNOWN};
};

struct ImportedBufferDesc {
    render::Buffer* Buffer{nullptr};
    render::BufferDescriptor Desc{};
    render::BufferState InitialState{render::BufferState::UNKNOWN};
};

class RenderGraphPassContext {
public:
    RenderFrameContext& Frame() noexcept;
    const RenderFrameContext& Frame() const noexcept;

    render::CommandBuffer* Cmd() noexcept;

    render::Texture* GetTexture(RgResourceHandle handle) noexcept;
    render::TextureView* GetTextureView(RgResourceHandle handle) noexcept;
    render::Buffer* GetBuffer(RgResourceHandle handle) noexcept;

private:
    friend class RenderGraph;
    class Impl;

    explicit RenderGraphPassContext(Impl* impl) noexcept;

    Impl* _impl{nullptr};
};

class PassBuilder {
public:
    RgResourceHandle ReadTexture(RgResourceHandle handle);
    RgResourceHandle WriteTexture(RgResourceHandle handle);
    RgResourceHandle ReadBuffer(RgResourceHandle handle);
    RgResourceHandle WriteBuffer(RgResourceHandle handle);
    void SetColorAttachment(RgResourceHandle handle);
    void SetPresentTarget(RgResourceHandle handle);
    void SetExecute(RenderGraphExecuteFn fn);

private:
    friend class RenderGraph;
    class Impl;

    explicit PassBuilder(Impl* impl) noexcept;

    Impl* _impl{nullptr};
};

class RenderGraph {
public:
    RenderGraph(render::Device* device = nullptr, PersistentResourceRegistry* persistentResources = nullptr) noexcept;
    ~RenderGraph() noexcept;

    void Reset(render::Device* device, PersistentResourceRegistry* persistentResources = nullptr) noexcept;

    RgResourceHandle ImportTexture(std::string_view name, const ImportedTextureDesc& desc);

    RgResourceHandle ImportBuffer(std::string_view name, const ImportedBufferDesc& desc);

    RgResourceHandle CreateTransientTexture(std::string_view name, const render::TextureDescriptor& desc);

    RgResourceHandle CreateTransientBuffer(std::string_view name, const render::BufferDescriptor& desc);

    RgResourceHandle ImportPersistentTexture(
        std::string_view name,
        PersistentTextureHandle handle,
        render::TextureState initialState = render::TextureState::Common);

    RgPassHandle AddRasterPass(std::string_view name, const std::function<void(PassBuilder&)>& setup);

    RgPassHandle AddCopyPass(std::string_view name, const std::function<void(PassBuilder&)>& setup);

    bool Compile(Nullable<string*> reason = nullptr) noexcept;

    bool Execute(RenderFrameContext& ctx, Nullable<string*> reason = nullptr) noexcept;

    std::span<const RgPassHandle> GetCompiledPassOrder() const noexcept;

    std::optional<render::TextureState> GetCompiledFinalTextureState(RgResourceHandle handle) const noexcept;

    std::optional<render::BufferState> GetCompiledFinalBufferState(RgResourceHandle handle) const noexcept;

private:
    class Impl;

    unique_ptr<Impl> _impl{};
};

}  // namespace radray::runtime
