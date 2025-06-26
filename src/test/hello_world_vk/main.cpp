#include <radray/logger.h>

#include <radray/render/device.h>
#include <radray/render/fence.h>
#include <radray/render/command_queue.h>
#include <radray/render/command_buffer.h>

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
    auto device = CreateDevice(vkDesc).Unwrap();
    auto cmdQueue = device->GetCommandQueue(QueueType::Direct).Unwrap();
    auto fence = device->CreateFence().Unwrap();
    auto cmdBuffer = cmdQueue->CreateCommandBuffer().Unwrap();
    cmdBuffer->Destroy();
    fence->Destroy();
    device->Destroy();
    GlobalTerminateGraphics();
    return 0;
}
