#pragma once

#include <radray/allocator.h>
#include <radray/runtime/render_framework/binding_registry.h>

namespace radray {
/// Renderer-owned resource versions and immutable upload ranges. All operations require its safe point.
class RendererResources {
public:
    RendererResources(render::Device* device, Guid owner);
    ~RendererResources() noexcept;
    std::optional<ResourceId> ImportBuffer(render::Buffer* buffer);
    std::optional<ResourceId> CreateBuffer(const render::BufferDescriptor& descriptor);
    std::optional<ResourceId> ImportTexture(render::Texture* texture);
    /// The registry owns the native view; the imported texture owner waits for its exit confirmation.
    std::optional<ResourceId> CreateView(ResourceId texture, render::TextureViewDescriptor descriptor);
    std::optional<ResourceId> CreateBufferRange(ResourceId buffer, render::BufferRange range, uint32_t stride = 0);
    std::optional<ResourceId> CreateSampler(const render::SamplerDescriptor& descriptor);
    bool DependsOn(ResourceId resource, ResourceId ancestor) const noexcept;
    void RequestAllExits(BindingRegistry& bindings);
    /// Identical CPU bytes, usage and stride share one immutable allocation before descriptors are compiled.
    std::optional<ResourceId> StoreData(std::span<const byte> bytes, uint32_t stride = 0);
    bool RetainRegistration(ResourceId id);
    bool ReleaseRegistration(ResourceId id);
    bool RetainFrame(ResourceId id);
    bool ReleaseFrame(ResourceId id);
    bool RequestExit(ResourceId id, BindingRegistry& bindings);
    /// Returned ids confirm that all dependent views, blocks and admitted/GPU uses have ended.
    vector<ResourceId> Collect(BindingRegistry& bindings);
    bool IsValid(ResourceId id, bool allowRetiring = false) const noexcept;
    std::optional<render::ShaderParameterValue> Value(ResourceId id) const;
    Nullable<render::Buffer*> Buffer(ResourceId id) const noexcept;
    void TrimEmptyPages();

private:
    enum class Kind : uint8_t { Buffer,
                                Texture,
                                View,
                                Data,
                                BufferRange,
                                Sampler,
                                OwnedBuffer };
    struct Page {
        unique_ptr<render::Buffer> Buffer;
        byte* Mapped;
        uint64_t Alignment;
        FirstFitAllocator Free;
        uint32_t Allocations{0};
        Page(unique_ptr<render::Buffer> buffer, byte* mapped, uint64_t alignment, uint64_t capacity);
        ~Page() noexcept;
    };
    struct Entry {
        Kind Type;
        Nullable<render::Buffer*> Buffer{nullptr};
        unique_ptr<render::Buffer> OwnedBuffer;
        Nullable<render::Texture*> Texture{nullptr};
        unique_ptr<render::TextureView> View;
        unique_ptr<render::Sampler> Sampler;
        std::optional<ResourceId> Parent;
        render::BufferRange Range{};
        uint32_t Stride{0}, Registrations{0}, Frames{0};
        bool Retiring{false};
        vector<byte> Bytes;
        size_t Hash{0}, PageIndex{0};
        FirstFitAllocator::Allocation Allocation;
    };
    struct Slot {
        uint32_t Generation{1};
        unique_ptr<Entry> Value;
    };
    Nullable<Entry*> Find(ResourceId id) const noexcept;
    ResourceId Insert(unique_ptr<Entry> entry);
    void Destroy(uint32_t index);
    render::Device* _device;
    Guid _owner;
    vector<Slot> _slots;
    vector<uint32_t> _free;
    vector<unique_ptr<Page>> _pages;
    unordered_map<size_t, vector<ResourceId>> _data;
};
}  // namespace radray
