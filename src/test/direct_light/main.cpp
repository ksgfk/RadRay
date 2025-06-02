#include <thread>
#include <fstream>
#include <filesystem>

#include <radray/logger.h>
#include <radray/utility.h>
#include <radray/triangle_mesh.h>
#include <radray/vertex_data.h>
#include <radray/camera_control.h>
#include <radray/image_data.h>
#include <radray/wavefront_obj.h>

#include <radray/window/glfw_window.h>

#include <radray/render/device.h>
#include <radray/render/resource.h>
#include <radray/render/command_queue.h>
#include <radray/render/command_buffer.h>
#include <radray/render/command_encoder.h>
#include <radray/render/swap_chain.h>
#include <radray/render/descriptor_set.h>
#include <radray/render/dxc.h>
#include <radray/render/tool_utility.h>

using namespace radray;
using namespace radray::render;
using namespace radray::window;

constexpr auto WIN_WIDTH = 1280;
constexpr auto WIN_HEIGHT = 720;

struct PreObject {
    Eigen::Matrix4f mvp;
    Eigen::Matrix4f model;
};

struct alignas(16) ObjectMaterial {
    Eigen::Vector3f baseColor;
};

class TestMesh {
public:
    u8string name;

    shared_ptr<Buffer> _vb;
    shared_ptr<Buffer> _ib;
    shared_ptr<Buffer> _upVb;
    shared_ptr<Buffer> _upIb;

    shared_ptr<Buffer> _cb;
    shared_ptr<ResourceView> _cbView;

    shared_ptr<RootSignature> _rs;
    shared_ptr<GraphicsPipelineState> _pso;

    uint32_t _vertexIndexCount = 0;
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
        _cubeBaseColorView = nullptr;
        _cubeBaseColor = nullptr;
        _cubeDescSet = nullptr;
        _rootSig = nullptr;
        _pso = nullptr;
        _meshes.clear();

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
            false};
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
        _camPos = Eigen::Vector3f(2.78f, 2.73f, 7.5f);
        _camRot = Eigen::AngleAxisf{Radian(180.0f), Eigen::Vector3f::UnitY()};
        _camCtrl.Distance = std::abs(_camPos.z());
        _fovDeg = 54.4322f;
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
        _SetupCubeTexture();
    }

    void _SetupCubeMaterial(const VertexData& vd) {
        radray::string color = ReadText(std::filesystem::path("shaders") / RADRAY_APPNAME / "baseColor.hlsl").value();
        std::string_view defines[] = {"BASE_COLOR_USE_TEXTURE"};
        DxcOutput outv = _dxc->Compile(
                                 color,
                                 "VSMain",
                                 ShaderStage::Vertex,
                                 HlslShaderModel::SM60,
                                 true,
                                 defines,
                                 {},
                                 false)
                             .value();
        DxilReflection reflv = _dxc->GetDxilReflection(ShaderStage::Vertex, outv.Refl).value();
        shared_ptr<Shader> vs = _device->CreateShader(
                                           outv.Data,
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
                                 defines,
                                 {},
                                 false)
                             .value();
        DxilReflection reflp = _dxc->GetDxilReflection(ShaderStage::Pixel, outp.Refl).value();
        shared_ptr<Shader> ps = _device->CreateShader(
                                           outp.Data,
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
        RootDescriptorInfo rdInfos[] = {
            RootDescriptorInfo{1, 0, ResourceType::CBuffer, ShaderStage::Graphics}};
        DescriptorSetInfo dsInfos[] = {
            {{DescriptorSetElementInfo{0, 0, 1, ResourceType::Texture, ShaderStage::Graphics}}}};
        StaticSamplerInfo ssInfos[] = {
            {0, 0, ShaderStage::Graphics, AddressMode::ClampToEdge, AddressMode::ClampToEdge,
             AddressMode::ClampToEdge, FilterMode::Linear, FilterMode::Linear,
             FilterMode::Linear, 0.0f, 0.0f, CompareFunction::Never, 1, false}};
        rsDesc.RootConstants = rsInfos;
        rsDesc.RootDescriptors = rdInfos;
        rsDesc.DescriptorSets = dsInfos;
        rsDesc.StaticSamplers = ssInfos;
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

    void _SetupCubeTexture() {
        auto filename = std::filesystem::path{"assets"} / "wall.png";
        std::ifstream file{filename, std::ios::binary};
        radray::ImageData png = radray::LoadPNG(file).value();
        if (png.Format == radray::ImageFormat::RGB8_BYTE) {
            png = png.RGB8ToRGBA8(0xff);
        }
        _cubeBaseColor = _device->CreateTexture(
                                    png.Width,
                                    png.Height,
                                    1,
                                    1,
                                    ImageToTextureFormat(png.Format),
                                    0,
                                    1,
                                    0,
                                    ColorClearValue{},
                                    ResourceType::Texture,
                                    ResourceState::Common,
                                    ResourceMemoryTip::Dedicated,
                                    "wall_tex")
                             .Unwrap();
        uint64_t needSize = _cubeBaseColor->GetUploadNeedSize(0, 0, 1);
        auto upload = _device->CreateBuffer(
                                 needSize,
                                 ResourceType::Buffer,
                                 ResourceUsage::Upload,
                                 ResourceState::GenericRead,
                                 ResourceMemoryTip::None)
                          .Unwrap();
        _cubeBaseColor->HelpCopyDataToUpload(upload.get(), png.Data.get(), png.GetSize(), 0, 0, 1);
        _cmdBuffer->Begin();
        {
            TextureBarrier barriers[] = {
                {_cubeBaseColor.get(),
                 ResourceState::Common,
                 ResourceState::CopyDestination,
                 0, 0, false}};
            ResourceBarriers rb{{}, barriers};
            _cmdBuffer->ResourceBarrier(rb);
        }
        _cmdBuffer->CopyTexture(upload.get(), 0, _cubeBaseColor.get(), 0, 0, 1);
        {
            TextureBarrier barriers[] = {
                {_cubeBaseColor.get(),
                 ResourceState::CopyDestination,
                 ResourceState::Common,
                 0, 0, false}};
            ResourceBarriers rb{{}, barriers};
            _cmdBuffer->ResourceBarrier(rb);
        }
        _cmdBuffer->End();
        CommandBuffer* t[] = {_cmdBuffer.get()};
        auto cmdQueue = _device->GetCommandQueue(QueueType::Direct, 0).Unwrap();
        cmdQueue->Submit(t, nullptr);
        cmdQueue->Wait();

        _cubeBaseColorView = _device->CreateTextureView(
                                        _cubeBaseColor.get(),
                                        ResourceType::Texture,
                                        TextureFormat::RGBA8_UNORM,
                                        TextureDimension::Dim2D,
                                        0,
                                        0,
                                        0,
                                        1)
                                 .Unwrap();

        DescriptorSetElementInfo info{0, 0, 1, ResourceType::Texture, ShaderStage::Graphics};
        _cubeDescSet = _device->CreateDescriptorSet(info).Unwrap();
    }

    void SetupCbox() {
        {
            auto v = std::filesystem::path{"assets"} / "box.obj";
            WavefrontObjReader reader{v};
            reader.Read();
            if (reader.HasError()) {
                RADRAY_ERR_LOG("{}", reader.Error());
            }
            for (const auto& obj : reader.Objects()) {
                TestMesh tm{};
                tm.name = obj.Name;
                TriangleMesh mesh{};
                reader.ToTriangleMesh(obj.Name, &mesh);
                VertexData meshVd{};
                mesh.ToVertexData(&meshVd);
                auto vbName = tm.name + u8"_vb";
                tm._vb = _device->CreateBuffer(
                                    meshVd.VertexSize,
                                    ResourceType::Buffer,
                                    ResourceUsage::Default,
                                    ResourceState::VertexAndConstantBuffer,
                                    ResourceMemoryTip::None,
                                    std::string_view{(char*)vbName.data(), vbName.size()})
                             .Unwrap();
                auto ibName = tm.name + u8"_ib";
                tm._ib = _device->CreateBuffer(
                                    meshVd.IndexSize,
                                    ResourceType::Buffer,
                                    ResourceUsage::Default,
                                    ResourceState::IndexBuffer,
                                    ResourceMemoryTip::None,
                                    std::string_view{(char*)ibName.data(), ibName.size()})
                             .Unwrap();
                tm._upVb = _device->CreateBuffer(
                                      meshVd.VertexSize,
                                      ResourceType::Buffer,
                                      ResourceUsage::Upload,
                                      ResourceState::GenericRead,
                                      ResourceMemoryTip::None)
                               .Unwrap();
                auto ptrVb = tm._upVb->Map(0, tm._upVb->GetSize()).Unwrap();
                std::memcpy(ptrVb, meshVd.VertexData.get(), meshVd.VertexSize);
                tm._upVb->Unmap();
                tm._upIb = _device->CreateBuffer(
                                      meshVd.IndexSize,
                                      ResourceType::Buffer,
                                      ResourceUsage::Upload,
                                      ResourceState::GenericRead,
                                      ResourceMemoryTip::None)
                               .Unwrap();
                auto ptrIb = tm._upIb->Map(0, tm._upIb->GetSize()).Unwrap();
                std::memcpy(ptrIb, meshVd.IndexData.get(), meshVd.IndexSize);
                tm._upIb->Unmap();
                tm._vertexIndexCount = meshVd.IndexCount;
                _meshes.emplace_back(std::move(tm));
            }
            vector<BufferBarrier> barriers;
            for (const auto& mesh : _meshes) {
                barriers.emplace_back(BufferBarrier{
                    mesh._vb.get(),
                    ResourceState::VertexAndConstantBuffer,
                    ResourceState::CopyDestination});
                barriers.emplace_back(BufferBarrier{
                    mesh._ib.get(),
                    ResourceState::IndexBuffer,
                    ResourceState::CopyDestination});
            }
            _cmdBuffer->Begin();
            ResourceBarriers rb{barriers, {}};
            _cmdBuffer->ResourceBarrier(rb);
            for (const auto& mesh : _meshes) {
                _cmdBuffer->CopyBuffer(mesh._upVb.get(), 0, mesh._vb.get(), 0, mesh._upVb->GetSize());
                _cmdBuffer->CopyBuffer(mesh._upIb.get(), 0, mesh._ib.get(), 0, mesh._upIb->GetSize());
            }
            barriers.clear();
            for (const auto& mesh : _meshes) {
                barriers.emplace_back(BufferBarrier{
                    mesh._vb.get(),
                    ResourceState::CopyDestination,
                    ResourceState::VertexAndConstantBuffer});
                barriers.emplace_back(BufferBarrier{
                    mesh._ib.get(),
                    ResourceState::CopyDestination,
                    ResourceState::IndexBuffer});
            }
            rb = {barriers, {}};
            _cmdBuffer->ResourceBarrier(rb);
            _cmdBuffer->End();
            CommandBuffer* t[] = {_cmdBuffer.get()};
            auto cmdQueue = _device->GetCommandQueue(QueueType::Direct, 0).Unwrap();
            cmdQueue->Submit(t, nullptr);
            cmdQueue->Wait();
        }
        {
            RootSignatureDescriptor rsDesc{};
            RootConstantInfo rsInfos[] = {RootConstantInfo{0, 0, sizeof(PreObject), ShaderStage::Vertex}};
            RootDescriptorInfo rdInfos[] = {
                RootDescriptorInfo{1, 0, ResourceType::CBuffer, ShaderStage::Graphics}};
            StaticSamplerInfo ssInfos[] = {
                {0, 0, ShaderStage::Graphics, AddressMode::ClampToEdge, AddressMode::ClampToEdge,
                 AddressMode::ClampToEdge, FilterMode::Linear, FilterMode::Linear,
                 FilterMode::Linear, 0.0f, 0.0f, CompareFunction::Never, 1, false}};
            rsDesc.RootConstants = rsInfos;
            rsDesc.RootDescriptors = rdInfos;
            rsDesc.StaticSamplers = ssInfos;
            auto setColor = _device->CreateRootSignature(rsDesc).Unwrap();
            radray::string color = ReadText(std::filesystem::path("shaders") / RADRAY_APPNAME / "baseColor.hlsl").value();
            shared_ptr<Shader> setColorVS, setColorPS;
            {
                DxcOutput outv = _dxc->Compile(color, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, true, {}, {}, false).value();
                DxcOutput outp = _dxc->Compile(color, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, true, {}, {}, false).value();
                setColorVS = _device->CreateShader(outv.Data, ShaderBlobCategory::DXIL, ShaderStage::Vertex, "VSMain", "setColorVS").Unwrap();
                setColorPS = _device->CreateShader(outp.Data, ShaderBlobCategory::DXIL, ShaderStage::Pixel, "PSMain", "setColorPS").Unwrap();
            }
            radray::shared_ptr<GraphicsPipelineState> pntPipe;
            {
                GraphicsPipelineStateDescriptor psoDesc{};
                psoDesc.RootSig = setColor.get();
                psoDesc.VS = setColorVS.get();
                psoDesc.PS = setColorPS.get();
                vector<VertexElement> elements = {
                    VertexElement{0, "POSITION", 0, VertexFormat::FLOAT32X3, 0},
                    VertexElement{12, "NORMAL", 0, VertexFormat::FLOAT32X3, 1},
                    VertexElement{24, "TEXCOORD", 0, VertexFormat::FLOAT32X2, 2}};
                psoDesc.VertexBuffers.emplace_back(VertexBufferLayout{
                    .ArrayStride = 32,
                    .StepMode = VertexStepMode::Vertex,
                    .Elements = elements});
                psoDesc.Primitive = DefaultPrimitiveState();
                psoDesc.DepthStencil = DefaultDepthStencilState();
                psoDesc.MultiSample = DefaultMultiSampleState();
                psoDesc.ColorTargets.emplace_back(DefaultColorTargetState(TextureFormat::RGBA8_UNORM));
                psoDesc.DepthStencilEnable = true;
                psoDesc.Primitive.StripIndexFormat = IndexFormat::UINT16;
                pntPipe = _device->CreateGraphicsPipeline(psoDesc).Unwrap();
            }
            Eigen::Vector3f colors[] = {
                {0.5f, 0.5f, 0.5f},
                {0.5f, 0.5f, 0.5f},
                {0.5f, 0.5f, 0.5f},
                {1.0f, 0.0f, 0.0f},
                {0.0f, 0.0f, 1.0f},
                {0.5f, 0.7f, 0.3f},
                {0.2f, 0.3f, 0.6f}};
            size_t i = 0;
            for (auto& mesh : _meshes) {
                mesh._rs = setColor;
                mesh._pso = pntPipe;

                auto cbName = mesh.name + u8"_cb";
                mesh._cb = _device->CreateBuffer(
                                      sizeof(ObjectMaterial),
                                      ResourceType::Buffer,
                                      ResourceUsage::Upload,
                                      ResourceState::GenericRead,
                                      ResourceMemoryTip::None,
                                      std::string_view{(char*)cbName.data(), cbName.size()})
                               .Unwrap();
                auto ptrCb = mesh._cb->Map(0, sizeof(ObjectMaterial)).Unwrap();
                ObjectMaterial mat{
                    colors[i % ArrayLength(colors)]};
                i = (i + 1) % ArrayLength(colors);
                std::memcpy(ptrCb, &mat, sizeof(ObjectMaterial));
                mesh._cb->Unmap();
                mesh._cbView = _device->CreateBufferView(
                                          mesh._cb.get(),
                                          ResourceType::CBuffer,
                                          TextureFormat::UNKNOWN,
                                          0,
                                          0,
                                          0)
                                   .Unwrap();
            }
        }
    }

    void Start() {
        ShowWindow();
        InitGraphics();
        SetupCamera();
        SetupCube();
        SetupCbox();
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
    shared_ptr<Texture> _cubeBaseColor;
    shared_ptr<ResourceView> _cubeBaseColorView;
    shared_ptr<DescriptorSet> _cubeDescSet;

    shared_ptr<RootSignature> _rootSig;
    shared_ptr<GraphicsPipelineState> _pso;

    vector<TestMesh> _meshes;

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
            _camCtrl.NowPos = xy;
            if (action == ACTION_PRESSED) {
                _camCtrl.CanOrbit = true;
                _camCtrl.LastPos = xy;
            } else if (action == ACTION_RELEASED) {
                _camCtrl.CanOrbit = false;
            }
        }
        RADRAY_UNUSED(modifiers);
    }

    void OnMouseMove(const Eigen::Vector2f& xy) {
        if (_camCtrl.CanOrbit) {
            _camCtrl.NowPos = xy;
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
        // pass->BindRootSignature(_rootSig.get());
        // pass->BindPipelineState(_pso.get());
        // Eigen::Affine3f m{Eigen::Translation3f(_cubePos)};
        // PreObject preObj{};
        // preObj.model = m.matrix();
        // preObj.mvp = _proj * _view * preObj.model;
        // pass->PushConstants(0, &preObj, sizeof(preObj));
        // ResourceView* texViews[] = {_cubeBaseColorView.get()};
        // _cubeDescSet->SetResources(0, texViews);
        // pass->BindDescriptorSet(0, _cubeDescSet.get());
        // VertexBufferView vbv[] = {_cubeVbv};
        // pass->BindVertexBuffers(vbv);
        // pass->BindIndexBuffer(_cubeIb.get(), _cubeIbStride, 0);
        // pass->DrawIndexed(_cubeIbCount, 0, 0);

        for (const auto& mesh : _meshes) {
            pass->BindRootSignature(mesh._rs.get());
            pass->BindPipelineState(mesh._pso.get());
            Eigen::Affine3f m{Eigen::Translation3f(0, 0, 0)};
            PreObject preObj{};
            preObj.model = m.matrix();
            preObj.mvp = _proj * _view * preObj.model;
            pass->PushConstants(0, &preObj, sizeof(preObj));
            pass->BindRootDescriptor(0, mesh._cbView.get());
            VertexBufferView vbv[] = {{mesh._vb.get(), 32, 0}};
            pass->BindVertexBuffers(vbv);
            pass->BindIndexBuffer(mesh._ib.get(), 2, 0);
            pass->DrawIndexed(mesh._vertexIndexCount, 0, 0);
        }

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
