#pragma once

#include <cstddef>
#include <cstdint>

namespace radray::render {

class CachedPipelineLayout;

// Single-threaded. All users must release their references before the cache/device is destroyed.
class PipelineLayoutCache {
public:
    PipelineLayoutCache() noexcept = default;
    PipelineLayoutCache(const PipelineLayoutCache&) = delete;
    PipelineLayoutCache& operator=(const PipelineLayoutCache&) = delete;

    virtual size_t GetEntryCount() const noexcept = 0;
    // Idempotent; requires an empty cache. Does not force-destroy live resources.
    void Destroy() noexcept;
    bool IsClosed() const noexcept { return _closed; }

protected:
    virtual ~PipelineLayoutCache() noexcept;

private:
    virtual void Evict(CachedPipelineLayout* layout) noexcept = 0;
    bool _closed{false};
};

// Backend-private derived objects own the native resource. The cache contributes no counted ref.
class CachedPipelineLayout {
public:
    CachedPipelineLayout(const CachedPipelineLayout&) = delete;
    CachedPipelineLayout& operator=(const CachedPipelineLayout&) = delete;

    uint32_t GetRefCount() const noexcept { return _refCount; }

protected:
    CachedPipelineLayout() noexcept = default;
    virtual ~CachedPipelineLayout() noexcept;
    void AddRef() noexcept;
    // Returns true when the concrete backend must evict and destroy this entry.
    bool ReleaseRef() noexcept;

private:
    uint32_t _refCount{0};
};

}  // namespace radray::render
