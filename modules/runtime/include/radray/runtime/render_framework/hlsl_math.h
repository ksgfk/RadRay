#pragma once

#include <cstring>
#include <type_traits>

#include <radray/basic_math.h>

namespace radray {

struct float2 {
    float x;
    float y;

    float2() = default;
    float2(const Eigen::Vector2f& value) noexcept : x(value.x()), y(value.y()) {}
    operator Eigen::Vector2f() const noexcept { return {x, y}; }
};

struct float3 {
    float x;
    float y;
    float z;

    float3() = default;
    float3(const Eigen::Vector3f& value) noexcept : x(value.x()), y(value.y()), z(value.z()) {}
    operator Eigen::Vector3f() const noexcept { return {x, y, z}; }
};

struct float4 {
    float x;
    float y;
    float z;
    float w;

    float4() = default;
    float4(const Eigen::Vector4f& value) noexcept : x(value.x()), y(value.y()), z(value.z()), w(value.w()) {}
    operator Eigen::Vector4f() const noexcept { return {x, y, z, w}; }
};

struct float4x4 {
    float m[16];

    float4x4() = default;
    float4x4(const Eigen::Matrix4f& value) noexcept { std::memcpy(m, value.data(), sizeof(m)); }
    operator Eigen::Matrix4f() const noexcept {
        Eigen::Matrix4f value;
        std::memcpy(value.data(), m, sizeof(m));
        return value;
    }
};

static_assert(sizeof(float2) == 8);
static_assert(sizeof(float3) == 12);
static_assert(sizeof(float4) == 16);
static_assert(sizeof(float4x4) == 64);
static_assert(alignof(float3) == alignof(float));
static_assert(std::is_standard_layout_v<float2> && std::is_trivially_copyable_v<float2>);
static_assert(std::is_standard_layout_v<float3> && std::is_trivially_copyable_v<float3>);
static_assert(std::is_standard_layout_v<float4> && std::is_trivially_copyable_v<float4>);
static_assert(std::is_standard_layout_v<float4x4> && std::is_trivially_copyable_v<float4x4>);

}  // namespace radray
