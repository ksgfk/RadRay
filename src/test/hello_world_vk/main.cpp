#include <radray/logger.h>

#include <radray/render/device.h>

using namespace radray;
using namespace radray::render;

int main() {
    VulkanBackendInitDdescriptor vkInitDesc{};
    vkInitDesc.IsEnableDebugLayer = true;
    vkInitDesc.IsEnableGpuBasedValid = false;
    BackendInitDescriptor initDescs[] = {vkInitDesc};
    GlobalInitGraphics(initDescs);
    VulkanDeviceDescriptor vkDesc{};
    CreateDevice(vkDesc);
    GlobalTerminateGraphics();
    return 0;
}
