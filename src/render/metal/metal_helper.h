#pragma once

#include <radray/platform.h>
#include <radray/types.h>
#include <radray/logger.h>
#include <radray/utility.h>
#include <radray/render/common.h>

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <MetalFX/MetalFX.hpp>
#include <QuartzCore/QuartzCore.hpp>

namespace radray::render::metal {

class ScopedAutoreleasePool {
public:
    ScopedAutoreleasePool() noexcept;
    ~ScopedAutoreleasePool() noexcept;
    ScopedAutoreleasePool(ScopedAutoreleasePool&&) = delete;
    ScopedAutoreleasePool(const ScopedAutoreleasePool&) = delete;
    ScopedAutoreleasePool& operator=(ScopedAutoreleasePool&&) = delete;
    ScopedAutoreleasePool& operator=(const ScopedAutoreleasePool&) = delete;

private:
    NS::AutoreleasePool* _pool;
};

template <class T>
decltype(auto) AutoRelease(T&& func) noexcept {
    ScopedAutoreleasePool pool{};
    return std::forward<T>(func)();
}

NS::String* NSStringInit(NS::String* that, const void* bytes, NS::UInteger len, NS::StringEncoding encoding) noexcept;

}  // namespace radray::render::metal

namespace MTL {
std::string_view format_as(LanguageVersion v) noexcept;
std::string_view format_as(GPUFamily v) noexcept;
}  // namespace MTL
