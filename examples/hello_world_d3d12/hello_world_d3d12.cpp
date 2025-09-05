#include <thread>

#include <radray/logger.h>
#include <radray/stopwatch.h>
#include <radray/triangle_mesh.h>
#include <radray/vertex_data.h>
#include <radray/utility.h>
#include <radray/render/common.h>
#include <radray/render/dxc.h>
#include <radray/window/glfw_window.h>

#include "../../modules/render/src/d3d12/d3d12_impl.h"

using namespace radray;
using namespace radray::render;
using namespace radray::window;

constexpr int WIN_WIDTH = 1280;
constexpr int WIN_HEIGHT = 720;
constexpr int RT_COUNT = 3;
const char* RADRAY_APPNAME = "hello_world_d3d12";

struct FrameData {
    shared_ptr<d3d12::CmdListD3D12> cmdBuffer;
};

unique_ptr<GlfwWindow> glfw;
shared_ptr<d3d12::DeviceD3D12> device;
d3d12::CmdQueueD3D12* cmdQueue;
shared_ptr<d3d12::SwapChainD3D12> swapchain;
vector<FrameData> frames;
unordered_map<Texture*, shared_ptr<d3d12::TextureD3D12>> rtViews;
uint32_t currentFrameIndex = 0;
ColorClearValue clear{0.0f, 0.0f, 0.0f, 1.0f};
int clearIndex = 0;
Stopwatch sw;
uint64_t last;
shared_ptr<d3d12::BufferD3D12> vertBuf;
shared_ptr<d3d12::BufferD3D12> idxBuf;
shared_ptr<Dxc> dxc;
shared_ptr<d3d12::RootSigD3D12> pipelineLayout;
shared_ptr<d3d12::GraphicsPsoD3D12> pso;

void Init() {
    GlobalInitGlfw();
    glfw = make_unique<GlfwWindow>(RADRAY_APPNAME, WIN_WIDTH, WIN_HEIGHT, false, false);
    device = std::static_pointer_cast<d3d12::DeviceD3D12>(CreateDevice(D3D12DeviceDescriptor{std::nullopt, true, false}).Unwrap());
    cmdQueue = static_cast<d3d12::CmdQueueD3D12*>(device->GetCommandQueue(QueueType::Direct, 0).Unwrap());
    swapchain = std::static_pointer_cast<d3d12::SwapChainD3D12>(device->CreateSwapChain({cmdQueue, glfw->GetNativeHandle(), WIN_WIDTH, WIN_HEIGHT, RT_COUNT, TextureFormat::RGBA8_UNORM, false}).Unwrap());
    frames.reserve(swapchain->_frames.size());
    for (size_t i = 0; i < swapchain->_frames.size(); ++i) {
        auto& f = frames.emplace_back();
        f.cmdBuffer = std::static_pointer_cast<d3d12::CmdListD3D12>(device->CreateCommandBuffer(cmdQueue).Unwrap());
    }
    currentFrameIndex = 0;
    sw.Reset();
    sw.Start();
    last = 0;

    TriangleMesh mesh;
    mesh.Positions = {{0, 0.5f, 0}, {-0.5f, -0.366f, 0}, {0.5f, -0.366f, 0}};
    mesh.Indices = {0, 1, 2};
    VertexData model;
    mesh.ToVertexData(&model);
    auto vertUpload = device->CreateBuffer({model.VertexSize, MemoryType::Upload, BufferUse::CopySource | BufferUse::MapWrite, ResourceHint::None, {}}).Unwrap();
    auto vert = device->CreateBuffer({model.VertexSize, MemoryType::Device, BufferUse::CopyDestination | BufferUse::Vertex, ResourceHint::None, {}}).Unwrap();
    auto idxUpload = device->CreateBuffer({model.IndexSize, MemoryType::Upload, BufferUse::CopySource | BufferUse::MapWrite, ResourceHint::None, {}}).Unwrap();
    auto idx = device->CreateBuffer({model.IndexSize, MemoryType::Device, BufferUse::CopyDestination | BufferUse::Index, ResourceHint::None, {}}).Unwrap();
    vertBuf = std::static_pointer_cast<d3d12::BufferD3D12>(vert);
    idxBuf = std::static_pointer_cast<d3d12::BufferD3D12>(idx);
    vertUpload->CopyFromHost({model.VertexData.get(), model.VertexSize}, 0);
    idxUpload->CopyFromHost({model.IndexData.get(), model.IndexSize}, 0);

    auto cmdBuffer = std::static_pointer_cast<d3d12::CmdListD3D12>(device->CreateCommandBuffer(cmdQueue).Unwrap());
    cmdBuffer->Begin();
    {
        BarrierBufferDescriptor barriers[] = {
            {vert.get(), BufferUse::Common, BufferUse::CopyDestination, nullptr, false},
            {idx.get(), BufferUse::Common, BufferUse::CopyDestination, nullptr, false}};
        cmdBuffer->ResourceBarrier(barriers, {});
    }
    cmdBuffer->CopyBufferToBuffer(vert.get(), 0, vertUpload.get(), 0, model.VertexSize);
    cmdBuffer->CopyBufferToBuffer(idx.get(), 0, idxUpload.get(), 0, model.IndexSize);
    {
        BarrierBufferDescriptor barriers[] = {
            {vert.get(), BufferUse::CopyDestination, BufferUse::Vertex, nullptr, false},
            {idx.get(), BufferUse::CopyDestination, BufferUse::Index, nullptr, false}};
        cmdBuffer->ResourceBarrier(barriers, {});
    }
    cmdBuffer->End();
    CommandBuffer* submitCmdBuffers[] = {cmdBuffer.get()};
    cmdQueue->Submit({submitCmdBuffers, {}, {}, {}, {}});
    cmdQueue->Wait();
}

bool Update() {
    GlobalPollEventsGlfw();
    bool isClose = glfw->ShouldClose();

    swapchain->AcquireNext();
    swapchain->Present();

    return !isClose;
}

void End() {
    cmdQueue->Wait();

    pso.reset();
    pipelineLayout.reset();

    vertBuf.reset();
    idxBuf.reset();

    rtViews.clear();
    frames.clear();
    swapchain = nullptr;
    cmdQueue = nullptr;
    device.reset();
    glfw.reset();
    GlobalTerminateGlfw();
}

int main() {
    Init();
    while (Update()) {
        std::this_thread::yield();
    }
    End();
    return 0;
}
