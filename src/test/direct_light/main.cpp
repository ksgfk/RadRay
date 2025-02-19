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
        _rootSig = nullptr;
        _pso = nullptr;

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
        using namespace std::placeholders;
        _camPos = Eigen::Vector3f(0.0f, 0.0f, -2.0f);
        _camRot = Eigen::Quaternionf::Identity();
        _camCtrl.distance = std::abs(_camPos.z());
        _fovDeg = 90.0f;
        _zNear = 0.1f;
        _zFar = 100.0f;
        _onMouseClick = {std::bind(&TestApp1::OnMouseClick, this, _1, _2, _3, _4), _window->EventMouseButtonCall()};
        _onMouseMove = {std::bind(&TestApp1::OnMouseMove, this, _1), _window->EventCursorPosition()};
    }

    void SetupCube() {
        _cubePos = Eigen::Vector3f(0.0f, 0.0f, 0.0f);
        _SetupCubeMaterial();
        _SetupCubeMesh();
    }

    void _SetupCubeMaterial() {
        radray::string color = ReadText(std::filesystem::path("shaders") / RADRAY_APPNAME / "normal.hlsl").value();
        DxcOutput outv = _dxc->Compile(
                                 color,
                                 "VSMain",
                                 ShaderStage::Vertex,
                                 HlslShaderModel::SM60,
                                 true,
                                 {},
                                 {},
                                 false)
                             .value();
        DxilReflection reflv = _dxc->GetDxilReflection(ShaderStage::Vertex, outv.refl).value();
        shared_ptr<Shader> vs = _device->CreateShader(
                                           outv.data,
                                           reflv,
                                           ShaderStage::Vertex,
                                           "VSMain",
                                           "colorVS")
                                    .Unwrap();
        DxcOutput outp = _dxc->Compile(
                                 color,
                                 "PSMain",
                                 ShaderStage::Pixel,
                                 HlslShaderModel::SM60,
                                 true,
                                 {},
                                 {},
                                 false)
                             .value();
        DxilReflection reflp = _dxc->GetDxilReflection(ShaderStage::Pixel, outp.refl).value();
        shared_ptr<Shader> ps = _device->CreateShader(
                                           outp.data,
                                           reflp,
                                           ShaderStage::Pixel,
                                           "PSMain",
                                           "colorPS")
                                    .Unwrap();

        // Shader* shaders[] = {vs.get(), ps.get()};
        // _rootSig = _device->CreateRootSignature(shaders).Unwrap();
        // GraphicsPipelineStateDescriptor psoDesc{};
        // psoDesc.RootSig = _rootSig.get();
        // psoDesc.VS = vs.get();
        // psoDesc.PS = ps.get();
        // _pso = _device->CreateGraphicsPipeline(psoDesc).Unwrap();
    }

    void _SetupCubeMesh() {
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
            UpdateCamera();
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
    Eigen::Vector3f _cubePos;
    shared_ptr<RootSignature> _rootSig;
    shared_ptr<GraphicsPipelineState> _pso;

    CameraControl _camCtrl;
    float _fovDeg;
    float _zNear;
    float _zFar;
    Eigen::Vector3f _camPos;
    Eigen::Quaternionf _camRot;
    Eigen::Matrix4f _view;
    Eigen::Matrix4f _proj;
    DelegateHandle<MouseButtonCallback> _onMouseClick;
    DelegateHandle<CursorPositionCallback> _onMouseMove;

    void OnMouseClick(const Eigen::Vector2f& xy, MouseButton button, Action action, KeyModifiers modifiers) {
        if (button == MOUSE_BUTTON_LEFT) {
            _camCtrl.nowPos = xy;
            if (action == ACTION_PRESSED) {
                _camCtrl.canOrbit = true;
                _camCtrl.lastPos = xy;
            } else if (action == ACTION_RELEASED) {
                _camCtrl.canOrbit = false;
            }
        }
        RADRAY_UNUSED(modifiers);
    }

    void OnMouseMove(const Eigen::Vector2f& xy) {
        if (_camCtrl.canOrbit) {
            _camCtrl.nowPos = xy;
        }
    }

    void UpdateCamera() {
        _camCtrl.Orbit(_camPos, _camRot);
        if (_camCtrl.canOrbit) {
            Eigen::Matrix3f rotMat = _camRot.toRotationMatrix();
            Eigen::Vector3f radian = rotMat.eulerAngles(2, 0, 1);
            RADRAY_INFO_LOG("x:{} ,y:{} ,z:{}", Degree(radian.x()), Degree(radian.y()), Degree(radian.z()));
        }
        Eigen::Vector3f front = (_camRot * Eigen::Vector3f{0, 0, 1}).normalized();
        Eigen::Vector3f up = (_camRot * Eigen::Vector3f{0, 1, 0}).normalized();
        _view = LookAtFrontLH(_camPos, front, up);
        _proj = PerspectiveLH(Radian(_fovDeg), WIN_WIDTH / static_cast<float>(WIN_HEIGHT), _zNear, _zFar);
    }
};

int main() {
    TestApp1 app{};
    app.Start();
    app.Update();
    return 0;
}
