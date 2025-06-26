#include <radray/logger.h>

#include <radray/render/dxc.h>
#include <radray/render/device.h>
#include <radray/render/fence.h>
#include <radray/render/command_queue.h>
#include <radray/render/command_buffer.h>
#include <radray/render/shader.h>

using namespace radray;
using namespace radray::render;

int main() {
    auto dxc = CreateDxc().Unwrap();
    string color = ReadText(std::filesystem::path("shaders") / RADRAY_APPNAME / "color.hlsl").value();
    std::string_view includes[] = {"shaders"};

    DxcOutput outv = *dxc->Compile(
        color,
        "VSMain",
        ShaderStage::Vertex,
        HlslShaderModel::SM60,
        true,
        {},
        includes,
        true);
    DxcOutput outp = *dxc->Compile(
        color,
        "PSMain",
        ShaderStage::Pixel,
        HlslShaderModel::SM60,
        true,
        {},
        includes,
        true);

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
    auto vs = device->CreateShader(
                        outv.Data,
                        outv.Category,
                        ShaderStage::Vertex,
                        "VSMain",
                        "colorVS")
                  .Unwrap();
    auto ps = device->CreateShader(
                        outp.Data,
                        outp.Category,
                        ShaderStage::Pixel,
                        "PSMain",
                        "colorPS")
                  .Unwrap();
    vs->Destroy();
    ps->Destroy();
    cmdBuffer->Destroy();
    fence->Destroy();
    device->Destroy();
    GlobalTerminateGraphics();
    return 0;
}
