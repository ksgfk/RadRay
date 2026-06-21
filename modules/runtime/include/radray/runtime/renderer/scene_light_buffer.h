#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include <radray/types.h>
#include <radray/render/common.h>

namespace radray {

class Scene;

struct DirectionalLightGpu {
    float Direction[4];
    float Irradiance[4];
};

struct PointLightGpu {
    float Position[4];
    float Intensity[4];
};

struct LightInfoGpu {
    uint32_t Counts[4];
};

static_assert(sizeof(DirectionalLightGpu) == 32);
static_assert(sizeof(PointLightGpu) == 32);
static_assert(sizeof(LightInfoGpu) == 16);

class SceneLightBuffer {
public:
    static constexpr uint32_t MaxDirectionalLights = 8;
    static constexpr uint32_t MaxPointLights = 64;
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
        const Scene& scene);

    void Clear() noexcept;

private:
    struct FlightResources {
        render::RootSignature* RootSig{nullptr};
        render::DescriptorSetIndex ViewSet{0};
        unique_ptr<render::Buffer> DirectionalBuffer;
        unique_ptr<render::Buffer> PointBuffer;
        unique_ptr<render::Buffer> InfoBuffer;
        unique_ptr<render::DescriptorSet> DescriptorSet;
    };

    bool EnsureFlightResources(
        render::Device& device,
        render::RootSignature* rootSig,
        render::DescriptorSetIndex viewSet,
        FlightResources& resources,
        uint32_t flightIndex);

    static bool WriteBuffer(render::Buffer& buffer, const void* data, uint64_t size, std::string_view debugName);
    static bool WriteDescriptorSetResources(FlightResources& resources);

    std::array<FlightResources, MaxFlights> _flights{};
};

}  // namespace radray
