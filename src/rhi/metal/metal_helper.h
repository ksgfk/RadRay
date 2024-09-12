#pragma once

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <MetalFX/MetalFX.hpp>
#include <QuartzCore/QuartzCore.hpp>

#include <radray/logger.h>
#include <radray/rhi/ctypes.h>
#include <radray/rhi/device_interface.h>
#include <radray/rhi/helper.h>

namespace radray::rhi::metal {

class MetalException : public std::runtime_error {
public:
    explicit MetalException(const radray::string& message) : std::runtime_error(message.c_str()) {}
    explicit MetalException(const char* message) : std::runtime_error(message) {}
};

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

template <typename T>
decltype(auto) AutoRelease(T&& func) noexcept {
    ScopedAutoreleasePool pool;
    return std::forward<T>(func)();
}

MTL::PixelFormat EnumConvert(RadrayFormat format) noexcept;
NS::UInteger EnumConvert(RadrayTextureMSAACount cnt) noexcept;
MTL::TextureType EnumConvert(RadrayTextureDimension dim) noexcept;
MTL::LoadAction EnumConvert(RadrayLoadAction load) noexcept;
MTL::StoreAction EnumConvert(RadrayStoreAction store) noexcept;

}  // namespace radray::rhi::metal

#ifndef RADRAY_MTL_THROW
#define RADRAY_MTL_THROW(fmt, ...) throw MetalException(radray::format(fmt __VA_OPT__(, ) __VA_ARGS__))
#endif
