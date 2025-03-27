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
#include <radray/render/command_buffer.h>
#include <radray/render/command_encoder.h>
#include <radray/render/swap_chain.h>
#include <radray/render/dxc.h>

using namespace radray;
using namespace radray::render;
using namespace radray::window;

constexpr auto WIN_WIDTH = 1280;
constexpr auto WIN_HEIGHT = 720;

struct PreObject {
    Eigen::Matrix4f mvp;
    Eigen::Matrix4f model;
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

        _depthView = nullptr;
        _depthTex = nullptr;

        _dxc = nullptr;
        _cmdBuffer = nullptr;
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
        CommandQueue* cmdQueue = _device->GetCommandQueue(QueueType::Direct, 0).Unwrap();
        _cmdBuffer = cmdQueue->CreateCommandBuffer().Unwrap();
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

        _depthTex = _device->CreateTexture(
                               WIN_WIDTH,
                               WIN_HEIGHT,
                               1,
                               1,
                               TextureFormat::D24_UNORM_S8_UINT,
                               0,
                               1,
                               0,
                               DepthStencilClearValue{1.0f, 0},
                               ResourceType::DepthStencil,
                               ResourceState::Common,
                               ResourceMemoryTip::None)
                        .Unwrap();
        _depthView = _device->CreateTextureView(
                                _depthTex.get(),
                                ResourceType::DepthStencil,
                                TextureFormat::D24_UNORM_S8_UINT,
                                TextureDimension::Dim2D,
                                0,
                                0,
                                0,
                                0)
                         .Unwrap();
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
        TriangleMesh cube{};
        cube.InitAsCube(0.5f);
        VertexData vd{};
        cube.ToVertexData(&vd);
        _cubePos = Eigen::Vector3f(0.0f, 0.0f, 0.0f);
        _SetupCubeMesh(vd);
        _SetupCubeMaterial(vd);
    }

    void _SetupCubeMaterial(const VertexData& vd) {
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
                                           ShaderBlobCategory::DXIL,
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
                                           ShaderBlobCategory::DXIL,
                                           ShaderStage::Pixel,
                                           "PSMain",
                                           "colorPS")
                                    .Unwrap();

        uint64_t stride = 0;
        radray::vector<VertexElement> elements{};
        elements.reserve(vd.Layouts.size());
        for (size_t v = 0; v < vd.Layouts.size(); v++) {
            const auto& i = vd.Layouts[v];
            stride += i.Size;
            auto& e = elements.emplace_back(VertexElement{});
            e.Offset = i.Offset;
            e.Semantic = i.Semantic;
            e.SemanticIndex = i.SemanticIndex;
            e.Format = ([&]() {
                switch (i.Size) {
                    case sizeof(float): return VertexFormat::FLOAT32;
                    case sizeof(float) * 2: return VertexFormat::FLOAT32X2;
                    case sizeof(float) * 3: return VertexFormat::FLOAT32X3;
                    case sizeof(float) * 4: return VertexFormat::FLOAT32X4;
                    default: return VertexFormat::UNKNOWN;
                }
            })();
            e.Location = v;
        }
        RootSignatureDescriptor rsDesc{};
        RootConstantInfo rsInfos[] = {RootConstantInfo{0, 0, sizeof(PreObject), ShaderStage::Vertex}};
        rsDesc.RootConstants = rsInfos;
        _rootSig = _device->CreateRootSignature(rsDesc).Unwrap();
        GraphicsPipelineStateDescriptor psoDesc{};
        psoDesc.RootSig = _rootSig.get();
        psoDesc.VS = vs.get();
        psoDesc.PS = ps.get();
        psoDesc.VertexBuffers.emplace_back(VertexBufferLayout{
            .ArrayStride = stride,
            .StepMode = VertexStepMode::Vertex,
            .Elements = elements});
        psoDesc.Primitive = DefaultPrimitiveState();
        psoDesc.DepthStencil = DefaultDepthStencilState();
        psoDesc.MultiSample = DefaultMultiSampleState();
        psoDesc.ColorTargets.emplace_back(DefaultColorTargetState(TextureFormat::RGBA8_UNORM));
        psoDesc.DepthStencilEnable = true;
        psoDesc.Primitive.StripIndexFormat = ([&]() {
            switch (vd.IndexType) {
                case VertexIndexType::UInt16: return IndexFormat::UINT16;
                case VertexIndexType::UInt32: return IndexFormat::UINT32;
            }
        })();
        _pso = _device->CreateGraphicsPipeline(psoDesc).Unwrap();

        _cubeVbv = {_cubeVb.get(), (uint32_t)stride, 0};
        _cubeIbStride = ([&]() {
            switch (vd.IndexType) {
                case VertexIndexType::UInt16: return sizeof(uint16_t);
                case VertexIndexType::UInt32: return sizeof(uint32_t);
            }
        })();
        _cubeIbCount = vd.IndexCount;
    }

    void _SetupCubeMesh(const VertexData& vd) {
        _cubeVb = _device->CreateBuffer(
                             vd.VertexSize,
                             ResourceType::Buffer,
                             ResourceUsage::Default,
                             ResourceState::VertexAndConstantBuffer,
                             ResourceMemoryTip::None,
                             "cube_vb")
                      .Unwrap();
        _cubeIb = _device->CreateBuffer(
                             vd.IndexSize,
                             ResourceType::Buffer,
                             ResourceUsage::Default,
                             ResourceState::IndexBuffer,
                             ResourceMemoryTip::None,
                             "cube_ib")
                      .Unwrap();

        auto uploadVb = _device->CreateBuffer(
                                   vd.VertexSize,
                                   ResourceType::Buffer,
                                   ResourceUsage::Upload,
                                   ResourceState::GenericRead,
                                   ResourceMemoryTip::None)
                            .Unwrap();
        {
            auto ptr = uploadVb->Map(0, uploadVb->GetSize()).Unwrap();
            std::memcpy(ptr, vd.VertexData.get(), vd.VertexSize);
            uploadVb->Unmap();
        }
        auto uploadIb = _device->CreateBuffer(
                                   vd.IndexSize,
                                   ResourceType::Buffer,
                                   ResourceUsage::Upload,
                                   ResourceState::GenericRead,
                                   ResourceMemoryTip::None)
                            .Unwrap();
        {
            auto ptr = uploadIb->Map(0, uploadIb->GetSize()).Unwrap();
            std::memcpy(ptr, vd.IndexData.get(), vd.IndexSize);
            uploadIb->Unmap();
        }
        _cubeCb = _device->CreateBuffer(
                             sizeof(PreObject),
                             ResourceType::Buffer,
                             ResourceUsage::Upload,
                             ResourceState::GenericRead,
                             ResourceMemoryTip::None,
                             "cube_cb")
                      .Unwrap();
        _cubeCbMapped = _cubeCb->Map(0, _cubeCb->GetSize()).Unwrap();

        _cmdBuffer->Begin();
        {
            BufferBarrier barriers[] = {
                {_cubeVb.get(),
                 ResourceState::VertexAndConstantBuffer,
                 ResourceState::CopyDestination},
                {_cubeIb.get(),
                 ResourceState::IndexBuffer,
                 ResourceState::CopyDestination}};
            ResourceBarriers rb{barriers, {}};
            _cmdBuffer->ResourceBarrier(rb);
        }
        _cmdBuffer->CopyBuffer(uploadVb.get(), 0, _cubeVb.get(), 0, vd.VertexSize);
        _cmdBuffer->CopyBuffer(uploadIb.get(), 0, _cubeIb.get(), 0, vd.IndexSize);
        {
            BufferBarrier barriers[] = {
                {_cubeVb.get(),
                 ResourceState::CopyDestination,
                 ResourceState::VertexAndConstantBuffer},
                {_cubeIb.get(),
                 ResourceState::CopyDestination,
                 ResourceState::IndexBuffer}};
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
            DrawCube();
            std::this_thread::yield();
        }
    }

    unique_ptr<GlfwWindow> _window;

    shared_ptr<Device> _device;
    shared_ptr<CommandBuffer> _cmdBuffer;
    shared_ptr<SwapChain> _swapchain;
    shared_ptr<Dxc> _dxc;

    shared_ptr<Texture> _depthTex;
    shared_ptr<ResourceView> _depthView;

    shared_ptr<Buffer> _cubeVb;
    VertexBufferView _cubeVbv;
    shared_ptr<Buffer> _cubeIb;
    uint32_t _cubeIbStride;
    uint32_t _cubeIbCount;
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
        // if (_camCtrl.canOrbit) {
        //     Eigen::Matrix3f rotMat = _camRot.toRotationMatrix();
        //     Eigen::Vector3f radian = rotMat.eulerAngles(2, 0, 1);
        //     RADRAY_INFO_LOG("x:{} ,y:{} ,z:{}", Degree(radian.x()), Degree(radian.y()), Degree(radian.z()));
        // }
        Eigen::Vector3f front = (_camRot * Eigen::Vector3f{0, 0, 1}).normalized();
        Eigen::Vector3f up = (_camRot * Eigen::Vector3f{0, 1, 0}).normalized();
        _view = LookAtFrontLH(_camPos, front, up);
        _proj = PerspectiveLH(Radian(_fovDeg), WIN_WIDTH / static_cast<float>(WIN_HEIGHT), _zNear, _zFar);
    }

    void DrawCube() {
        _cmdBuffer->Begin();
        Texture* rt = _swapchain->GetCurrentRenderTarget();
        {
            TextureBarrier barriers[] = {
                {rt,
                 ResourceState::Present,
                 ResourceState::RenderTarget,
                 0,
                 0,
                 false},
                {
                    _depthTex.get(),
                    ResourceState::Common,
                    ResourceState::DepthWrite,
                    0,
                    0,
                    false,
                }};
            ResourceBarriers rb{{}, barriers};
            _cmdBuffer->ResourceBarrier(rb);
        }
        auto rtView = _device->CreateTextureView(
                                 rt,
                                 ResourceType::RenderTarget,
                                 TextureFormat::RGBA8_UNORM,
                                 TextureDimension::Dim2D,
                                 0,
                                 0,
                                 0,
                                 0)
                          .Unwrap();
        RenderPassDesc rpDesc{};
        rpDesc.Name = "normal";
        ColorAttachment colors[] = {DefaultColorAttachment(rtView.get(), {0.0f, 0.2f, 0.4f, 1.0f})};
        rpDesc.ColorAttachments = colors;
        rpDesc.DepthStencilAttachment = DefaultDepthStencilAttachment(_depthView.get());
        radray::unique_ptr<CommandEncoder> pass = _cmdBuffer->BeginRenderPass(rpDesc).Unwrap();
        pass->SetViewport({0.0f, 0.0f, WIN_WIDTH, WIN_HEIGHT, 0.0f, 1.0f});
        pass->SetScissor({0, 0, WIN_WIDTH, WIN_HEIGHT});
        pass->BindRootSignature(_rootSig.get());
        pass->BindPipelineState(_pso.get());
        Eigen::Affine3f m{Eigen::Translation3f(_cubePos)};
        PreObject preObj{};
        preObj.model = m.matrix();
        preObj.mvp = _proj * _view * preObj.model;
        pass->PushConstants(0, &preObj, sizeof(preObj));
        VertexBufferView vbv[] = {_cubeVbv};
        pass->BindVertexBuffers(vbv);
        pass->BindIndexBuffer(_cubeIb.get(), _cubeIbStride, 0);
        pass->DrawIndexed(_cubeIbCount, 0, 0);
        _cmdBuffer->EndRenderPass(std::move(pass));
        {
            TextureBarrier barriers[] = {
                {rt,
                 ResourceState::RenderTarget,
                 ResourceState::Present,
                 0,
                 0,
                 false},
                {
                    _depthTex.get(),
                    ResourceState::DepthWrite,
                    ResourceState::Common,
                    0,
                    0,
                    false,
                }};
            ResourceBarriers rb{{}, barriers};
            _cmdBuffer->ResourceBarrier(rb);
        }
        _cmdBuffer->End();
        CommandBuffer* t[] = {_cmdBuffer.get()};
        CommandQueue* cmdQueue = _device->GetCommandQueue(QueueType::Direct, 0).Unwrap();
        cmdQueue->Submit(t, Nullable<Fence>{});
        _swapchain->Present();
        cmdQueue->Wait();
    }
};

int main() {
    TestApp1 app{};
    app.Start();
    app.Update();
    return 0;
}
