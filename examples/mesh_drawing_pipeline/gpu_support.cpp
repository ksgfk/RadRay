#include "gpu_support.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#include <radray/basic_math.h>
#include <radray/image_data.h>
#include <radray/logger.h>
#include <radray/scope_guard.h>

#include "pipeline.h"

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

Nullable<unique_ptr<render::Buffer>> UploadBuffer(
    render::Device& device, std::span<const byte> bytes, render::BufferUse use) {
    auto buffer = device.CreateBuffer({.Size = bytes.size(), .Memory = render::MemoryType::Upload, .Usage = use | render::BufferUse::MapWrite});
    if (!buffer) return nullptr;
    Nullable<void*> mapped = buffer->Map(0, bytes.size());
    if (!mapped) {
        RADRAY_ERR_LOG("无法映射几何上传缓冲");
        return nullptr;
    }
    std::memcpy(mapped.Get(), bytes.data(), bytes.size());
    buffer->FlushMappedRange({0, bytes.size()});
    buffer->Unmap();
    return buffer;
}

}  // namespace

GpuResources::~GpuResources() noexcept {
    // 为什么：GPU 完成后还要结束 allocator epoch，才能释放本次录制引用的 PSO、附件和缓冲。
    // 在成功与失败路径统一执行；主流程保证已开启的 encoder 和 command 在返回前成对结束。
    if (Device) {
        Device->GetQueue().Wait();
        if (Allocator) Allocator->Reset();
    }
}

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
    VertexShader = LoadShader(device, shaderDirectory / fmt::format("cube.VSMain.{}.bin", suffix), render::ShaderStage::Vertex);
    if (!VertexShader) return false;
    PixelShader = LoadShader(device, shaderDirectory / fmt::format("cube.PSMain.{}.bin", suffix), render::ShaderStage::Pixel);
    if (!PixelShader) return false;

    ColorTexture = device.CreateTexture({.Dim = render::TextureDimension::Dim2D, .Width = Width, .Height = Height, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = render::TextureFormat::RGBA8_UNORM, .Memory = render::MemoryType::Device, .Usage = render::TextureUse::RenderTarget | render::TextureUse::CopySource});
    DepthTexture = device.CreateTexture({.Dim = render::TextureDimension::Dim2D, .Width = Width, .Height = Height, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = render::TextureFormat::D32_FLOAT, .Memory = render::MemoryType::Device, .Usage = render::TextureUse::DepthStencilWrite});
    if (!ColorTexture || !DepthTexture) return false;
    ColorView = device.CreateTextureView({ColorTexture.Get(), render::TextureDimension::Dim2D, render::TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, render::TextureViewUsage::RenderTarget});
    DepthWriteView = device.CreateTextureView({DepthTexture.Get(), render::TextureDimension::Dim2D, render::TextureFormat::D32_FLOAT, {0, 1, 0, 1}, render::TextureViewUsage::DepthWrite});
    if (!ColorView || !DepthWriteView) return false;

    RowPitch = Align(uint64_t{Width} * 4, std::max<uint64_t>(1, device.GetCapabilities().Detail.TextureDataPitchAlignment));
    Readback = device.CreateBuffer({.Size = RowPitch * Height, .Memory = render::MemoryType::ReadBack, .Usage = render::BufferUse::CopyDestination | render::BufferUse::MapRead});
    if (!Readback) return false;
    Allocator = device.CreateCommandAllocator(&Device->GetQueue());
    if (!Allocator) return false;
    Command = device.CreateCommandBuffer(Allocator.Get());
    return Command.HasValue();
}

bool GpuResources::UploadGeometry(std::span<const byte> vertices, std::span<const uint32_t> indices) {
    // 步骤 1（GPU 部分）：为所有立方体只准备一份顶点和索引资源。
    // 为什么：多个 Primitive 的区别在物体变换，不需要重复上传相同的几何。
    // UE5 对照：Engine/Public/StaticMeshResources.h 的 FStaticMeshLODResources 持有可复用的几何资源。
    // 本例简化：直接使用 CPU 可写的 Upload 缓冲，省去 staging copy；录制时仍显式处理访问同步。
    VertexBuffer = UploadBuffer(Device->GetDevice(), vertices, render::BufferUse::Vertex);
    IndexBuffer = UploadBuffer(Device->GetDevice(), std::as_bytes(indices), render::BufferUse::Index);
    return VertexBuffer && IndexBuffer;
}

bool GpuResources::CreatePipelines(const VertexInput& input) {
    auto& device = Device->GetDevice();
    const render::PushConstantDescriptor push{
        0, sizeof(DrawConstants), render::ShaderStage::Vertex | render::ShaderStage::Pixel,
        render::PushConstantDescriptor::D3D12Location{0, 0}, render::PushConstantDescriptor::VulkanLocation{0}};
    Layout = device.CreatePipelineLayout({{}, std::span{&push, 1}});
    if (!Layout) return false;
    vector<render::VertexAttribute> attributes;
    for (const auto& attribute : input.Attributes) {
        attributes.push_back({attribute.BufferBinding, attribute.Offset, attribute.Semantic,
                              attribute.SemanticIndex, attribute.Format, attribute.Location});
    }
    auto primitive = render::PrimitiveState::Default();
    primitive.Cull = render::CullMode::None;
    primitive.UnclippedDepth = false;
    auto depth = render::DepthStencilState::Default();  // D32_FLOAT、Less，开启深度测试和写入。
    render::GraphicsPipelineStateDescriptor descriptor{
        .PipelineLayout = Layout.Get(), .VS = render::ShaderEntry{VertexShader.Get(), "VSMain"}, .VertexInput = {input.Layouts, attributes}, .Primitive = primitive, .DepthStencil = depth, .MultiSample = render::MultiSampleState::Default()};
    DepthPipeline = device.CreateGraphicsPipelineState(descriptor);
    if (!DepthPipeline) return false;

    // ColorOnly 没有预深度，Equal 会错误丢弃它；LessEqual 既复用已有深度，也接纳更近的新表面。
    // 同时写深度，使这些对象自身及彼此正确遮挡。附件 Load，两个 PSO 仍共享同一个 VS。
    depth.DepthCompare = render::CompareFunction::LessEqual;
    depth.DepthWriteEnable = true;
    const auto color = render::ColorTargetState::Default(render::TextureFormat::RGBA8_UNORM);
    descriptor.PS = render::ShaderEntry{PixelShader.Get(), "PSMain"};
    descriptor.DepthStencil = depth;
    descriptor.ColorTargets = std::span{&color, 1};
    ColorPipeline = device.CreateGraphicsPipelineState(descriptor);
    return ColorPipeline.HasValue();
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

bool GpuResources::SaveImage(const std::filesystem::path& output) const {
    // 步骤 8（读回部分）：从 GPU copy 的行跨度中提取紧密 RGBA8 像素，再写 PNG。
    // 为什么：Readback 每行可能含 padding；直接把整块内存当图片会造成错行。
    // UE5 对照：这一段属于离屏结果验证，位于 Mesh Drawing Pipeline 之后，不对应 FMeshDrawCommand。
    // 本例简化：主流程已经 Queue.Wait，只读回一张 800×600 图片，不实现异步读回队列。
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

void TransitionTexture(render::CommandBuffer& command, render::Texture& texture,
                       render::TextureStates before, render::TextureStates after) {
    const render::ResourceBarrierDescriptor barrier = render::BarrierTextureDescriptor{&texture, before, after, {0, 1, 0, 1}};
    command.ResourceBarrier(std::span{&barrier, 1});
}

}  // namespace radray::examples::mesh_drawing
