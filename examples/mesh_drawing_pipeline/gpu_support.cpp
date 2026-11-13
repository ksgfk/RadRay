#include "gpu_support.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#include <radray/basic_math.h>
#include <radray/image_data.h>
#include <radray/logger.h>
#include <radray/scope_guard.h>

namespace radray::examples::mesh_drawing {
namespace {

Nullable<unique_ptr<render::Shader>> LoadShader(
    render::Device& device, const std::filesystem::path& path, render::ShaderStage stage) {
    std::ifstream file{path, std::ios::binary | std::ios::ate};
    if (!file || file.tellg() <= 0) {
        RADRAY_ERR_LOG("无法读取 shader：{}；请先显式运行 compile_shaders.py", path.string());
        return nullptr;
    }
    const auto size = static_cast<size_t>(file.tellg());
    vector<byte> bytes(size);
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        RADRAY_ERR_LOG("shader 文件读取不完整：{}", path.string());
        return nullptr;
    }
    const auto category = device.GetBackend() == render::RenderBackend::Vulkan
                              ? render::ShaderBlobCategory::SPIRV
                              : render::ShaderBlobCategory::DXIL;
    return device.CreateShader({bytes, category, stage});
}

}  // namespace

bool GpuResources::Initialize(render::RenderBackend backend, const std::filesystem::path& shaderDirectory) {
    RADRAY_INFO_LOG("准备 GPU：backend={}，请求开启后端验证，读取预编译 shader", backend);
    Env = Environment::Create(backend, true, "RadRay Mesh Drawing tutorial");
    if (!Env) {
        RADRAY_ERR_LOG("GPU 环境初始化失败，backend={}；不切换其他后端", backend);
        return false;
    }
    Device = DeviceContext::Create(*Env);
    if (!Device) {
        RADRAY_ERR_LOG("GPU 设备初始化失败，backend={}；请检查驱动、adapter 和验证层日志", backend);
        return false;
    }
    auto& device = Device->GetDevice();
    const auto suffix = backend == render::RenderBackend::Vulkan ? "spirv" : "dxil";
    VertexShader = LoadShader(device, shaderDirectory / radray::format("cube.VSMain.{}.bin", suffix), render::ShaderStage::Vertex);
    if (!VertexShader) return false;
    PixelShader = LoadShader(device, shaderDirectory / radray::format("cube.PSMain.{}.bin", suffix), render::ShaderStage::Pixel);
    if (!PixelShader) return false;

    // 步骤 M1：把离线编译并加载的 shader、布局和材质性质放进 MaterialResource。
    // 为什么：多个实例可共享同一组程序；shader 可用性与具体 Pass 的参与规则是两件事。
    // UE5 对照：Engine/Public/MaterialShared.h 的 FMaterial / FMaterialResource / FMaterialShaderMap。
    // 本例简化：具名程序与 Surface/Outline 标签由应用准备；标签不触发编译，也不是 UE 的固定材质枚举。
    const render::BindingDescriptor passBinding{
        .BindingId = 0,
        .Kind = render::BindingKind::CBuffer,
        .Count = 1,
        .Stages = render::ShaderStage::Vertex,
        .D3D12 = render::D3D12BindingDescriptor{render::BindingRegisterClass::CBV, 0, 0, render::D3D12BufferPlacement::Table},
        .Vulkan = render::VulkanBindingDescriptor{0, 0, render::VulkanBufferDescriptorPlacement::Regular}};
    PassGroupLayout = device.CreateBindingGroupLayout({std::span{&passBinding, 1}});
    if (!PassGroupLayout) return false;
    const render::PipelineLayoutGroupDescriptor passGroup{.GroupSlot = 0, .Layout = PassGroupLayout.Get()};
    const render::PushConstantDescriptor push{
        0, sizeof(MaterialConstants), render::ShaderStage::Pixel,
        render::PushConstantDescriptor::D3D12Location{1, 0}, render::PushConstantDescriptor::VulkanLocation{0}};
    Layout = device.CreatePipelineLayout({std::span{&passGroup, 1}, std::span{&push, 1}});
    if (!Layout) return false;
    OpaqueResource.Programs = {
        {"Depth", Layout.Get(), VertexShader.Get(), "VSMain", nullptr, ""},
        {"Color", Layout.Get(), VertexShader.Get(), "VSMain", PixelShader.Get(), "PSMain"}};
    const auto surface = MaterialTags.Intern("Surface");
    const auto opaque = MaterialTags.Intern("Opaque");
    const auto transparent = MaterialTags.Intern("Transparent");
    const auto outline = MaterialTags.Intern("Outline");
    const auto enabled = MaterialTags.Intern("Enabled");
    if (!surface || !opaque || !transparent || !outline || !enabled) return false;
    OpaqueTag = {*surface, *opaque};
    TransparentTag = {*surface, *transparent};
    OutlineTag = {*outline, *enabled};
    OpaqueResource.Properties.Tags = MaterialTagSet{MaterialTags};
    if (!OpaqueResource.Properties.Tags.Set(*surface, *opaque)) return false;
    TwoSidedResource = OpaqueResource;
    if (!TwoSidedResource.Properties.Tags.Set(*outline, *enabled)) return false;
    auto& twoSided = TwoSidedResource.Properties.RenderState;
    twoSided.Cull = render::CullMode::None;
    twoSided.Stencil = render::StencilState::Default();
    twoSided.Stencil->Front.PassOp = twoSided.Stencil->Back.PassOp = render::StencilOperation::Replace;
    twoSided.StencilReference = 7;
    TransparentResource = OpaqueResource;
    if (!TransparentResource.Properties.Tags.Set(*surface, *transparent)) return false;
    auto& translucent = TransparentResource.Properties.RenderState;
    translucent.DepthWriteEnable = false;
    translucent.Blend = render::BlendState{
        {render::BlendFactor::SrcAlpha, render::BlendFactor::OneMinusSrcAlpha, render::BlendOperation::Add},
        {render::BlendFactor::One, render::BlendFactor::OneMinusSrcAlpha, render::BlendOperation::Add}};
    RADRAY_INFO_LOG("材质标签登记：Surface=Opaque / Transparent，Outline=Enabled；描边物体写模板 7，透明材质启用 alpha 混合并关闭深度写入");

    return true;
}

bool GpuResources::PrepareGeometry(std::span<const byte> vertices, std::span<const uint32_t> indices) {
    // Scene 可以引用尚未写入的 Device 缓冲；字节由首帧 RDG 的 Staging Copy 上传。
    VertexBuffer = Device->GetDevice().CreateBuffer({.Size = vertices.size(), .Memory = render::MemoryType::Device, .Usage = render::BufferUse::Vertex | render::BufferUse::CopyDestination});
    IndexBuffer = Device->GetDevice().CreateBuffer({.Size = indices.size_bytes(), .Memory = render::MemoryType::Device, .Usage = render::BufferUse::Index | render::BufferUse::CopyDestination});
    PendingVertexBytes.assign(vertices.begin(), vertices.end());
    const auto indexBytes = std::as_bytes(indices);
    PendingIndexBytes.assign(indexBytes.begin(), indexBytes.end());
    return VertexBuffer && IndexBuffer;
}

bool GpuFrameResources::Initialize(const GpuResources& shared) {
    Width = shared.Width;
    Height = shared.Height;
    auto& device = shared.Device->GetDevice();
    RowPitch = Align(uint64_t{Width} * 4, std::max<uint64_t>(1, device.GetCapabilities().Detail.TextureDataPitchAlignment));
    Readback = device.CreateBuffer({.Size = RowPitch * Height, .Memory = render::MemoryType::ReadBack, .Usage = render::BufferUse::CopyDestination | render::BufferUse::MapRead});
    if (!Readback) return false;
    for (auto& parameters : PassParameterSets) {
        parameters = device.CreateShaderParameterSet({shared.PassGroupLayout.Get()});
        if (!parameters) return false;
    }
    CompletionFence = device.CreateFence();
    return CompletionFence.HasValue();
}

bool GpuResources::CreatePipelines(const VertexInput& input, const MaterialResource& resource) {
    // 步骤 M4：材质程序 + 材质性质 + 顶点输入 + Pass 状态，形成具体 PSO。
    // 为什么：材质提供状态默认值；Depth/Outline 明确覆盖状态，Color/Transparent 沿用材质，附件格式由 Pass 补充。
    // UE5 对照：Renderer/Private/DepthRendering.cpp 的 FDepthPassMeshProcessor::Process；
    // Renderer/Public/MeshPassProcessor.inl 的 BuildMeshDrawCommands 组装 GraphicsPSOInitializer。
    // 本例简化：启动时按已知标签准备有限的 PSO；实际 Batch 是否参与仍由 Processor 判断，不提供通用 PSO 缓存。
    const auto depthIndex = resource.FindProgram("Depth");
    const auto colorIndex = resource.FindProgram("Color");
    if (!depthIndex || !colorIndex) {
        RADRAY_ERR_LOG("样例材质需要 Depth/Color 程序；Surface 标签由 Processor 筛选");
        return false;
    }
    const auto& depthProgram = *resource.GetProgram(*depthIndex);
    const auto& colorProgram = *resource.GetProgram(*colorIndex);
    if (!depthProgram.Layout || !depthProgram.VertexShader || depthProgram.VertexEntry.empty() || depthProgram.PixelShader ||
        !colorProgram.Layout || !colorProgram.VertexShader || colorProgram.VertexEntry.empty() ||
        !colorProgram.PixelShader || colorProgram.PixelEntry.empty() ||
        depthProgram.Layout != colorProgram.Layout || depthProgram.VertexShader != colorProgram.VertexShader ||
        depthProgram.VertexEntry != colorProgram.VertexEntry) {
        RADRAY_ERR_LOG("样例材质程序无效，Depth/Color 程序必须共享布局和顶点位置计算");
        return false;
    }
    auto& device = Device->GetDevice();
    vector<render::VertexAttribute> attributes;
    for (const auto& attribute : input.Attributes) {
        attributes.push_back({attribute.BufferBinding, attribute.Offset, attribute.Semantic,
                              attribute.SemanticIndex, attribute.Format, attribute.Location});
    }
    auto primitive = render::PrimitiveState::Default();
    const auto& state = resource.Properties.RenderState;
    primitive.Cull = state.Cull;
    primitive.UnclippedDepth = false;
    auto depth = render::DepthStencilState::Default();  // Depth Pass：Less、测试/写入开启、模板关闭。
    depth.Format = render::TextureFormat::D32_FLOAT_S8_UINT;
    depth.DepthBias = state.DepthBias;
    render::GraphicsPipelineStateDescriptor descriptor{
        .PipelineLayout = depthProgram.Layout.Get(), .VS = render::ShaderEntry{depthProgram.VertexShader.Get(), depthProgram.VertexEntry}, .VertexInput = {input.Layouts, attributes}, .Primitive = primitive, .DepthStencil = depth, .MultiSample = render::MultiSampleState::Default()};
    MaterialPipelines pipelines{&resource};
    if (resource.Properties.Tags.Matches(OpaqueTag.Key, OpaqueTag.Value)) {
        pipelines.Depth = device.CreateGraphicsPipelineState(descriptor);
        if (!pipelines.Depth) return false;
    }

    // ColorOnly 没有预深度，Equal 会错误丢弃它；LessEqual 既复用已有深度，也接纳更近的新表面。
    // 不透明材质补写深度；透明材质只测试已有深度，再通过 alpha 混合叠加颜色。所有 PSO 共用同一个 VS。
    depth.DepthCompare = state.DepthCompare;
    depth.DepthTestEnable = state.DepthTestEnable;
    depth.DepthWriteEnable = state.DepthWriteEnable;
    depth.Stencil = state.Stencil;
    auto color = render::ColorTargetState::Default(render::TextureFormat::RGBA8_UNORM);
    color.Blend = state.Blend;
    color.WriteMask = state.ColorWriteMask;
    descriptor.PS = render::ShaderEntry{colorProgram.PixelShader.Get(), colorProgram.PixelEntry};
    descriptor.DepthStencil = depth;
    descriptor.ColorTargets = std::span{&color, 1};
    pipelines.Color = device.CreateGraphicsPipelineState(descriptor);
    if (!pipelines.Color) return false;
    if (resource.Properties.Tags.Matches(OutlineTag.Key, OutlineTag.Value)) {
        // 步骤 M4a：描边 PSO 只保留模板不等于 7 的像素，禁止改写深度和模板。
        // 为什么：Color 已为原物体写入 7；放大的外壳被内部模板挖空，只留下轮廓，深度测试仍遮住被前景挡住的部分。
        // UE5 对照：Renderer/Private/CustomDepthRendering.cpp 的 TryAddMeshBatch 设置模板状态和参考值。
        // 本例简化：共用主深度/模板，以放大几何绘制外壳；不等同于 UE CustomDepth/Stencil + 后处理描边。
        // 放大后的正面比原表面更近，仍能通过深度测试；必须依靠模板拒绝内部，不能只靠深度做出这个效果。
        descriptor.Primitive.Cull = render::CullMode::None;
        depth.DepthTestEnable = true;
        depth.DepthWriteEnable = false;
        depth.DepthCompare = render::CompareFunction::LessEqual;
        depth.DepthBias = {};
        depth.Stencil = render::StencilState::Default();
        depth.Stencil->Front.Compare = depth.Stencil->Back.Compare = render::CompareFunction::NotEqual;
        depth.Stencil->WriteMask = 0;
        descriptor.DepthStencil = depth;
        color.Blend = std::nullopt;
        color.WriteMask = render::ColorWrite::All;
        pipelines.Outline = device.CreateGraphicsPipelineState(descriptor);
        if (!pipelines.Outline) return false;
    }
    RADRAY_INFO_LOG("材质 PSO：Cull={}，Blend={}，DepthWrite={}，模板参考值={}；Depth={} Color={} Outline={}",
                    static_cast<int32_t>(state.Cull), state.Blend.has_value(), state.DepthWriteEnable, state.StencilReference,
                    pipelines.Depth.HasValue(), pipelines.Color.HasValue(), pipelines.Outline.HasValue());
    Pipelines.push_back(std::move(pipelines));
    return true;
}

bool PrepareOutputDirectory(const std::filesystem::path& output) {
    std::error_code error;
    const auto parent = output.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, error);
    if (error) {
        RADRAY_ERR_LOG("无法创建输出目录 {}：{}", parent.string(), error.message());
        return false;
    }
    if (std::filesystem::is_directory(output, error)) {
        RADRAY_ERR_LOG("输出路径是目录而不是 PNG 文件：{}", output.string());
        return false;
    }
    return true;
}

bool GpuFrameResources::SaveImage(const std::filesystem::path& output) const {
    Nullable<void*> mapped = Readback->Map(0, RowPitch * Height);
    if (!mapped) {
        RADRAY_ERR_LOG("无法映射图片读回缓冲");
        return false;
    }
    const auto unmap = MakeScopeGuard([&]() noexcept { Readback->Unmap(); });
    Readback->InvalidateMappedRange({0, RowPitch * Height});
    ImageData image;
    image.Width = Width;
    image.Height = Height;
    image.Format = ImageFormat::RGBA8_BYTE;
    image.Data = make_unique<byte[]>(image.GetSize());
    const auto* source = static_cast<const byte*>(mapped.Get());
    for (uint32_t row = 0; row < Height; ++row) {
        std::memcpy(image.Data.get() + size_t{row} * Width * 4, source + row * RowPitch, size_t{Width} * 4);
    }
    const auto outputName = output.string();
    if (!image.WritePNG({outputName, false})) {
        RADRAY_ERR_LOG("PNG 写入失败：{}", outputName);
        return false;
    }
    RADRAY_INFO_LOG("PNG 已写入：{}（{}×{}，GPU row pitch={}）", outputName, Width, Height, RowPitch);
    return true;
}

}  // namespace radray::examples::mesh_drawing
