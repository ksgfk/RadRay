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
    VulkanCommandQueueDescriptor queueDesc[] = {
        {QueueType::Direct, 1},
        {QueueType::Compute, 1},
        {QueueType::Copy, 1}};
    vkDesc.Queues = queueDesc;
    CreateDevice(vkDesc);
    GlobalTerminateGraphics();
    return 0;
}
