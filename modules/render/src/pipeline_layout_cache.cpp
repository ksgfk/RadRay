#include <radray/render/pipeline_layout_cache.h>

#include <limits>

#include <radray/logger.h>

namespace radray::render {

PipelineLayoutCache::~PipelineLayoutCache() noexcept = default;

void PipelineLayoutCache::Destroy() noexcept {
    RADRAY_ASSERT(GetEntryCount() == 0);
    _closed = true;
}

CachedPipelineLayout::~CachedPipelineLayout() noexcept {
    RADRAY_ASSERT(_refCount == 0);
}

void CachedPipelineLayout::AddRef() noexcept {
    RADRAY_ASSERT(_refCount != std::numeric_limits<uint32_t>::max());
    ++_refCount;
}

bool CachedPipelineLayout::ReleaseRef() noexcept {
    RADRAY_ASSERT(_refCount > 0);
    return --_refCount == 0;
}

}  // namespace radray::render
