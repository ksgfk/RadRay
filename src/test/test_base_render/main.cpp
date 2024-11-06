#include <radray/logger.h>
#include <radray/utility.h>
#include <radray/render/device.h>
#include <radray/render/dxc.h>
#include <radray/render/spirvc.h>
#include <radray/render/command_queue.h>
#include <radray/render/command_pool.h>
#include <radray/render/command_buffer.h>
#include <radray/render/shader.h>

using namespace radray;
using namespace radray::render;

int main() {
    std::shared_ptr<Device> device;
#if defined(RADRAY_PLATFORM_WINDOWS)
    device = CreateDevice(D3D12DeviceDescriptor{}).value();
#elif defined(RADRAY_PLATFORM_MACOS) || defined(RADRAY_PLATFORM_IOS)
    device = CreateDevice(MetalDeviceDescriptor{}).value();
#endif
    auto cmdQueue = device->GetCommandQueue(QueueType::Direct, 0).value();
    auto cmdPool = cmdQueue->CreateCommandPool().value();
    auto cmdBuffer = cmdPool->CreateCommandBuffer().value();
    if (cmdBuffer == nullptr) {
        std::abort();
    }
    auto dxc = CreateDxc().value();
    {
        std::string_view includes[] = {std::string_view{"shaders"}};
        auto color = ReadText(std::filesystem::path("shaders") / "DefaultVS.hlsl").value();
        auto outv = dxc->Compile(
            color,
            "main",
            ShaderStage::Vertex,
            HlslShaderModel::SM60,
            true,
            {},
            includes,
            true);
        auto outp = outv.value();
        RADRAY_INFO_LOG("type={} size={}", outp.category, outp.data.size());
        auto msl = SpirvToMsl(outp.data, MslVersion::MSL24, MslPlatform::Macos);
        RADRAY_INFO_LOG("to msl\n{}", msl.value());
        // std::span<const byte> blob{reinterpret_cast<const byte*>(msl->data()), msl->size()};
        // auto shader = device->CreateShader(blob, ShaderBlobCategory::MSL, ShaderStage::Vertex, "main", "color").value();
        // RADRAY_INFO_LOG("shader name {}", shader->Name);
    }
    {
        std::string_view includes[] = {std::string_view{"shaders"}};
        auto color = ReadText(std::filesystem::path("shaders") / "DefaultPS.hlsl").value();
        auto outp = *dxc->Compile(
            color,
            "main",
            ShaderStage::Pixel,
            HlslShaderModel::SM60,
            true,
            {},
            includes,
            true);
        RADRAY_INFO_LOG("type={} size={}", outp.category, outp.data.size());
        auto msl = SpirvToMsl(outp.data, MslVersion::MSL24, MslPlatform::Macos);
        RADRAY_INFO_LOG("to msl\n{}", msl.value());
        // std::span<const byte> blob{reinterpret_cast<const byte*>(msl->data()), msl->size()};
        // auto shader = device->CreateShader(blob, ShaderBlobCategory::MSL, ShaderStage::Pixel, "main", "color").value();
        // RADRAY_INFO_LOG("shader name {}", shader->Name);
    }
    return 0;
}
