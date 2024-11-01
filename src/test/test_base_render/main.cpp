#include <radray/render/device.h>

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
    auto queue = device->GetCommandQueue(QueueType::Direct, 0).value();
    auto same = device->GetCommandQueue(QueueType::Direct, 0).value();
    if (queue != same) {
        std::abort();
    }
    return 0;
}
