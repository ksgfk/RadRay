#include <radray/render/device.h>
#include <radray/render/command_queue.h>
#include <radray/render/command_pool.h>
#include <radray/render/command_buffer.h>

using namespace radray;
using namespace radray::render;

int main() {
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
    return 0;
}
