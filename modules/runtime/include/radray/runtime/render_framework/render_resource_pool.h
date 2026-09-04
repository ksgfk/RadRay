#pragma once

#include <radray/render/render_pass_registry.h>

namespace radray {

struct TexturePoolKey {
    render::TextureDescriptor Desc;
    friend bool operator==(const TexturePoolKey& lhs, const TexturePoolKey& rhs) noexcept;
};
struct BufferPoolKey {
    render::BufferDescriptor Desc;
    friend bool operator==(const BufferPoolKey& lhs, const BufferPoolKey& rhs) noexcept;
};
struct TextureViewKey {
    render::TextureDimension Dimension;
    render::TextureFormat Format;
    render::SubresourceRange Range;
    render::TextureViewUsage Usage;
    friend bool operator==(const TextureViewKey&, const TextureViewKey&) = default;
};
struct PooledTextureView {
    TextureViewKey Key;
    unique_ptr<render::TextureView> View;
};
struct PooledTexture {
    uint64_t Id{0};
    TexturePoolKey Key;
    unique_ptr<render::Texture> Texture;
    vector<PooledTextureView> Views;
    vector<render::TextureStates> States;
    uint64_t LastUsedCycle{0};
    bool InUse{false};
};
struct PooledBuffer {
    uint64_t Id{0};
    BufferPoolKey Key;
    unique_ptr<render::Buffer> Buffer;
    render::BufferStates State{render::BufferState::Undefined};
    uint64_t LastUsedCycle{0};
    bool InUse{false};
};
struct RenderResourcePoolStats {
    uint64_t Hits{0}, Misses{0}, Created{0}, Trimmed{0}, ViewsCreated{0};
    uint32_t TextureCount{0}, BufferCount{0}, ViewCount{0};
    uint64_t EstimatedBytes{0};
};

class RenderResourcePool {
public:
    RenderResourcePool(render::Device& device, render::RenderPassRegistry& registry, uint32_t unusedFlightCycles = 3);
    ~RenderResourcePool();
    RenderResourcePool(const RenderResourcePool&) = delete;
    RenderResourcePool& operator=(const RenderResourcePool&) = delete;

    /// Called only after the owning flight's previous GPU work is complete.
    void BeginFlight(uint64_t frameSerial);
    Nullable<PooledTexture*> AcquireTexture(const render::TextureDescriptor& desc, std::string_view name);
    Nullable<PooledBuffer*> AcquireBuffer(const render::BufferDescriptor& desc, std::string_view name);
    Nullable<render::TextureView*> GetTextureView(PooledTexture& texture, TextureViewKey key);
    Nullable<render::TextureView*> CreateExternalTextureView(const render::TextureViewDescriptor& desc);
    void EndGraph() noexcept;
    /// Changes future safe-point eviction; never destroys an object during recording.
    void SetUnusedFlightCycles(uint32_t cycles) noexcept { _unusedFlightCycles = cycles; }
    void Clear();
    const RenderResourcePoolStats& GetStats() const noexcept { return _stats; }
    uint64_t GetFrameSerial() const noexcept { return _frameSerial; }

private:
    void RemoveViews(PooledTexture& texture);
    void RefreshStats();
    render::Device& _device;
    render::RenderPassRegistry& _registry;
    vector<unique_ptr<PooledTexture>> _textures;
    vector<unique_ptr<PooledBuffer>> _buffers;
    vector<unique_ptr<render::TextureView>> _externalViews;
    RenderResourcePoolStats _stats;
    uint64_t _cycle{0}, _frameSerial{0}, _nextId{1};
    uint32_t _unusedFlightCycles;
};

render::BufferStates InitialBufferState(const render::BufferDescriptor& desc) noexcept;
uint64_t EstimateTextureBytes(const render::TextureDescriptor& desc) noexcept;

}  // namespace radray
