#include <radray/runtime/renderer/scene_light_buffer.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <unordered_map>

#include <fmt/format.h>

#include <radray/basic_math.h>
#include <radray/logger.h>
#include <radray/runtime/renderer/light_scene_proxy.h>
#include <radray/runtime/renderer/scene.h>

namespace radray {
namespace {

constexpr uint64_t DirectionalBufferSize = uint64_t(SceneLightBuffer::MaxDirectionalLights) * sizeof(DirectionalLightGpu);
constexpr uint64_t PointBufferSize = uint64_t(SceneLightBuffer::MaxPointLights) * sizeof(PointLightGpu);
constexpr uint64_t SpotBufferSize = uint64_t(SceneLightBuffer::MaxSpotLights) * sizeof(SpotLightGpu);
constexpr float ShadowMapResolutionGpu = 2048.0f;

void FillFloat4(float out[4], const Eigen::Vector3f& value, float w) noexcept {
    out[0] = value.x();
    out[1] = value.y();
    out[2] = value.z();
    out[3] = w;
}

}  // namespace

SceneLightBuffer::~SceneLightBuffer() noexcept {
    Clear();
}

render::DescriptorSet* SceneLightBuffer::Update(
    render::Device& device,
    render::RootSignature* rootSig,
    render::DescriptorSetIndex viewSet,
    uint32_t flightIndex,
    const Scene& scene,
    const ShadowCascadeData& shadow,
    render::TextureView* shadowArraySrv,
    const AdditionalShadowData& additionalShadow,
    render::TextureView* additionalShadowArraySrv) {
    if (rootSig == nullptr) {
        return nullptr;
    }
    if (flightIndex >= _flights.size()) {
        RADRAY_ERR_LOG("SceneLightBuffer: flight index {} exceeds MaxFlights {}", flightIndex, _flights.size());
        return nullptr;
    }

    // per-light → first shadow slice map (URP's per-light index mapping, minimal form).
    std::unordered_map<const LightSceneProxy*, uint32_t> shadowSliceOfLight;
    if (additionalShadow.Enabled) {
        shadowSliceOfLight.reserve(additionalShadow.Lights.size());
        for (const AdditionalShadowLight& record : additionalShadow.Lights) {
            if (record.Light != nullptr) {
                shadowSliceOfLight.emplace(record.Light, record.FirstSlice);
            }
        }
    }

    vector<DirectionalLightGpu> directionalLights;
    vector<PointLightGpu> pointLights;
    vector<SpotLightGpu> spotLights;
    directionalLights.reserve(MaxDirectionalLights);
    pointLights.reserve(MaxPointLights);
    spotLights.reserve(MaxSpotLights);

    for (const unique_ptr<LightSceneProxy>& light : scene.GetLights()) {
        if (light == nullptr) {
            continue;
        }

        const Eigen::Vector3f radiance = light->GetColor() * light->GetIntensity();
        const LightType type = light->GetLightType();
        float shadowSlice = -1.0f;
        if (auto it = shadowSliceOfLight.find(light.get()); it != shadowSliceOfLight.end()) {
            shadowSlice = static_cast<float>(it->second);
        }

        if (type == LightType::Directional) {
            if (directionalLights.size() >= MaxDirectionalLights) {
                continue;
            }
            Eigen::Vector3f direction = light->GetDirection();
            const float len = direction.norm();
            if (len > 1e-6f) {
                direction /= len;
            } else {
                direction = Eigen::Vector3f{0.0f, -1.0f, 0.0f};
            }

            DirectionalLightGpu gpu{};
            FillFloat4(gpu.Direction, direction, 0.0f);
            FillFloat4(gpu.Irradiance, radiance, 0.0f);
            directionalLights.push_back(gpu);
        } else if (type == LightType::Point) {
            if (pointLights.size() >= MaxPointLights) {
                continue;
            }
            PointLightGpu gpu{};
            FillFloat4(gpu.Position, light->GetPosition(), std::max(light->GetRange(), 0.0f));
            FillFloat4(gpu.Intensity, radiance, shadowSlice);
            pointLights.push_back(gpu);
        } else if (type == LightType::Spot) {
            if (spotLights.size() >= MaxSpotLights) {
                continue;
            }
            Eigen::Vector3f direction = light->GetDirection();
            const float len = direction.norm();
            if (len > 1e-6f) {
                direction /= len;
            } else {
                direction = Eigen::Vector3f{0.0f, -1.0f, 0.0f};
            }
            SpotLightGpu gpu{};
            FillFloat4(gpu.Position, light->GetPosition(), std::max(light->GetRange(), 0.0f));
            FillFloat4(gpu.Direction, direction, std::cos(light->GetSpotOuterAngle()));
            FillFloat4(gpu.Intensity, radiance, std::cos(light->GetSpotInnerAngle()));
            gpu.Params[0] = shadowSlice;
            gpu.Params[1] = 0.0f;
            gpu.Params[2] = 0.0f;
            gpu.Params[3] = 0.0f;
            spotLights.push_back(gpu);
        }
    }

    FlightResources& resources = _flights[flightIndex];
    if (!EnsureFlightResources(device, rootSig, viewSet, resources, flightIndex)) {
        return nullptr;
    }

    std::array<DirectionalLightGpu, MaxDirectionalLights> directionalUpload{};
    std::array<PointLightGpu, MaxPointLights> pointUpload{};
    std::array<SpotLightGpu, MaxSpotLights> spotUpload{};
    if (!directionalLights.empty()) {
        std::memcpy(
            directionalUpload.data(),
            directionalLights.data(),
            directionalLights.size() * sizeof(DirectionalLightGpu));
    }
    if (!pointLights.empty()) {
        std::memcpy(
            pointUpload.data(),
            pointLights.data(),
            pointLights.size() * sizeof(PointLightGpu));
    }
    if (!spotLights.empty()) {
        std::memcpy(
            spotUpload.data(),
            spotLights.data(),
            spotLights.size() * sizeof(SpotLightGpu));
    }

    LightInfoGpu info{};
    info.Counts[0] = static_cast<uint32_t>(directionalLights.size());
    info.Counts[1] = static_cast<uint32_t>(pointLights.size());
    info.Counts[2] = static_cast<uint32_t>(spotLights.size());

    ShadowParamGpu shadowGpu{};
    const uint32_t cascadeCount = std::min<uint32_t>(shadow.CascadeCount, SceneLightBuffer::MaxShadowCascadesGpu);
    for (uint32_t i = 0; i < cascadeCount; ++i) {
        std::memcpy(shadowGpu.WorldToShadow[i], shadow.Cascades[i].View.ViewProjMatrix.data(), sizeof(shadowGpu.WorldToShadow[i]));
        shadowGpu.CascadeSphere[i][0] = shadow.SphereCenters[i].x();
        shadowGpu.CascadeSphere[i][1] = shadow.SphereCenters[i].y();
        shadowGpu.CascadeSphere[i][2] = shadow.SphereCenters[i].z();
        shadowGpu.CascadeSphere[i][3] = shadow.SphereRadiiSq[i];
    }
    shadowGpu.Params[0] = shadow.Enabled ? 1.0f : 0.0f;
    shadowGpu.Params[1] = ShadowMapResolutionGpu;
    shadowGpu.Params[2] = static_cast<float>(cascadeCount);
    shadowGpu.Params[3] = static_cast<float>(static_cast<uint32_t>(shadow.SoftMode));

    AdditionalShadowParamGpu additionalGpu{};
    const uint32_t sliceCount = std::min<uint32_t>(additionalShadow.SliceCount, SceneLightBuffer::MaxAdditionalShadowSlicesGpu);
    for (uint32_t i = 0; i < sliceCount; ++i) {
        std::memcpy(
            additionalGpu.WorldToShadow[i],
            additionalShadow.Slices[i].View.ViewProjMatrix.data(),
            sizeof(additionalGpu.WorldToShadow[i]));
    }
    additionalGpu.Params[0] = additionalShadow.Enabled ? 1.0f : 0.0f;
    additionalGpu.Params[1] = static_cast<float>(additionalShadow.Resolution);
    additionalGpu.Params[2] = static_cast<float>(sliceCount);
    additionalGpu.Params[3] = static_cast<float>(static_cast<uint32_t>(additionalShadow.SoftMode));

    if (!WriteBuffer(*resources.DirectionalBuffer, directionalUpload.data(), DirectionalBufferSize, "directional lights") ||
        !WriteBuffer(*resources.PointBuffer, pointUpload.data(), PointBufferSize, "point lights") ||
        !WriteBuffer(*resources.SpotBuffer, spotUpload.data(), SpotBufferSize, "spot lights") ||
        !WriteBuffer(*resources.InfoBuffer, &info, sizeof(info), "light info") ||
        !WriteBuffer(*resources.ShadowBuffer, &shadowGpu, sizeof(shadowGpu), "shadow param") ||
        !WriteBuffer(*resources.AdditionalShadowBuffer, &additionalGpu, sizeof(additionalGpu), "additional shadow param")) {
        return nullptr;
    }

    if (!WriteDescriptorSetResources(resources, shadowArraySrv, additionalShadowArraySrv)) {
        return nullptr;
    }

    return resources.DescriptorSet.get();
}

void SceneLightBuffer::Clear() noexcept {
    for (FlightResources& resources : _flights) {
        resources.DescriptorSet.reset();
        resources.ShadowCmpSampler.reset();
        resources.AdditionalShadowBuffer.reset();
        resources.ShadowBuffer.reset();
        resources.InfoBuffer.reset();
        resources.SpotBuffer.reset();
        resources.PointBuffer.reset();
        resources.DirectionalBuffer.reset();
        resources.RootSig = nullptr;
        resources.ViewSet = render::DescriptorSetIndex{0};
    }
}

bool SceneLightBuffer::EnsureFlightResources(
    render::Device& device,
    render::RootSignature* rootSig,
    render::DescriptorSetIndex viewSet,
    FlightResources& resources,
    uint32_t flightIndex) {
    if (resources.DescriptorSet != nullptr &&
        resources.RootSig == rootSig &&
        resources.ViewSet == viewSet &&
        resources.DirectionalBuffer != nullptr &&
        resources.PointBuffer != nullptr &&
        resources.SpotBuffer != nullptr &&
        resources.InfoBuffer != nullptr &&
        resources.ShadowBuffer != nullptr &&
        resources.AdditionalShadowBuffer != nullptr &&
        resources.ShadowCmpSampler != nullptr) {
        return true;
    }

    resources.DescriptorSet.reset();
    resources.ShadowCmpSampler.reset();
    resources.AdditionalShadowBuffer.reset();
    resources.ShadowBuffer.reset();
    resources.InfoBuffer.reset();
    resources.SpotBuffer.reset();
    resources.PointBuffer.reset();
    resources.DirectionalBuffer.reset();
    resources.RootSig = rootSig;
    resources.ViewSet = viewSet;

    auto createBuffer = [&device](uint64_t size, render::BufferUses usage, std::string_view name) -> unique_ptr<render::Buffer> {
        render::BufferDescriptor desc{
            .Size = size,
            .Memory = render::MemoryType::Upload,
            .Usage = usage,
            .Hints = render::ResourceHint::None};
        auto bufferOpt = device.CreateBuffer(desc);
        if (!bufferOpt.HasValue()) {
            RADRAY_ERR_LOG("SceneLightBuffer: CreateBuffer '{}' size={} failed", name, size);
            return nullptr;
        }
        auto buffer = bufferOpt.Release();
        buffer->SetDebugName(name);
        return buffer;
    };

    // Light data is rewritten by the CPU every frame and bound as SRV structured buffers.
    // The current Vulkan and D3D12 backends both support Upload memory with Resource|MapWrite
    // for read-only structured buffer descriptors, avoiding a copy/upload pass in this run.
    resources.DirectionalBuffer = createBuffer(
        DirectionalBufferSize,
        render::BufferUse::Resource | render::BufferUse::MapWrite,
        fmt::format("scene_light_directional_f{}", flightIndex));
    resources.PointBuffer = createBuffer(
        PointBufferSize,
        render::BufferUse::Resource | render::BufferUse::MapWrite,
        fmt::format("scene_light_point_f{}", flightIndex));
    resources.SpotBuffer = createBuffer(
        SpotBufferSize,
        render::BufferUse::Resource | render::BufferUse::MapWrite,
        fmt::format("scene_light_spot_f{}", flightIndex));
    const uint64_t cbSize = Align(
        static_cast<uint64_t>(sizeof(LightInfoGpu)),
        static_cast<uint64_t>(std::max<uint32_t>(device.GetDetail().CBufferAlignment, 1u)));
    resources.InfoBuffer = createBuffer(
        cbSize,
        render::BufferUse::CBuffer | render::BufferUse::MapWrite,
        fmt::format("scene_light_info_f{}", flightIndex));
    const uint64_t shadowCbSize = Align(
        static_cast<uint64_t>(sizeof(ShadowParamGpu)),
        static_cast<uint64_t>(std::max<uint32_t>(device.GetDetail().CBufferAlignment, 1u)));
    resources.ShadowBuffer = createBuffer(
        shadowCbSize,
        render::BufferUse::CBuffer | render::BufferUse::MapWrite,
        fmt::format("scene_light_shadow_f{}", flightIndex));
    const uint64_t additionalShadowCbSize = Align(
        static_cast<uint64_t>(sizeof(AdditionalShadowParamGpu)),
        static_cast<uint64_t>(std::max<uint32_t>(device.GetDetail().CBufferAlignment, 1u)));
    resources.AdditionalShadowBuffer = createBuffer(
        additionalShadowCbSize,
        render::BufferUse::CBuffer | render::BufferUse::MapWrite,
        fmt::format("scene_light_add_shadow_f{}", flightIndex));

    if (resources.DirectionalBuffer == nullptr ||
        resources.PointBuffer == nullptr ||
        resources.SpotBuffer == nullptr ||
        resources.InfoBuffer == nullptr ||
        resources.ShadowBuffer == nullptr ||
        resources.AdditionalShadowBuffer == nullptr) {
        return false;
    }

    render::SamplerDescriptor shadowSamplerDesc{
        render::AddressMode::ClampToEdge,
        render::AddressMode::ClampToEdge,
        render::AddressMode::ClampToEdge,
        render::FilterMode::Linear,
        render::FilterMode::Linear,
        render::FilterMode::Linear,
        0.0f,
        std::numeric_limits<float>::max(),
        render::CompareFunction::LessEqual,
        0};
    auto shadowSamplerOpt = device.CreateSampler(shadowSamplerDesc);
    if (!shadowSamplerOpt.HasValue()) {
        RADRAY_ERR_LOG("SceneLightBuffer: CreateSampler shadow comparison failed");
        return false;
    }
    resources.ShadowCmpSampler = shadowSamplerOpt.Release();
    resources.ShadowCmpSampler->SetDebugName(fmt::format("scene_light_shadow_cmp_f{}", flightIndex));

    auto setOpt = device.CreateDescriptorSet(rootSig, viewSet);
    if (!setOpt.HasValue()) {
        RADRAY_ERR_LOG("SceneLightBuffer: CreateDescriptorSet(set={}) failed", viewSet.Value);
        return false;
    }
    resources.DescriptorSet = setOpt.Release();
    resources.DescriptorSet->SetDebugName(fmt::format("scene_light_set_f{}", flightIndex));

    return true;
}

bool SceneLightBuffer::WriteBuffer(render::Buffer& buffer, const void* data, uint64_t size, std::string_view debugName) {
    void* mapped = buffer.Map(0, size);
    if (mapped == nullptr) {
        RADRAY_ERR_LOG("SceneLightBuffer: Map {} failed", debugName);
        return false;
    }
    std::memcpy(mapped, data, static_cast<size_t>(size));
    buffer.Unmap(0, size);
    return true;
}

bool SceneLightBuffer::WriteDescriptorSetResources(
    SceneLightBuffer::FlightResources& resources,
    render::TextureView* shadowArraySrv,
    render::TextureView* additionalShadowArraySrv) {
    render::BufferBindingDescriptor directionalView{};
    directionalView.Target = resources.DirectionalBuffer.get();
    directionalView.Range = render::BufferRange{0, DirectionalBufferSize};
    directionalView.Stride = sizeof(DirectionalLightGpu);
    directionalView.Usage = render::BufferViewUsage::ReadOnlyStorage;
    if (!resources.DescriptorSet->WriteResource("gDirectionalLights", directionalView)) {
        RADRAY_ERR_LOG("SceneLightBuffer: WriteResource gDirectionalLights failed");
        return false;
    }

    render::BufferBindingDescriptor pointView{};
    pointView.Target = resources.PointBuffer.get();
    pointView.Range = render::BufferRange{0, PointBufferSize};
    pointView.Stride = sizeof(PointLightGpu);
    pointView.Usage = render::BufferViewUsage::ReadOnlyStorage;
    if (!resources.DescriptorSet->WriteResource("gPointLights", pointView)) {
        RADRAY_ERR_LOG("SceneLightBuffer: WriteResource gPointLights failed");
        return false;
    }

    render::BufferBindingDescriptor spotView{};
    spotView.Target = resources.SpotBuffer.get();
    spotView.Range = render::BufferRange{0, SpotBufferSize};
    spotView.Stride = sizeof(SpotLightGpu);
    spotView.Usage = render::BufferViewUsage::ReadOnlyStorage;
    if (!resources.DescriptorSet->WriteResource("gSpotLights", spotView)) {
        RADRAY_ERR_LOG("SceneLightBuffer: WriteResource gSpotLights failed");
        return false;
    }

    render::BufferBindingDescriptor infoView{};
    infoView.Target = resources.InfoBuffer.get();
    infoView.Range = render::BufferRange{0, sizeof(LightInfoGpu)};
    infoView.Usage = render::BufferViewUsage::CBuffer;
    if (!resources.DescriptorSet->WriteResource("gLightInfo", infoView)) {
        RADRAY_ERR_LOG("SceneLightBuffer: WriteResource gLightInfo failed");
        return false;
    }

    render::BufferBindingDescriptor shadowView{};
    shadowView.Target = resources.ShadowBuffer.get();
    shadowView.Range = render::BufferRange{0, sizeof(ShadowParamGpu)};
    shadowView.Usage = render::BufferViewUsage::CBuffer;
    if (!resources.DescriptorSet->WriteResource("gShadowParam", shadowView)) {
        RADRAY_ERR_LOG("SceneLightBuffer: WriteResource gShadowParam failed");
        return false;
    }

    render::BufferBindingDescriptor additionalShadowView{};
    additionalShadowView.Target = resources.AdditionalShadowBuffer.get();
    additionalShadowView.Range = render::BufferRange{0, sizeof(AdditionalShadowParamGpu)};
    additionalShadowView.Usage = render::BufferViewUsage::CBuffer;
    if (!resources.DescriptorSet->WriteResource("gAdditionalShadowParam", additionalShadowView)) {
        RADRAY_ERR_LOG("SceneLightBuffer: WriteResource gAdditionalShadowParam failed");
        return false;
    }

    if (shadowArraySrv == nullptr) {
        RADRAY_ERR_LOG("SceneLightBuffer: shadow map array SRV is null");
        return false;
    }
    if (!resources.DescriptorSet->WriteResource("gShadowMap", shadowArraySrv)) {
        RADRAY_ERR_LOG("SceneLightBuffer: WriteResource gShadowMap failed");
        return false;
    }
    if (additionalShadowArraySrv == nullptr) {
        RADRAY_ERR_LOG("SceneLightBuffer: additional shadow map array SRV is null");
        return false;
    }
    if (!resources.DescriptorSet->WriteResource("gAdditionalShadowMap", additionalShadowArraySrv)) {
        RADRAY_ERR_LOG("SceneLightBuffer: WriteResource gAdditionalShadowMap failed");
        return false;
    }
    if (!resources.DescriptorSet->WriteSampler("gShadowSampler", resources.ShadowCmpSampler.get())) {
        RADRAY_ERR_LOG("SceneLightBuffer: WriteSampler gShadowSampler failed");
        return false;
    }

    return true;
}

}  // namespace radray
