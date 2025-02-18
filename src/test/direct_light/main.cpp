#include <thread>

#include <radray/logger.h>
#include <radray/utility.h>
#include <radray/triangle_mesh.h>
#include <radray/vertex_data.h>
#include <radray/camera_control.h>

#include <radray/window/glfw_window.h>

#include <radray/render/device.h>
#include <radray/render/resource.h>
#include <radray/render/command_queue.h>
#include <radray/render/command_pool.h>
#include <radray/render/command_buffer.h>
#include <radray/render/command_encoder.h>

using namespace radray;
using namespace radray::render;
using namespace radray::window;

constexpr auto WIN_WIDTH = 1280;
constexpr auto WIN_HEIGHT = 720;

struct PreObject {
    Eigen::Matrix4f mvp;
};

class TestApp1 {
public:
    TestApp1() {
        RADRAY_INFO_LOG("{} start", RADRAY_APPNAME);
        GlobalInitGlfw();
    }

    ~TestApp1() {
        _cubeVb = nullptr;
        _cubeIb = nullptr;
        _cubeCb = nullptr;

        _dxc = nullptr;
        _cmdBuffer = nullptr;
        _cmdPool = nullptr;
        _swapchain = nullptr;
        _device = nullptr;
        _window = nullptr;
        GlobalTerminateGlfw();
        RADRAY_INFO_LOG("{} end", RADRAY_APPNAME);
    }

    void ShowWindow() {
        _window = make_unique<GlfwWindow>(RADRAY_APPNAME, WIN_WIDTH, WIN_HEIGHT);
        RADRAY_INFO_LOG("show window");
    }

    void InitGraphics() {
        RADRAY_INFO_LOG("start init graphics");
        D3D12DeviceDescriptor d3d12Desc{
            std::nullopt,
            true,
            true};
        _device = CreateDevice(d3d12Desc).Unwrap();
        CommandQueue* cmdQueue =
            _device->GetCommandQueue(QueueType::Direct, 0).Unwrap();
        _cmdPool = _device->CreateCommandPool(cmdQueue).Unwrap();
        _cmdBuffer = _device->CreateCommandBuffer(_cmdPool.get()).Unwrap();
        _dxc = CreateDxc().Unwrap();
        _swapchain = _device->CreateSwapChain(
                                cmdQueue,
                                _window->GetNativeHandle(),
                                WIN_WIDTH,
                                WIN_HEIGHT,
                                2,
                                TextureFormat::RGBA8_UNORM,
                                true)
                         .Unwrap();
        RADRAY_INFO_LOG("end init graphics");
    }

    void SetupCamera() {
        _camPos = Eigen::Vector3f(0.0f, 0.0f, -2.0f);
        _camTarget = Eigen::Vector3f(0.0f, 0.0f, 0.0f);
        _camUp = Eigen::Vector3f(0.0f, 1.0f, 0.0f);
    }

    void SetupCube() {
        TriangleMesh cube{};
        cube.InitAsCube(0.5f);
        VertexData vd{};
        cube.ToVertexData(&vd);

        _cubeVb = _device->CreateBuffer(
                             vd.vertexSize,
                             ResourceType::Buffer,
                             ResourceUsage::Default,
                             ToFlag(ResourceState::VertexAndConstantBuffer),
                             ToFlag(ResourceMemoryTip::None),
                             "cube_vb")
                      .Unwrap();
        _cubeIb = _device->CreateBuffer(
                             vd.indexSize,
                             ResourceType::Buffer,
                             ResourceUsage::Default,
                             ToFlag(ResourceState::IndexBuffer),
                             ToFlag(ResourceMemoryTip::None),
                             "cube_vb")
                      .Unwrap();

        auto uploadVb = _device->CreateBuffer(
                                   vd.vertexSize,
                                   ResourceType::Buffer,
                                   ResourceUsage::Upload,
                                   ToFlag(ResourceState::GenericRead),
                                   ToFlag(ResourceMemoryTip::None))
                            .Unwrap();
        {
            auto ptr = uploadVb->Map(0, uploadVb->GetSize()).Unwrap();
            std::memcpy(ptr, vd.vertexData.get(), vd.vertexSize);
            uploadVb->Unmap();
        }
        auto uploadIb = _device->CreateBuffer(
                                   vd.indexSize,
                                   ResourceType::Buffer,
                                   ResourceUsage::Upload,
                                   ToFlag(ResourceState::GenericRead),
                                   ToFlag(ResourceMemoryTip::None))
                            .Unwrap();
        {
            auto ptr = uploadIb->Map(0, uploadIb->GetSize()).Unwrap();
            std::memcpy(ptr, vd.indexData.get(), vd.indexSize);
            uploadIb->Unmap();
        }
        _cubeCb = _device->CreateBuffer(
                             sizeof(PreObject),
                             ResourceType::Buffer,
                             ResourceUsage::Upload,
                             ToFlag(ResourceState::GenericRead),
                             ToFlag(ResourceMemoryTip::None),
                             "cube_cb")
                      .Unwrap();
        _cubeCbMapped = _cubeCb->Map(0, _cubeCb->GetSize()).Unwrap();

        _cmdPool->Reset();
        _cmdBuffer->Begin();
        {
            BufferBarrier barriers[] = {
                {_cubeVb.get(),
                 ToFlag(ResourceState::VertexAndConstantBuffer),
                 ToFlag(ResourceState::CopyDestination)},
                {_cubeIb.get(),
                 ToFlag(ResourceState::IndexBuffer),
                 ToFlag(ResourceState::CopyDestination)}};
            ResourceBarriers rb{barriers, {}};
            _cmdBuffer->ResourceBarrier(rb);
        }
        _cmdBuffer->CopyBuffer(uploadVb.get(), 0, _cubeVb.get(), 0, vd.vertexSize);
        _cmdBuffer->CopyBuffer(uploadIb.get(), 0, _cubeIb.get(), 0, vd.indexSize);
        {
            BufferBarrier barriers[] = {
                {_cubeVb.get(),
                 ToFlag(ResourceState::CopyDestination),
                 ToFlag(ResourceState::VertexAndConstantBuffer)},
                {_cubeIb.get(),
                 ToFlag(ResourceState::CopyDestination),
                 ToFlag(ResourceState::IndexBuffer)}};
            ResourceBarriers rb{barriers, {}};
            _cmdBuffer->ResourceBarrier(rb);
        }
        _cmdBuffer->End();
        CommandBuffer* t[] = {_cmdBuffer.get()};
        auto cmdQueue = _device->GetCommandQueue(QueueType::Direct, 0).Unwrap();
        cmdQueue->Submit(t, Nullable<Fence>{});
        cmdQueue->Wait();
    }

    void Start() {
        ShowWindow();
        InitGraphics();
        SetupCamera();
        SetupCube();
    }

    void Update() {
        while (true) {
            GlobalPollEventsGlfw();
            if (_window->ShouldClose()) {
                break;
            }
            std::this_thread::yield();
        }
    }

    unique_ptr<GlfwWindow> _window;

    shared_ptr<Device> _device;
    shared_ptr<CommandPool> _cmdPool;
    shared_ptr<CommandBuffer> _cmdBuffer;
    shared_ptr<SwapChain> _swapchain;
    shared_ptr<Dxc> _dxc;

    shared_ptr<Buffer> _cubeVb;
    shared_ptr<Buffer> _cubeIb;
    shared_ptr<Buffer> _cubeCb;
    void* _cubeCbMapped;

    CameraControl _camCtrl;
    Eigen::Vector3f _camPos;
    Eigen::Vector3f _camTarget;
    Eigen::Vector3f _camUp;
};

int main() {
    TestApp1 app{};
    app.Start();
    app.Update();
    return 0;
}
