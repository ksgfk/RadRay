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

MTL::TextureType ToMtlTextureType(TextureDimension dim) noexcept;

TextureDimension ToRhiDimension(MTL::TextureType type) noexcept;

MTL::VertexFormat ToMtlVertexFormat(InputElementFormat format) noexcept;

MTL::BlendFactor ToMtlBlendFactor(BlendType type) noexcept;

MTL::BlendOperation ToMtlBlendOp(BlendOpMode mode) noexcept;

MTL::ColorWriteMask ToMtlColorWriteMask(ColorWriteEnable value) noexcept;

MTL::PrimitiveTopologyClass ToMtlPrimitiveTopology(PrimitiveTopology topo) noexcept;

}  // namespace radray::rhi::metal

extern "C" void RadrayPrintMTLFunctionLog(MTL::LogContainer* logs) noexcept;
