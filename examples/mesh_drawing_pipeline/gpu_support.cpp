#include "gpu_support.h"

#include <algorithm>
#include <cstring>
#include <cstddef>

#include <radray/basic_math.h>
#include <radray/file.h>
#include <radray/image_data.h>
#include <radray/logger.h>
#include <radray/scope_guard.h>

namespace radray::examples::mesh_drawing {
shader::DiagnosticResult<ShaderAsset> GpuResources::CompileCube(const shader::Client& compiler, const Uri& uri, bool swapColors) {
    const std::filesystem::path root{RADRAY_SAMPLE_ROOT};
    const auto source = ReadBinaryFile(root / "examples/mesh_drawing_pipeline/cube.hlsl");
    if (!source) return {std::nullopt, "cannot read cube HLSL"};
    const array includes{root / "shaderlib"};
    ShaderCompileDesc desc{.SourceUri = uri, .Source = *source, .IncludePaths = includes};
    if (swapColors) desc.Defines.push_back({"SWAP_COLORS", "1"});
    desc.ReflectionPolicy.Bindings.push_back({.Name = "Pass", .GroupSlot = 0, .BindingId = 0});
    desc.ReflectionPolicy.PushConstants.push_back({"MaterialPush", 0});
    return ShaderAsset::Compile(Device->GetDevice(), compiler, desc);
}

bool GpuResources::Initialize(gpu::GpuBackend backend, const std::filesystem::path& shaderDirectory,
                              Nullable<const shader::Client*> compiler) {
    RADRAY_INFO_LOG("准备 GPU：backend={}，请求开启后端验证", backend);
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
    LayoutRegistry = make_unique<PipelineLayoutRegistry>(device);
    Assets = make_unique<AssetManager>();
    if (compiler) {
        const auto uri = Uri::TryCreate("virtual://shaders/cube.hlsl");
        if (!uri) return false;
        auto compiled = CompileCube(*compiler, *uri, false);
        if (!compiled.Succeeded()) {
            RADRAY_ERR_LOG("{}", compiled.Diagnostics);
            return false;
        }
        Shader = Assets->Emplace(std::move(*compiled.Value), {}, MakeAssetId(*uri));
        if (!Shader) return false;
    } else {
        auto loaded = ShaderAsset::Load(device, shaderDirectory / "cube.index.json");
        if (!loaded) return false;
        Shader = Assets->Emplace(std::move(*loaded), {}, MakeAssetId("cube.index.json"));
    }
    if (!Shader) return false;
    MaterialAssetDesc opaqueDesc;
    opaqueDesc.Programs = {{"Depth", Shader, 0, "VSMain", ""}, {"Color", Shader, 0, "VSMain", "PSMain"}};
    opaqueDesc.Parameters = {{"BaseColor", array<float, 4>{0.72f, 0.25f, 0.09f, 1}}, {"Brightness", 1.0f}};
    // The pass combines these CPU parameters into its Color push constant.
    opaqueDesc.Families = {{Shader, {}, {}}};
    const auto surface = MaterialTags.Intern("Surface");
    const auto opaque = MaterialTags.Intern("Opaque");
    const auto transparent = MaterialTags.Intern("Transparent");
    const auto outline = MaterialTags.Intern("Outline");
    const auto enabled = MaterialTags.Intern("Enabled");
    if (!surface || !opaque || !transparent || !outline || !enabled) return false;
    OpaqueTag = {*surface, *opaque};
    TransparentTag = {*surface, *transparent};
    OutlineTag = {*outline, *enabled};
    opaqueDesc.Properties.Tags = MaterialTagSet{MaterialTags};
    if (!opaqueDesc.Properties.Tags.Set(*surface, *opaque)) return false;
    auto twoSidedDesc = opaqueDesc;
    if (!twoSidedDesc.Properties.Tags.Set(*outline, *enabled)) return false;
    auto& twoSided = twoSidedDesc.Properties.RenderState;
    twoSided.Cull = gpu::CullMode::None;
    twoSided.Stencil = gpu::StencilState::Default();
    twoSided.Stencil->Front.PassOp = twoSided.Stencil->Back.PassOp = gpu::StencilOperation::Replace;
    twoSided.StencilReference = 7;
    auto transparentDesc = opaqueDesc;
    if (!transparentDesc.Properties.Tags.Set(*surface, *transparent)) return false;
    auto& translucent = transparentDesc.Properties.RenderState;
    translucent.DepthWriteEnable = false;
    translucent.Blend = gpu::BlendState{
        {gpu::BlendFactor::SrcAlpha, gpu::BlendFactor::OneMinusSrcAlpha, gpu::BlendOperation::Add},
        {gpu::BlendFactor::One, gpu::BlendFactor::OneMinusSrcAlpha, gpu::BlendOperation::Add}};
    OpaqueMaterial = CreateMaterial(*Assets, opaqueDesc);
    TwoSidedMaterial = CreateMaterial(*Assets, twoSidedDesc);
    TransparentMaterial = CreateMaterial(*Assets, transparentDesc);
    if (!OpaqueMaterial || !TwoSidedMaterial || !TransparentMaterial) return false;
    RADRAY_INFO_LOG("材质标签登记：Surface=Opaque / Transparent，Outline=Enabled；描边物体写模板 7，透明材质启用 alpha 混合并关闭深度写入");

    return true;
}

bool GpuResources::PrepareGeometry(std::span<const byte> vertices, std::span<const uint32_t> indices) {
    // Scene 可以引用尚未写入的 Device 缓冲；字节由首帧 RDG 的 Staging Copy 上传。
    VertexBuffer = Device->GetDevice().CreateBuffer({.Size = vertices.size(), .Memory = gpu::MemoryType::Device, .Usage = gpu::BufferUse::Vertex | gpu::BufferUse::CopyDestination});
    IndexBuffer = Device->GetDevice().CreateBuffer({.Size = indices.size_bytes(), .Memory = gpu::MemoryType::Device, .Usage = gpu::BufferUse::Index | gpu::BufferUse::CopyDestination});
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
    Readback = device.CreateBuffer({.Size = RowPitch * Height, .Memory = gpu::MemoryType::ReadBack, .Usage = gpu::BufferUse::CopyDestination | gpu::BufferUse::MapRead});
    if (!Readback) return false;
    PassGroupLayout = shared.PassGroupLayout;
    for (auto& parameters : PassParameterSets) {
        parameters = device.CreateShaderParameterSet({PassGroupLayout.Get()});
        if (!parameters) return false;
    }
    CompletionFence = device.CreateFence();
    return CompletionFence.HasValue();
}

namespace {
bool MatchesCubeConstants(const ShaderAsset& shader) {
    using shader::ShaderTypeKind;
    const auto pass = shader.FindBinding("Pass");
    const auto push = shader.FindPushConstant("MaterialPush");
    if (!pass || !pass->ActiveOnDevice || pass->Reference.GroupSlot != 0 || pass->Reference.BindingId != 0 ||
        pass->Kind != gpu::BindingKind::CBuffer || pass->Count != 1 || pass->BufferByteSize != sizeof(PassConstants) ||
        !push || !push->ActiveOnDevice || push->PushId != 0 || push->Size != sizeof(MaterialConstants)) return false;
    const auto member = [&](std::string_view block, std::string_view name, ShaderTypeKind kind, uint32_t rows,
                            uint32_t columns, size_t offset, size_t size) {
        auto field = shader.FindBufferMember(block, name);
        return field && field->Kind == kind && field->ElementCount == 1 && field->ScalarKind == shader::ShaderScalarKind::Float &&
               field->RowCount == rows && field->ColumnCount == columns && field->AbsoluteOffset == offset && field->Size == size &&
               (kind != ShaderTypeKind::Matrix || (!field->RowMajor && field->MatrixStride == 16));
    };
    return member("Pass", "ViewProjection", ShaderTypeKind::Matrix, 4, 4, offsetof(PassConstants, ViewProjection), sizeof(PassConstants::ViewProjection)) &&
           member("Pass", "LocalScale", ShaderTypeKind::Scalar, 1, 1, offsetof(PassConstants, LocalScale), sizeof(float)) &&
           member("MaterialPush", "Color", ShaderTypeKind::Vector, 1, 4, offsetof(MaterialConstants, Color), sizeof(MaterialConstants));
}
}  // namespace

std::optional<MaterialPipelines> GpuResources::PreparePipelines(const VertexInput& input, AssetHandle<MaterialAsset> material) {
    // 步骤 M4：材质程序 + 材质性质 + 顶点输入 + Pass 状态，形成具体 PSO。
    // 为什么：材质提供状态默认值；Depth/Outline 明确覆盖状态，Color/Transparent 沿用材质，附件格式由 Pass 补充。
    // UE5 对照：Renderer/Private/DepthRendering.cpp 的 FDepthPassMeshProcessor::Process；
    // Renderer/Public/MeshPassProcessor.inl 的 BuildMeshDrawCommands 组装 GraphicsPSOInitializer。
    // 本例简化：启动时按已知标签准备有限的 PSO；实际 Batch 是否参与仍由 Processor 判断，不提供通用 PSO 缓存。
    const auto resource = Assets->Get(material);
    const auto shader = Shader.Get();
    if (!resource || !shader || !MatchesCubeConstants(*shader)) return std::nullopt;
    const auto table = resource->GetProperties().Tags.GetTable();
    if (table && table.Get() != &MaterialTags) return std::nullopt;
    const auto depthIndex = resource->FindProgram("Depth");
    const auto colorIndex = resource->FindProgram("Color");
    if (!depthIndex || !colorIndex) {
        RADRAY_ERR_LOG("样例材质需要 Depth/Color 程序；Surface 标签由 Processor 筛选");
        return std::nullopt;
    }
    const auto& depthProgram = *resource->GetProgram(*depthIndex);
    const auto& colorProgram = *resource->GetProgram(*colorIndex);
    if (depthProgram.Shader != Shader.GetHandle() || colorProgram.Shader != Shader.GetHandle()) return std::nullopt;
    const auto vs = shader->GetStage(depthProgram.VariantIndex, gpu::ShaderStage::Vertex);
    const auto colorVs = shader->GetStage(colorProgram.VariantIndex, gpu::ShaderStage::Vertex);
    const auto ps = shader->GetStage(colorProgram.VariantIndex, gpu::ShaderStage::Pixel);
    if (!vs || !colorVs || !ps || vs->Target != colorVs->Target || !depthProgram.PixelEntry.empty()) return std::nullopt;
    auto layout = LayoutRegistry->Acquire(shader->GetLayoutDesc());
    if (!layout) return std::nullopt;
    auto passGroup = layout->GetGroupLayout(0);
    if (!passGroup || (PassGroupLayout && PassGroupLayout.Get() != passGroup.Get())) return std::nullopt;
    auto& device = Device->GetDevice();
    ResolvedVertexInputState depthInput, colorInput;
    const auto prepareInput = [&](uint32_t variant, ResolvedVertexInputState& resolved) {
        const auto interface = shader->GetVariantInterface(variant);
        if (!interface) return false;
        for (const auto binding : interface->ActiveBindings)
            if (binding != shader::ShaderBindingReference{0, 0}) return false;
        for (const auto id : interface->ActivePushConstants)
            if (id != 0) return false;
        return PrepareMaterialVertexInput(*interface, input, resolved);
    };
    if (!prepareInput(depthProgram.VariantIndex, depthInput) ||
        !prepareInput(colorProgram.VariantIndex, colorInput)) return std::nullopt;
    auto primitive = gpu::PrimitiveState::Default();
    const auto& state = resource->GetProperties().RenderState;
    primitive.Cull = state.Cull;
    primitive.UnclippedDepth = false;
    auto depth = gpu::DepthStencilState::Default();  // Depth Pass：Less、测试/写入开启、模板关闭。
    depth.Format = gpu::TextureFormat::D32_FLOAT_S8_UINT;
    depth.DepthBias = state.DepthBias;
    gpu::GraphicsPipelineStateDescriptor descriptor{
        .PipelineLayout = &layout->GetLayout(), .VS = *vs, .VertexInput = {depthInput.Layouts, depthInput.Attributes}, .Primitive = primitive, .DepthStencil = depth, .MultiSample = gpu::MultiSampleState::Default()};
    MaterialPipelines pipelines{material, layout.Release()};
    if (resource->GetProperties().Tags.Matches(OpaqueTag.Key, OpaqueTag.Value)) {
        pipelines.Depth = device.CreateGraphicsPipelineState(descriptor);
        if (!pipelines.Depth) return std::nullopt;
    }

    // ColorOnly 没有预深度，Equal 会错误丢弃它；LessEqual 既复用已有深度，也接纳更近的新表面。
    // 不透明材质补写深度；透明材质只测试已有深度，再通过 alpha 混合叠加颜色。所有 PSO 共用同一个 VS。
    depth.DepthCompare = state.DepthCompare;
    depth.DepthTestEnable = state.DepthTestEnable;
    depth.DepthWriteEnable = state.DepthWriteEnable;
    depth.Stencil = state.Stencil;
    auto color = gpu::ColorTargetState::Default(gpu::TextureFormat::RGBA8_UNORM);
    color.Blend = state.Blend;
    color.WriteMask = state.ColorWriteMask;
    descriptor.VertexInput = {colorInput.Layouts, colorInput.Attributes};
    descriptor.PS = *ps;
    descriptor.DepthStencil = depth;
    descriptor.ColorTargets = std::span{&color, 1};
    pipelines.Color = device.CreateGraphicsPipelineState(descriptor);
    if (!pipelines.Color) return std::nullopt;
    if (resource->GetProperties().Tags.Matches(OutlineTag.Key, OutlineTag.Value)) {
        // 步骤 M4a：描边 PSO 只保留模板不等于 7 的像素，禁止改写深度和模板。
        // 为什么：Color 已为原物体写入 7；放大的外壳被内部模板挖空，只留下轮廓，深度测试仍遮住被前景挡住的部分。
        // UE5 对照：Renderer/Private/CustomDepthRendering.cpp 的 TryAddMeshBatch 设置模板状态和参考值。
        // 本例简化：共用主深度/模板，以放大几何绘制外壳；不等同于 UE CustomDepth/Stencil + 后处理描边。
        // 放大后的正面比原表面更近，仍能通过深度测试；必须依靠模板拒绝内部，不能只靠深度做出这个效果。
        descriptor.Primitive.Cull = gpu::CullMode::None;
        depth.DepthTestEnable = true;
        depth.DepthWriteEnable = false;
        depth.DepthCompare = gpu::CompareFunction::LessEqual;
        depth.DepthBias = {};
        depth.Stencil = gpu::StencilState::Default();
        depth.Stencil->Front.Compare = depth.Stencil->Back.Compare = gpu::CompareFunction::NotEqual;
        depth.Stencil->WriteMask = 0;
        descriptor.DepthStencil = depth;
        color.Blend = std::nullopt;
        color.WriteMask = gpu::ColorWrite::All;
        pipelines.Outline = device.CreateGraphicsPipelineState(descriptor);
        if (!pipelines.Outline) return std::nullopt;
    }
    RADRAY_INFO_LOG("材质 PSO：Cull={}，Blend={}，DepthWrite={}，模板参考值={}；Depth={} Color={} Outline={}",
                    static_cast<int32_t>(state.Cull), state.Blend.has_value(), state.DepthWriteEnable, state.StencilReference,
                    pipelines.Depth.HasValue(), pipelines.Color.HasValue(), pipelines.Outline.HasValue());
    pipelines.AppliedShaderRevision = pipelines.CompatibleShaderRevision = Assets->GetRevision(Shader.Untyped());
    pipelines.AppliedMaterialRevision = pipelines.CompatibleMaterialRevision = Assets->GetRevision(material.Untyped());
    return pipelines;
}

bool GpuResources::CreatePipelines(const VertexInput& input, AssetHandle<MaterialAsset> material) {
    auto prepared = PreparePipelines(input, material);
    if (!prepared) return false;
    PassGroupLayout = prepared->Layout->GetGroupLayout(0);
    Pipelines.push_back(std::move(*prepared));
    return true;
}

void GpuResources::SyncPipelines(const VertexInput& input, std::span<const AssetReloadEvent> events,
                                 Scene& scene, std::span<const MaterialProxyHandle> proxies, FrameSlot& slot) {
    const auto shaderRevision = Assets->GetRevision(Shader.Untyped());
    for (auto& current : Pipelines) {
        const auto materialRevision = Assets->GetRevision(current.Material.Untyped());
        if (current.AppliedShaderRevision == shaderRevision && current.AppliedMaterialRevision == materialRevision) continue;
        for (const auto& event : events) {
            if (event.Kind != AssetReloadKind::Compatible) continue;
            if (event.Handle == Shader.Untyped() && event.Revision - 1 == current.CompatibleShaderRevision)
                current.CompatibleShaderRevision = event.Revision;
            if (event.Handle == current.Material.Untyped() && event.Revision - 1 == current.CompatibleMaterialRevision)
                current.CompatibleMaterialRevision = event.Revision;
        }
        auto next = PreparePipelines(input, current.Material);
        if (!next && (!current.Color || (current.CompatibleShaderRevision == shaderRevision &&
                                         current.CompatibleMaterialRevision == materialRevision))) continue;
        // Processor 长期借用 Pipelines 的 span；只原位替换元素。空 Color 表示暂停。
        if (current.Color) slot.Emplace<MaterialPipelines>(std::move(current));
        if (next) current = std::move(*next);
        for (const auto proxy : proxies) {
            const auto snapshot = scene.GetMaterial(proxy);
            if (snapshot && snapshot->GetMaterial() == current.Material) scene.MarkMaterialDirty(proxy);
        }
    }
}

bool SyncMaterialProxies(const AssetManager& assets, Scene& scene, std::span<const MaterialInstance> instances,
                         std::span<const MaterialProxyHandle> proxies, std::span<uint32_t> appliedRevisions) {
    if (instances.size() != proxies.size() || proxies.size() != appliedRevisions.size()) return false;
    for (size_t i = 0; i < instances.size(); ++i) {
        const auto revision = assets.GetRevision(instances[i].GetMaterial().Untyped());
        if (revision && revision == appliedRevisions[i]) continue;
        MaterialRenderProxy proxy;
        if (!proxy.Update(assets, instances[i]) || !scene.UpdateMaterial(proxies[i], std::move(proxy))) return false;
        appliedRevisions[i] = revision;
    }
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

std::optional<ImageData> GpuFrameResources::ReadImage() const {
    Nullable<void*> mapped = Readback->GetMappedData();
    if (!mapped) {
        RADRAY_ERR_LOG("无法映射图片读回缓冲");
        return std::nullopt;
    }
    if (!Readback->InvalidateMappedRange({0, RowPitch * Height})) return std::nullopt;
    ImageData image;
    image.Width = Width;
    image.Height = Height;
    image.Format = ImageFormat::RGBA8_BYTE;
    image.Data = make_unique<byte[]>(image.GetSize());
    const auto* source = static_cast<const byte*>(mapped.Get());
    for (uint32_t row = 0; row < Height; ++row) {
        std::memcpy(image.Data.get() + size_t{row} * Width * 4, source + row * RowPitch, size_t{Width} * 4);
    }
    return image;
}

}  // namespace radray::examples::mesh_drawing
