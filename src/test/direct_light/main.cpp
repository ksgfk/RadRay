#include <thread>
#include <fstream>
#include <filesystem>
#include <chrono>

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

using namespace radray;
using namespace radray::render;
using namespace radray::window;

constexpr auto WIN_WIDTH = 1280;
constexpr auto WIN_HEIGHT = 720;

struct PreObject {
    Eigen::Matrix4f mvp;
    Eigen::Matrix4f model;
    Eigen::Vector3f cameraPos;
};

struct ObjectMaterial {
    Eigen::Vector3f baseColor;
    float _pad0;
    float metallic;
    float roughness;
    float _pad1;
    float _pad2;
};

struct PointLight {
    Eigen::Vector3f posW;
    float _pad0;
    Eigen::Vector3f color;
    float intensity;
};

constexpr auto CBUFFER_SIZE = 3 * 256;

class TestMesh {
public:
    u8string name;

    shared_ptr<Buffer> _vb;
    shared_ptr<Buffer> _ib;
    shared_ptr<Buffer> _upVb;
    shared_ptr<Buffer> _upIb;

    shared_ptr<Buffer> _cb;
    void *_cbPreObjMapped = nullptr, *_cbObjMatMapped = nullptr, *_cbPLightMapped = nullptr;
    shared_ptr<ResourceView> _cbPreObjView, _cbObjMatView, _cbPLightView;

    shared_ptr<RootSignature> _rs;
    shared_ptr<GraphicsPipelineState> _pso;

    uint32_t _vertexIndexCount = 0;
    uint32_t _ibStride = 0;

    shared_ptr<ResourceView> _baseColorView;
    shared_ptr<ResourceView> _normalMapView;
    shared_ptr<DescriptorSet> _descSet;

    Eigen::Vector3f _pos = Eigen::Vector3f::Zero();
    Eigen::Vector3f baseColor;
    float metallic = 0.0f;
    float roughness = 0.6f;
};

class TestApp1 {
public:
    TestApp1() {
        RADRAY_INFO_LOG("{} start", RADRAY_APPNAME);
        GlobalInitGlfw();
    }

    ~TestApp1() {
        _rootSig = nullptr;
        _pso = nullptr;
        _textures.clear();
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
                               ResourceHint::None)
                        .Unwrap();
        _depthView = _device->CreateTextureView(
                                _depthTex.get(),
                                ResourceType::DepthStencil,
                                TextureFormat::D24_UNORM_S8_UINT,
                                TextureViewDimension::Dim2D,
                                0,
                                0,
                                0,
                                0)
                         .Unwrap();
    }

    void SetupCamera() {
        _camPos = Eigen::Vector3f(0, 0, -1.8f);
        _camRot = Eigen::AngleAxisf{Radian(0.0f), Eigen::Vector3f::UnitY()};
        _camCtrl.Distance = std::abs(_camPos.z());
        _fovDeg = 60.0f;
        _zNear = 0.3f;
        _zFar = 1000.0f;
        _onMouseClick = {&TestApp1::OnMouseClick, this, _window->EventMouseButtonCall()};
        _onMouseMove = {&TestApp1::OnMouseMove, this, _window->EventCursorPosition()};
    }

    void SetupCbox() {
        DescriptorSetElementInfo texs{0, 0, 2, ResourceType::Texture, ShaderStage::Graphics};
        DescriptorSetInfo dsInfos[] = {{{texs}}};
        VertexIndexType vit;
        {
            auto baseColorPath = std::filesystem::path{"assets"} / "sutr_tmave_sedy.png";
            std::ifstream baseColorFile{baseColorPath, std::ios::binary | std::ios::in};
            radray::ImageData baseColorData = radray::LoadPNG(baseColorFile, {0xFF, true}).value();
            auto baseColorTex = _device->CreateTexture(
                                           baseColorData.Width,
                                           baseColorData.Height,
                                           1,
                                           1,
                                           ImageToTextureFormat(baseColorData.Format),
                                           0,
                                           1,
                                           0,
                                           ColorClearValue{},
                                           ResourceType::Texture,
                                           ResourceState::Common,
                                           ResourceHint::Dedicated,
                                           "baseColor_tex")
                                    .Unwrap();
            _textures.emplace_back(baseColorTex);
            uint64_t baseColorUploadSize = _device->GetUploadBufferNeedSize(baseColorTex.get(), 0, 0, 1);
            auto baseColorUpload = _device->CreateBuffer(
                                              baseColorUploadSize,
                                              ResourceType::Buffer,
                                              ResourceMemoryUsage::Upload,
                                              ResourceState::GenericRead,
                                              ResourceHint::None)
                                       .Unwrap();
            _device->CopyDataToUploadBuffer(baseColorUpload.get(), baseColorData.Data.get(), baseColorData.GetSize(), 0, 0, 1);

            auto normalMapPath = std::filesystem::path{"assets"} / "sutr_tmave_sedy_NormalsMap.png";
            std::ifstream normalMapFile{normalMapPath, std::ios::binary | std::ios::in};
            radray::ImageData normalMapData = radray::LoadPNG(normalMapFile, {0xFF, true}).value();
            auto normalMapTex = _device->CreateTexture(
                                           normalMapData.Width,
                                           normalMapData.Height,
                                           1,
                                           1,
                                           ImageToTextureFormat(normalMapData.Format),
                                           0,
                                           1,
                                           0,
                                           ColorClearValue{},
                                           ResourceType::Texture,
                                           ResourceState::Common,
                                           ResourceHint::Dedicated,
                                           "baseColor_tex")
                                    .Unwrap();
            _textures.emplace_back(normalMapTex);
            uint64_t normalMapUploadSize = _device->GetUploadBufferNeedSize(normalMapTex.get(), 0, 0, 1);
            auto normalMapUpload = _device->CreateBuffer(
                                              normalMapUploadSize,
                                              ResourceType::Buffer,
                                              ResourceMemoryUsage::Upload,
                                              ResourceState::GenericRead,
                                              ResourceHint::None)
                                       .Unwrap();
            _device->CopyDataToUploadBuffer(normalMapUpload.get(), normalMapData.Data.get(), normalMapData.GetSize(), 0, 0, 1);

            auto modelPath = std::filesystem::path{"assets"} / "sutr_tmave_sedy.obj";
            WavefrontObjReader reader{modelPath};
            reader.Read();
            if (reader.HasError()) {
                RADRAY_ERR_LOG("{}", reader.Error());
            }

            TestMesh tm{};
            tm.name = u8"sutr_tmave_sedy";
            TriangleMesh mesh{};
            reader.ToTriangleMesh(&mesh);
            VertexData meshVd{};
            mesh.ToVertexData(&meshVd);
            vit = meshVd.IndexType;
            auto vbName = tm.name + u8"_vb";
            tm._vb = _device->CreateBuffer(
                                meshVd.VertexSize,
                                ResourceType::Buffer,
                                ResourceMemoryUsage::Default,
                                ResourceState::VertexAndConstantBuffer,
                                ResourceHint::None,
                                std::string_view{(char*)vbName.data(), vbName.size()})
                         .Unwrap();
            auto ibName = tm.name + u8"_ib";
            tm._ib = _device->CreateBuffer(
                                meshVd.IndexSize,
                                ResourceType::Buffer,
                                ResourceMemoryUsage::Default,
                                ResourceState::IndexBuffer,
                                ResourceHint::None,
                                std::string_view{(char*)ibName.data(), ibName.size()})
                         .Unwrap();
            tm._upVb = _device->CreateBuffer(
                                  meshVd.VertexSize,
                                  ResourceType::Buffer,
                                  ResourceMemoryUsage::Upload,
                                  ResourceState::GenericRead,
                                  ResourceHint::None)
                           .Unwrap();
            auto ptrVb = tm._upVb->Map(0, tm._upVb->GetSize()).Unwrap();
            std::memcpy(ptrVb, meshVd.VertexData.get(), meshVd.VertexSize);
            tm._upVb->Unmap();
            tm._upIb = _device->CreateBuffer(
                                  meshVd.IndexSize,
                                  ResourceType::Buffer,
                                  ResourceMemoryUsage::Upload,
                                  ResourceState::GenericRead,
                                  ResourceHint::None)
                           .Unwrap();
            auto ptrIb = tm._upIb->Map(0, tm._upIb->GetSize()).Unwrap();
            std::memcpy(ptrIb, meshVd.IndexData.get(), meshVd.IndexSize);
            tm._upIb->Unmap();
            tm._vertexIndexCount = meshVd.IndexCount;
            tm._ibStride = ([&]() {
                switch (vit) {
                    case VertexIndexType::UInt16: return sizeof(uint16_t);
                    case VertexIndexType::UInt32: return sizeof(uint32_t);
                }
            })();
            _meshes.emplace_back(std::move(tm));

            vector<BufferBarrier> bufBarriers;
            vector<TextureBarrier> texBarriers;
            for (const auto& mesh : _meshes) {
                bufBarriers.emplace_back(BufferBarrier{
                    mesh._vb.get(),
                    ResourceState::VertexAndConstantBuffer,
                    ResourceState::CopyDestination});
                bufBarriers.emplace_back(BufferBarrier{
                    mesh._ib.get(),
                    ResourceState::IndexBuffer,
                    ResourceState::CopyDestination});
            }
            texBarriers.emplace_back(TextureBarrier{
                baseColorTex.get(),
                ResourceState::Common,
                ResourceState::CopyDestination,
                0, 0, false});
            texBarriers.emplace_back(TextureBarrier{
                normalMapTex.get(),
                ResourceState::Common,
                ResourceState::CopyDestination,
                0, 0, false});
            _cmdBuffer->Begin();
            ResourceBarriers rb{bufBarriers, texBarriers};
            _cmdBuffer->ResourceBarrier(rb);
            for (const auto& mesh : _meshes) {
                _cmdBuffer->CopyBuffer(mesh._upVb.get(), 0, mesh._vb.get(), 0, mesh._upVb->GetSize());
                _cmdBuffer->CopyBuffer(mesh._upIb.get(), 0, mesh._ib.get(), 0, mesh._upIb->GetSize());
            }
            _cmdBuffer->CopyTexture(baseColorUpload.get(), 0, baseColorTex.get(), 0, 0, 1);
            _cmdBuffer->CopyTexture(normalMapUpload.get(), 0, normalMapTex.get(), 0, 0, 1);
            bufBarriers.clear();
            texBarriers.clear();
            for (const auto& mesh : _meshes) {
                bufBarriers.emplace_back(BufferBarrier{
                    mesh._vb.get(),
                    ResourceState::CopyDestination,
                    ResourceState::VertexAndConstantBuffer});
                bufBarriers.emplace_back(BufferBarrier{
                    mesh._ib.get(),
                    ResourceState::CopyDestination,
                    ResourceState::IndexBuffer});
            }
            texBarriers.emplace_back(TextureBarrier{
                baseColorTex.get(),
                ResourceState::CopyDestination,
                ResourceState::Common,
                0, 0, false});
            texBarriers.emplace_back(TextureBarrier{
                normalMapTex.get(),
                ResourceState::CopyDestination,
                ResourceState::Common,
                0, 0, false});
            rb = {bufBarriers, texBarriers};
            _cmdBuffer->ResourceBarrier(rb);
            _cmdBuffer->End();
            CommandBuffer* t[] = {_cmdBuffer.get()};
            auto cmdQueue = _device->GetCommandQueue(QueueType::Direct, 0).Unwrap();
            cmdQueue->Submit(t, nullptr);
            cmdQueue->Wait();
            auto baseColorView = _device->CreateTextureView(
                                            baseColorTex.get(),
                                            ResourceType::Texture,
                                            TextureFormat::RGBA8_UNORM,
                                            TextureViewDimension::Dim2D,
                                            0,
                                            0,
                                            0,
                                            1)
                                     .Unwrap();
            auto normalMapView = _device->CreateTextureView(
                                            normalMapTex.get(),
                                            ResourceType::Texture,
                                            TextureFormat::RGBA8_UNORM,
                                            TextureViewDimension::Dim2D,
                                            0,
                                            0,
                                            0,
                                            1)
                                     .Unwrap();
            auto descSet = _device->CreateDescriptorSet(texs).Unwrap();
            ResourceView* views[] = {baseColorView.get(), normalMapView.get()};
            descSet->SetResources(0, views);
            for (auto& mesh : _meshes) {
                mesh._upIb = nullptr;
                mesh._upVb = nullptr;
                mesh._baseColorView = baseColorView;
                mesh._normalMapView = normalMapView;
                mesh._descSet = descSet;
            }
        }
        {
            RootSignatureDescriptor rsDesc{};
            // RootConstantInfo rsInfos[] = {RootConstantInfo{0, 0, sizeof(PreObject), ShaderStage::Vertex}};
            RootDescriptorInfo rdInfos[] = {
                RootDescriptorInfo{0, 0, ResourceType::CBuffer, ShaderStage::Graphics},
                RootDescriptorInfo{1, 0, ResourceType::CBuffer, ShaderStage::Graphics},
                RootDescriptorInfo{2, 0, ResourceType::CBuffer, ShaderStage::Graphics}};
            StaticSamplerInfo ssInfos[] = {
                {0, 0, ShaderStage::Graphics, AddressMode::ClampToEdge, AddressMode::ClampToEdge,
                 AddressMode::ClampToEdge, FilterMode::Linear, FilterMode::Linear,
                 FilterMode::Linear, 0.0f, 0.0f, CompareFunction::Never, 1, false}};
            // rsDesc.RootConstants = rsInfos;
            rsDesc.RootDescriptors = rdInfos;
            rsDesc.DescriptorSets = dsInfos;
            rsDesc.StaticSamplers = ssInfos;
            auto setColor = _device->CreateRootSignature(rsDesc).Unwrap();
            string color = ReadText(std::filesystem::path("shaders") / RADRAY_APPNAME / "baseColor.hlsl").value();
            shared_ptr<Shader> setColorVS, setColorPS;
            {
                std::string_view defines[] = {"BASE_COLOR_USE_TEXTURE"};
                DxcOutput outv = _dxc->Compile(color, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, true, defines, {}, false).value();
                DxcOutput outp = _dxc->Compile(color, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, true, defines, {}, false).value();
                setColorVS = _device->CreateShader(outv.Data, ShaderBlobCategory::DXIL, ShaderStage::Vertex, "VSMain", "setColorVS").Unwrap();
                setColorPS = _device->CreateShader(outp.Data, ShaderBlobCategory::DXIL, ShaderStage::Pixel, "PSMain", "setColorPS").Unwrap();
            }
            shared_ptr<GraphicsPipelineState> pntPipe;
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
                psoDesc.Primitive.Cull = CullMode::None;
                psoDesc.Primitive.StripIndexFormat = ([&]() {
                    switch (vit) {
                        case VertexIndexType::UInt16: return IndexFormat::UINT16;
                        case VertexIndexType::UInt32: return IndexFormat::UINT32;
                    }
                })();
                pntPipe = _device->CreateGraphicsPipeline(psoDesc).Unwrap();
            }
            // Eigen::Vector3f colors[] = {
            //     {0.5f, 0.5f, 0.5f},
            //     {0.5f, 0.5f, 0.5f},
            //     {0.5f, 0.5f, 0.5f},
            //     {1.0f, 0.0f, 0.0f},
            //     {0.0f, 0.0f, 1.0f},
            //     {0.5f, 0.7f, 0.3f},
            //     {0.2f, 0.3f, 0.6f}};
            // size_t i = 0;
            for (auto& mesh : _meshes) {
                mesh._rs = setColor;
                mesh._pso = pntPipe;

                auto cbName = mesh.name + u8"_cb";
                mesh._cb = _device->CreateBuffer(
                                      CBUFFER_SIZE,
                                      ResourceType::Buffer,
                                      ResourceMemoryUsage::Upload,
                                      ResourceState::GenericRead,
                                      ResourceHint::None,
                                      std::string_view{(char*)cbName.data(), cbName.size()})
                               .Unwrap();
                // auto ptrCb = mesh._cb->Map(0, CBUFFER_SIZE).Unwrap();
                // ObjectMaterial mat{
                //     colors[i % ArrayLength(colors)]};
                // i = (i + 1) % ArrayLength(colors);
                // std::memcpy(ptrCb, &mat, sizeof(ObjectMaterial));
                // mesh._cb->Unmap();
                mesh._cbPreObjMapped = mesh._cb->Map(0, CBUFFER_SIZE).Unwrap();
                mesh._cbObjMatMapped = (byte*)mesh._cbPreObjMapped + 256;
                mesh._cbPLightMapped = (byte*)mesh._cbObjMatMapped + 256;
                mesh._cbPreObjView = _device->CreateBufferView(
                                                mesh._cb.get(),
                                                ResourceType::CBuffer,
                                                TextureFormat::UNKNOWN,
                                                0,
                                                0,
                                                0)
                                         .Unwrap();
                mesh._cbObjMatView = _device->CreateBufferView(
                                                mesh._cb.get(),
                                                ResourceType::CBuffer,
                                                TextureFormat::UNKNOWN,
                                                256,
                                                0,
                                                0)
                                         .Unwrap();
                mesh._cbPLightView = _device->CreateBufferView(
                                                mesh._cb.get(),
                                                ResourceType::CBuffer,
                                                TextureFormat::UNKNOWN,
                                                256 * 2,
                                                0,
                                                0)
                                         .Unwrap();
            }
        }
    }

    void SetupPLight() {
        _plight.posW = Eigen::Vector3f{1.5f, 0, 0};
        _plight.color = Eigen::Vector3f{1, 1, 1};
        _plight.intensity = 5.0f;

        _pLightInit = _plight.posW;
        _pLightRot = Eigen::Quaternionf::Identity();
        _pLightSpeed = 360.0f / 8;
    }

    void Start() {
        ShowWindow();
        InitGraphics();
        SetupCamera();
        // SetupCube();
        SetupCbox();
        SetupPLight();
    }

    void Update() {
        while (true) {
            auto now = std::chrono::high_resolution_clock::now();
            _deltaTime = std::chrono::duration<float>(now - _lastTime).count();
            _lastTime = now;

            GlobalPollEventsGlfw();
            if (_window->ShouldClose()) {
                break;
            }
            UpdateCamera();
            UpdateLight();
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

    shared_ptr<RootSignature> _rootSig;
    shared_ptr<GraphicsPipelineState> _pso;

    vector<TestMesh> _meshes;
    vector<shared_ptr<Texture>> _textures;

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

    PointLight _plight;
    Eigen::Vector3f _pLightInit;
    Eigen::Quaternionf _pLightRot = Eigen::Quaternionf::Identity();
    float _pLightSpeed;

    std::chrono::high_resolution_clock::time_point _lastTime = std::chrono::high_resolution_clock::now();
    float _deltaTime = 0;

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

    void UpdateLight() {
        Eigen::Quaternionf deltaQ(Eigen::AngleAxisf(Radian(_deltaTime * _pLightSpeed), Eigen::Vector3f::UnitY()));
        _pLightRot = deltaQ * _pLightRot;
        _pLightRot.normalize();
        _plight.posW = _pLightRot * _pLightInit;
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
                                 TextureViewDimension::Dim2D,
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
        unique_ptr<CommandEncoder> pass = _cmdBuffer->BeginRenderPass(rpDesc).Unwrap();
        pass->SetViewport({0.0f, 0.0f, WIN_WIDTH, WIN_HEIGHT, 0.0f, 1.0f});
        pass->SetScissor({0, 0, WIN_WIDTH, WIN_HEIGHT});

        for (const auto& mesh : _meshes) {
            pass->BindRootSignature(mesh._rs.get());
            pass->BindPipelineState(mesh._pso.get());
            Eigen::Affine3f m{Eigen::Translation3f(mesh._pos)};
            PreObject preObj{};
            preObj.model = m.matrix();
            preObj.mvp = _proj * _view;
            preObj.cameraPos = _camPos;
            std::memcpy(mesh._cbPreObjMapped, &preObj, sizeof(PreObject));
            ObjectMaterial mat{};
            mat.baseColor = mesh.baseColor;
            mat.metallic = mesh.metallic;
            mat.roughness = mesh.roughness;
            std::memcpy(mesh._cbObjMatMapped, &mat, sizeof(ObjectMaterial));
            std::memcpy(mesh._cbPLightMapped, &_plight, sizeof(PointLight));
            // pass->PushConstants(0, &preObj, sizeof(preObj));
            pass->BindRootDescriptor(0, mesh._cbPreObjView.get());
            pass->BindRootDescriptor(1, mesh._cbObjMatView.get());
            pass->BindRootDescriptor(2, mesh._cbPLightView.get());
            pass->BindDescriptorSet(0, mesh._descSet.get());
            VertexBufferView vbv[] = {{mesh._vb.get(), 32, 0}};
            pass->BindVertexBuffers(vbv);
            pass->BindIndexBuffer(mesh._ib.get(), mesh._ibStride, 0);
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
