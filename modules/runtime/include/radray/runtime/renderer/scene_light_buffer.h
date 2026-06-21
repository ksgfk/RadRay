#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include <radray/basic_math.h>
#include <radray/types.h>
#include <radray/render/common.h>
#include <radray/runtime/renderer/scene_renderer.h>

namespace radray {

class Scene;

struct DirectionalLightGpu {
    float Direction[4];
    float Irradiance[4];
};

struct PointLightGpu {
    float Position[4];   // xyz position, w = range
    float Intensity[4];  // rgb radiance, w = shadow first-slice index (-1 = no shadow)
};

struct SpotLightGpu {
    float Position[4];   // xyz position, w = range
    float Direction[4];  // xyz normalized spot direction, w = cos(outer half-angle)
    float Intensity[4];  // rgb radiance, w = cos(inner half-angle)
    float Params[4];     // x = shadow slice index (-1 = no shadow), yzw reserved
};

struct LightInfoGpu {
    uint32_t Counts[4];  // x directional, y point, z spot
};

static constexpr uint32_t MaxShadowCascadesGpu = 4; // Must match ShadowCascadeData::MaxCascades and RADRAY_MAX_CASCADES.
static constexpr uint32_t MaxAdditionalShadowSlicesGpu = 16; // Must match AdditionalShadowData::MaxSlices and RADRAY_MAX_ADD_SHADOW_SLICES.

struct ShadowParamGpu {
    float WorldToShadow[MaxShadowCascadesGpu][16];
    float CascadeSphere[MaxShadowCascadesGpu][4];
    float Params[4];
};

struct AdditionalShadowParamGpu {
    float WorldToShadow[MaxAdditionalShadowSlicesGpu][16];
    float Params[4]; // x enable, y atlas size(px), z slice count, w soft mode
};

static_assert(sizeof(DirectionalLightGpu) == 32);
static_assert(sizeof(PointLightGpu) == 32);
static_assert(sizeof(SpotLightGpu) == 64);
static_assert(sizeof(LightInfoGpu) == 16);
static_assert(sizeof(ShadowParamGpu) == 336);
static_assert(sizeof(AdditionalShadowParamGpu) == MaxAdditionalShadowSlicesGpu * 64 + 16);

class SceneLightBuffer {
public:
    static constexpr uint32_t MaxDirectionalLights = 8;
    static constexpr uint32_t MaxPointLights = 64;
    static constexpr uint32_t MaxSpotLights = 64;
    static constexpr uint32_t MaxShadowCascadesGpu = radray::MaxShadowCascadesGpu;
    static constexpr uint32_t MaxAdditionalShadowSlicesGpu = radray::MaxAdditionalShadowSlicesGpu;
    // GpuSystem's default FlightDataCount is 2 and apps may choose triple buffering.
    // There is no engine-wide max-flight constant available to this class yet.
    static constexpr uint32_t MaxFlights = 3;

    SceneLightBuffer() noexcept = default;
    SceneLightBuffer(const SceneLightBuffer&) = delete;
    SceneLightBuffer(SceneLightBuffer&&) = delete;
    SceneLightBuffer& operator=(const SceneLightBuffer&) = delete;
    SceneLightBuffer& operator=(SceneLightBuffer&&) = delete;
    ~SceneLightBuffer() noexcept;

    render::DescriptorSet* Update(
        render::Device& device,
        render::RootSignature* rootSig,
        render::DescriptorSetIndex viewSet,
        uint32_t flightIndex,
        const Scene& scene,
        const ShadowCascadeData& shadow,
        render::TextureView* shadowArraySrv,
        const AdditionalShadowData& additionalShadow,
        render::TextureView* additionalShadowArraySrv);

    void Clear() noexcept;

private:
    struct FlightResources {
        render::RootSignature* RootSig{nullptr};
        render::DescriptorSetIndex ViewSet{0};
        unique_ptr<render::Buffer> DirectionalBuffer;
        unique_ptr<render::Buffer> PointBuffer;
        unique_ptr<render::Buffer> SpotBuffer;
        unique_ptr<render::Buffer> InfoBuffer;
        unique_ptr<render::Buffer> ShadowBuffer;
        unique_ptr<render::Buffer> AdditionalShadowBuffer;
        unique_ptr<render::Sampler> ShadowCmpSampler;
        unique_ptr<render::DescriptorSet> DescriptorSet;
    };

    bool EnsureFlightResources(
        render::Device& device,
        render::RootSignature* rootSig,
        render::DescriptorSetIndex viewSet,
        FlightResources& resources,
        uint32_t flightIndex);

    static bool WriteBuffer(render::Buffer& buffer, const void* data, uint64_t size, std::string_view debugName);
    static bool WriteDescriptorSetResources(
        FlightResources& resources,
        render::TextureView* shadowArraySrv,
        render::TextureView* additionalShadowArraySrv);

    std::array<FlightResources, MaxFlights> _flights{};
};

}  // namespace radray
