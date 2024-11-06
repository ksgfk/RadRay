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
    auto color = ReadText(std::filesystem::path("shaders") / RADRAY_APPNAME / "color.hlsl").value();
    DeviceDescriptor deviceDesc;
#if defined(RADRAY_PLATFORM_WINDOWS)
    deviceDesc = D3D12DeviceDescriptor{};
#elif defined(RADRAY_PLATFORM_MACOS) || defined(RADRAY_PLATFORM_IOS)
    deviceDesc = MetalDeviceDescriptor{};
#endif
    auto device = CreateDevice(deviceDesc).value();
    auto cmdQueue = device->GetCommandQueue(QueueType::Direct, 0).value();
    auto cmdPool = cmdQueue->CreateCommandPool().value();
    auto cmdBuffer = cmdPool->CreateCommandBuffer().value();
    if (cmdBuffer == nullptr) {
        std::abort();
    }
    auto dxc = CreateDxc().value();
    {
        auto outp = dxc->Compile(color, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, false, {}, {}, false).value();
        RADRAY_INFO_LOG("type={} size={}", outp.category, outp.data.size());
    }
    {
        auto outp = dxc->Compile(color, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, false, {}, {}, true).value();
        RADRAY_INFO_LOG("type={} size={}", outp.category, outp.data.size());
        auto msl = SpirvToMsl(outp.data);
        // RADRAY_INFO_LOG("to msl\n{}", msl.value());
        std::span<const byte> blob{reinterpret_cast<const byte*>(msl->data()), msl->size()};
        auto shader = device->CreateShader(blob, ShaderBlobCategory::MSL, ShaderStage::Pixel, "PSMain", "color").value();
        RADRAY_INFO_LOG("shader name {}", shader->Name);
    }
    return 0;
}
