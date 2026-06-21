#include <radray/runtime/renderer/scene_light_buffer.h>

#include <algorithm>
#include <cstring>

#include <fmt/format.h>

#include <radray/basic_math.h>
#include <radray/logger.h>
#include <radray/runtime/renderer/light_scene_proxy.h>
#include <radray/runtime/renderer/scene.h>

namespace radray {
namespace {

constexpr uint64_t DirectionalBufferSize = uint64_t(SceneLightBuffer::MaxDirectionalLights) * sizeof(DirectionalLightGpu);
constexpr uint64_t PointBufferSize = uint64_t(SceneLightBuffer::MaxPointLights) * sizeof(PointLightGpu);

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
    const Scene& scene) {
    if (rootSig == nullptr) {
        return nullptr;
    }
    if (flightIndex >= _flights.size()) {
        RADRAY_ERR_LOG("SceneLightBuffer: flight index {} exceeds MaxFlights {}", flightIndex, _flights.size());
        return nullptr;
    }

    vector<DirectionalLightGpu> directionalLights;
    vector<PointLightGpu> pointLights;
    directionalLights.reserve(MaxDirectionalLights);
    pointLights.reserve(MaxPointLights);

    for (const unique_ptr<LightSceneProxy>& light : scene.GetLights()) {
        if (light == nullptr) {
            continue;
        }

        const Eigen::Vector3f radiance = light->GetColor() * light->GetIntensity();
        if (light->GetLightType() == LightType::Directional) {
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
        } else if (light->GetLightType() == LightType::Point) {
            if (pointLights.size() >= MaxPointLights) {
                continue;
            }
            PointLightGpu gpu{};
            FillFloat4(gpu.Position, light->GetPosition(), 1.0f);
            FillFloat4(gpu.Intensity, radiance, 0.0f);
            pointLights.push_back(gpu);
        }
    }

    FlightResources& resources = _flights[flightIndex];
    if (!EnsureFlightResources(device, rootSig, viewSet, resources, flightIndex)) {
        return nullptr;
    }

    std::array<DirectionalLightGpu, MaxDirectionalLights> directionalUpload{};
    std::array<PointLightGpu, MaxPointLights> pointUpload{};
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

    LightInfoGpu info{};
    info.Counts[0] = static_cast<uint32_t>(directionalLights.size());
    info.Counts[1] = static_cast<uint32_t>(pointLights.size());

    if (!WriteBuffer(*resources.DirectionalBuffer, directionalUpload.data(), DirectionalBufferSize, "directional lights") ||
        !WriteBuffer(*resources.PointBuffer, pointUpload.data(), PointBufferSize, "point lights") ||
        !WriteBuffer(*resources.InfoBuffer, &info, sizeof(info), "light info")) {
        return nullptr;
    }

    if (!WriteDescriptorSetResources(resources)) {
        return nullptr;
    }

    return resources.DescriptorSet.get();
}

void SceneLightBuffer::Clear() noexcept {
    for (FlightResources& resources : _flights) {
        resources.DescriptorSet.reset();
        resources.InfoBuffer.reset();
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
        resources.InfoBuffer != nullptr) {
        return true;
    }

    resources.DescriptorSet.reset();
    resources.InfoBuffer.reset();
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
    const uint64_t cbSize = Align(
        static_cast<uint64_t>(sizeof(LightInfoGpu)),
        static_cast<uint64_t>(std::max<uint32_t>(device.GetDetail().CBufferAlignment, 1u)));
    resources.InfoBuffer = createBuffer(
        cbSize,
        render::BufferUse::CBuffer | render::BufferUse::MapWrite,
        fmt::format("scene_light_info_f{}", flightIndex));

    if (resources.DirectionalBuffer == nullptr || resources.PointBuffer == nullptr || resources.InfoBuffer == nullptr) {
        return false;
    }

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

bool SceneLightBuffer::WriteDescriptorSetResources(SceneLightBuffer::FlightResources& resources) {
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

    render::BufferBindingDescriptor infoView{};
    infoView.Target = resources.InfoBuffer.get();
    infoView.Range = render::BufferRange{0, sizeof(LightInfoGpu)};
    infoView.Usage = render::BufferViewUsage::CBuffer;
    if (!resources.DescriptorSet->WriteResource("gLightInfo", infoView)) {
        RADRAY_ERR_LOG("SceneLightBuffer: WriteResource gLightInfo failed");
        return false;
    }

    return true;
}

}  // namespace radray
