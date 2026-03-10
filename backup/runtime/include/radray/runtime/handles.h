#pragma once

#include <compare>

#include <radray/types.h>

namespace radray::runtime {

template <class TTag>
struct TypedHandle {
    uint32_t Value{0};

    constexpr bool IsValid() const noexcept { return Value != 0; }

    constexpr explicit operator bool() const noexcept { return IsValid(); }

    static constexpr TypedHandle Invalid() noexcept { return TypedHandle{}; }

    friend auto operator<=>(const TypedHandle& lhs, const TypedHandle& rhs) noexcept = default;
};

using MeshHandle = TypedHandle<struct MeshHandleTag>;
using MaterialHandle = TypedHandle<struct MaterialHandleTag>;
using TextureHandle = TypedHandle<struct TextureHandleTag>;
using BufferHandle = TypedHandle<struct BufferHandleTag>;
using ShaderHandle = TypedHandle<struct ShaderHandleTag>;
using SamplerHandle = TypedHandle<struct SamplerHandleTag>;

}  // namespace radray::runtime
