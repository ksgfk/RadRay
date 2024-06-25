#pragma once

#include <Metal/Metal.hpp>

#include <radray/rhi/common.h>

namespace radray::rhi::metal {

class ScopedAutoreleasePool {
public:
    ScopedAutoreleasePool() noexcept : _pool{NS::AutoreleasePool::alloc()->init()} {}
    ~ScopedAutoreleasePool() noexcept { _pool->release(); }
    ScopedAutoreleasePool(ScopedAutoreleasePool&&) = delete;
    ScopedAutoreleasePool(const ScopedAutoreleasePool&) = delete;
    ScopedAutoreleasePool& operator=(ScopedAutoreleasePool&&) = delete;
    ScopedAutoreleasePool& operator=(const ScopedAutoreleasePool&) = delete;

private:
    NS::AutoreleasePool* _pool;
};

MTL::PixelFormat ToMtlFormat(PixelFormat format) noexcept;

PixelFormat ToRhiFormat(MTL::PixelFormat format) noexcept;

}  // namespace radray::rhi::metal
