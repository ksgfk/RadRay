#include "render_test_framework.h"

#include <gtest/gtest.h>
#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstring>

#include <radray/basic_math.h>
#include <radray/runtime/gpu_resource.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/pipeline_state_cache.h>
#include <radray/runtime/render_pass_registry.h>
#include <radray/runtime/sampler_cache.h>
#include <radray/runtime/shader_variant_library.h>
#include <radray/render/shader_compiler/dxc.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/material_asset.h>
#include <radray/runtime/shader_asset.h>
#include <radray/runtime/render_framework/mesh_pass_executor.h>
#include <radray/runtime/render_framework/primitive_scene_proxy.h>
#include <radray/runtime/render_framework/render_queue.h>

namespace radray::render::test {
namespace {

// 全屏三角 mesh pass。
// - PerObject cbuffer (b1, space1): 逐物体常量, 承载 ObjectToWorld。执行器每 draw 填充并绑定。
//   PS 取矩阵首列 rgb 作为输出色, 从而验证 per-object cbuffer 确实通到了 shader。
// - MaterialParams push_constant (b0, space0): material property (_Tint), 与 per-object 互不冲突。
constexpr std::string_view kMeshPassSource = R"(
struct MaterialParams {
    float4 Tint;
};
[[vk::binding(0, 2)]] ConstantBuffer<MaterialParams> gMaterial : register(b0, space2);

struct PerObject {
    float4x4 ObjectToWorld;
};
[[vk::binding(1, 0)]] ConstantBuffer<PerObject> gPerObject : register(b1, space0);

struct VSIn {
    float2 Pos : POSITION0;
};

struct VSOut {
    float4 Pos : SV_POSITION;
};

VSOut VSMain(VSIn i) {
    VSOut o;
    o.Pos = float4(i.Pos, 0.0, 1.0);
    return o;
}

float4 PSMain(VSOut i) : SV_TARGET {
    // ObjectToWorld 的首列前 3 个分量作为颜色 (由 CPU 端写入的 (r,g,b) 决定)。
    float3 c = float3(gPerObject.ObjectToWorld[0][0], gPerObject.ObjectToWorld[1][0], gPerObject.ObjectToWorld[2][0]);
    c *= gMaterial.Tint.rgb;
    return float4(c, 1.0);
}
)";

constexpr std::string_view kGenericBindingSource = R"(
struct MixedData {
    float4x4 ObjectToWorld;
    float4 Tint;
};

[[vk::binding(2, 5)]] ConstantBuffer<MixedData> CustomData : register(b2, space5);
[[vk::binding(4, 5)]] Texture2D DefaultTexture : register(t4, space5);
[[vk::binding(6, 5)]] SamplerState DefaultSampler : register(s6, space5);

struct VSIn {
    float2 Pos : POSITION0;
};

struct VSOut {
    float4 Pos : SV_POSITION;
};

VSOut VSMain(VSIn input) {
    VSOut output;
    output.Pos = mul(CustomData.ObjectToWorld, float4(input.Pos, 0.0, 1.0));
    return output;
}

float4 PSMain(VSOut input) : SV_TARGET {
    (void)input;
    return DefaultTexture.Sample(DefaultSampler, float2(0.0, 0.0)) * CustomData.Tint;
}
)";

constexpr std::string_view kMissingTextureSource = R"(
[[vk::binding(0, 3)]] Texture2D MissingTexture : register(t0, space3);

struct VSIn { float2 Pos : POSITION0; };
struct VSOut { float4 Pos : SV_POSITION; };

VSOut VSMain(VSIn input) {
    VSOut output;
    output.Pos = float4(input.Pos, 0.0, 1.0);
    return output;
}

float4 PSMain(VSOut input) : SV_TARGET {
    (void)input;
    return MissingTexture.Load(int3(0, 0, 0));
}
)";

constexpr std::string_view kUnsupportedBufferSource = R"(
[[vk::binding(0, 3)]] StructuredBuffer<float4> UnsupportedBuffer : register(t0, space3);

struct VSIn { float2 Pos : POSITION0; };
struct VSOut { float4 Pos : SV_POSITION; };

VSOut VSMain(VSIn input) {
    VSOut output;
    output.Pos = float4(input.Pos, 0.0, 1.0);
    return output;
}

float4 PSMain(VSOut input) : SV_TARGET {
    (void)input;
    return UnsupportedBuffer[0];
}
)";

constexpr std::string_view kUnsupportedUavSource = R"(
[[vk::binding(0, 3)]] RWStructuredBuffer<float4> UnsupportedUav : register(u0, space3);

struct VSIn { float2 Pos : POSITION0; };
struct VSOut { float4 Pos : SV_POSITION; };

VSOut VSMain(VSIn input) {
    VSOut output;
    output.Pos = float4(input.Pos, 0.0, 1.0);
    return output;
}

float4 PSMain(VSOut input) : SV_TARGET {
    (void)input;
    UnsupportedUav[0] = float4(1.0, 0.0, 0.0, 1.0);
    return UnsupportedUav[0];
}
)";

constexpr std::string_view kUnsupportedArraySource = R"(
[[vk::binding(0, 3)]] Texture2D UnsupportedTextures[2] : register(t0, space3);

struct VSIn { float2 Pos : POSITION0; };
struct VSOut { float4 Pos : SV_POSITION; };

VSOut VSMain(VSIn input) {
    VSOut output;
    output.Pos = float4(input.Pos, 0.0, 1.0);
    return output;
}

float4 PSMain(VSOut input) : SV_TARGET {
    (void)input;
    return UnsupportedTextures[0].Load(int3(0, 0, 0));
}
)";

constexpr std::string_view kUnsupportedBindlessSource = R"(
[[vk::binding(0, 3)]] StructuredBuffer<float4> UnsupportedBindless[] : register(t0, space3);

struct VSIn { float2 Pos : POSITION0; };
struct VSOut { float4 Pos : SV_POSITION; };

VSOut VSMain(VSIn input) {
    VSOut output;
    output.Pos = float4(input.Pos, 0.0, 1.0);
    return output;
}

float4 PSMain(VSOut input) : SV_TARGET {
    (void)input;
    return UnsupportedBindless[0][0];
}
)";

constexpr std::string_view kUnsupportedPushConstantSource = R"(
struct PushData { float4 Value; };
[[vk::push_constant]] ConstantBuffer<PushData> UnsupportedPush : register(b0, space0);

struct VSIn { float2 Pos : POSITION0; };
struct VSOut { float4 Pos : SV_POSITION; };

VSOut VSMain(VSIn input) {
    VSOut output;
    output.Pos = float4(input.Pos, 0.0, 1.0);
    return output;
}

float4 PSMain(VSOut input) : SV_TARGET {
    (void)input;
    return UnsupportedPush.Value;
}
)";

constexpr std::string_view kMissingViewProviderSource = R"(
struct ViewData { float4 Tint; };
[[vk::binding(0, 3)]] ConstantBuffer<ViewData> MissingView : register(b0, space3);

struct VSIn { float2 Pos : POSITION0; };
struct VSOut { float4 Pos : SV_POSITION; };

VSOut VSMain(VSIn input) {
    VSOut output;
    output.Pos = float4(input.Pos, 0.0, 1.0);
    return output;
}

float4 PSMain(VSOut input) : SV_TARGET {
    (void)input;
    return MissingView.Tint;
}
)";

constexpr std::string_view kErrorMaterialSource = R"(
struct ObjectData { float4x4 ObjectToWorld; };
[[vk::binding(0, 0)]] ConstantBuffer<ObjectData> ErrorObject : register(b0, space0);

struct VSIn { float2 Pos : POSITION0; };
struct VSOut { float4 Pos : SV_POSITION; };

VSOut VSMain(VSIn input) {
    VSOut output;
    output.Pos = mul(ErrorObject.ObjectToWorld, float4(input.Pos, 0.0, 1.0));
    return output;
}

float4 PSMain(VSOut input) : SV_TARGET {
    (void)input;
    return float4(1.0, 0.0, 1.0, 1.0);
}
)";

constexpr uint32_t kRtSize = 4;
constexpr TextureFormat kRtFormat = TextureFormat::RGBA8_UNORM;

// 一个自带真实 VB/IB 的测试 proxy。三角覆盖整个 [-1,1] NDC (全屏三角)。
class TriangleProxy : public PrimitiveSceneProxy {
public:
    bool Init(ComputeTestContext& ctx, const Eigen::Matrix4f& localToWorld, string* reason) {
        _localToWorld = localToWorld;

        // 全屏三角三个顶点 (float2)。
        const std::array<float, 6> verts = {
            -1.0f, -3.0f,
            -1.0f, 1.0f,
            3.0f, 1.0f};
        const std::array<uint32_t, 3> indices = {0, 1, 2};

        BufferDescriptor vbDesc{};
        vbDesc.Size = sizeof(verts);
        vbDesc.Memory = MemoryType::Device;
        vbDesc.Usage = BufferUse::Vertex | BufferUse::CopyDestination;
        auto vbOpt = ctx.CreateBuffer(vbDesc, reason);
        if (!vbOpt.HasValue()) {
            return false;
        }
        _vb = vbOpt.Release();
        if (!ctx.UploadBufferData(
                _vb.get(),
                std::as_bytes(std::span<const float>{verts.data(), verts.size()}),
                BufferState::Vertex,
                reason)) {
            return false;
        }

        BufferDescriptor ibDesc{};
        ibDesc.Size = sizeof(indices);
        ibDesc.Memory = MemoryType::Device;
        ibDesc.Usage = BufferUse::Index | BufferUse::CopyDestination;
        auto ibOpt = ctx.CreateBuffer(ibDesc, reason);
        if (!ibOpt.HasValue()) {
            return false;
        }
        _ib = ibOpt.Release();
        if (!ctx.UploadBufferData(
                _ib.get(),
                std::as_bytes(std::span<const uint32_t>{indices.data(), indices.size()}),
                BufferState::Index,
                reason)) {
            return false;
        }

        _draw.Vbv = VertexBufferView{.Target = _vb.get(), .Offset = 0, .Size = sizeof(verts)};
        _draw.Ibv = IndexBufferView{.Target = _ib.get(), .Offset = 0, .Stride = sizeof(uint32_t)};
        return true;
    }

    Eigen::Matrix4f GetLocalToWorld() const noexcept override { return _localToWorld; }

    MeshDrawArgs GetDrawArgs(uint32_t sectionIndex) const noexcept override {
        if (sectionIndex != 0) {
            return MeshDrawArgs{};
        }
        MeshDrawArgs args{};
        args.Geometry = &_draw;
        args.FirstIndex = 0;
        args.IndexCount = 3;
        args.VertexOffset = 0;
        return args;
    }

private:
    Eigen::Matrix4f _localToWorld{Eigen::Matrix4f::Identity()};
    unique_ptr<Buffer> _vb;
    unique_ptr<Buffer> _ib;
    GpuMesh::DrawData _draw{};
};

ShaderPassDesc MakeMeshPass() {
    ShaderPassDesc pass{};
    pass.PassTag = "ForwardLit";
    pass.Source = string{kMeshPassSource};
    pass.VertexEntry = "VSMain";
    pass.PixelEntry = "PSMain";
    // 全屏三角, 关背面剔除避免绕序问题。
    pass.Primitive = PrimitiveState::Default();
    pass.Primitive.Cull = CullMode::None;
    pass.MultiSample = MultiSampleState::Default();
    pass.ColorTargets.push_back(ColorTargetState::Default(kRtFormat));
    pass.DynamicBufferBindings.push_back(DynamicBufferBinding{.Group = 0, .Binding = 1});
    pass.ParameterSources = {
        ShaderParameterSourceDesc{
            .Name = "gPerObject",
            .Scope = ShaderParameterScope::Object,
            .ProviderName = "gPerObject"},
        ShaderParameterSourceDesc{
            .Name = "gMaterial",
            .Scope = ShaderParameterScope::Material,
            .ProviderName = "gMaterial"}};
    // 顶点布局: 一个 float2 POSITION。
    OwningVertexBufferLayout layout{};
    layout.ArrayStride = sizeof(float) * 2;
    layout.StepMode = VertexStepMode::Vertex;
    layout.Elements.push_back(VertexElement{
        .Offset = 0,
        .Semantic = "POSITION",
        .SemanticIndex = 0,
        .Format = VertexFormat::FLOAT32X2,
        .Location = 0});
    pass.VertexLayouts.push_back(std::move(layout));
    return pass;
}

ShaderPassDesc MakeGenericBindingPass() {
    ShaderPassDesc pass{};
    pass.PassTag = "ForwardLit";
    pass.Source = string{kGenericBindingSource};
    pass.VertexEntry = "VSMain";
    pass.PixelEntry = "PSMain";
    pass.Primitive = PrimitiveState::Default();
    pass.Primitive.Cull = CullMode::None;
    pass.MultiSample = MultiSampleState::Default();
    pass.ColorTargets.push_back(ColorTargetState::Default(kRtFormat));
    pass.DynamicBufferBindings.push_back(DynamicBufferBinding{.Group = 5, .Binding = 2});
    pass.ParameterSources = {
        ShaderParameterSourceDesc{
            .Name = "CustomData.ObjectToWorld",
            .Scope = ShaderParameterScope::Object,
            .ProviderName = "ObjectToWorld"},
        ShaderParameterSourceDesc{
            .Name = "CustomData.Tint",
            .Scope = ShaderParameterScope::Material,
            .ProviderName = "Tint"},
        ShaderParameterSourceDesc{
            .Name = "DefaultTexture",
            .Scope = ShaderParameterScope::Material},
        ShaderParameterSourceDesc{
            .Name = "DefaultSampler",
            .Scope = ShaderParameterScope::Material}};
    OwningVertexBufferLayout layout{};
    layout.ArrayStride = sizeof(float) * 2;
    layout.StepMode = VertexStepMode::Vertex;
    layout.Elements.push_back(VertexElement{
        .Offset = 0,
        .Semantic = "POSITION",
        .SemanticIndex = 0,
        .Format = VertexFormat::FLOAT32X2,
        .Location = 0});
    pass.VertexLayouts.push_back(std::move(layout));
    return pass;
}

ShaderPassDesc MakeSimpleUnlitPass(
    std::string_view source,
    bool withObjectBinding = false) {
    ShaderPassDesc pass{};
    pass.PassTag = "ForwardLit";
    pass.Source = string{source};
    pass.VertexEntry = "VSMain";
    pass.PixelEntry = "PSMain";
    pass.Primitive = PrimitiveState::Default();
    pass.Primitive.Cull = CullMode::None;
    pass.MultiSample = MultiSampleState::Default();
    pass.ColorTargets.push_back(ColorTargetState::Default(kRtFormat));
    if (withObjectBinding) {
        pass.DynamicBufferBindings.push_back(DynamicBufferBinding{.Group = 0, .Binding = 0});
        pass.ParameterSources.push_back(ShaderParameterSourceDesc{
            .Name = "ErrorObject",
            .Scope = ShaderParameterScope::Object,
            .ProviderName = "ObjectToWorld"});
    }
    OwningVertexBufferLayout layout{};
    layout.ArrayStride = sizeof(float) * 2;
    layout.StepMode = VertexStepMode::Vertex;
    layout.Elements.push_back(VertexElement{
        .Offset = 0,
        .Semantic = "POSITION",
        .SemanticIndex = 0,
        .Format = VertexFormat::FLOAT32X2,
        .Location = 0});
    pass.VertexLayouts.push_back(std::move(layout));
    return pass;
}

class MeshPassExecutorTest : public ::testing::TestWithParam<TestBackend> {
protected:
    void SetUp() override {
        string reason;
        if (!_ctx.Initialize(this->GetParam(), &reason)) {
            GTEST_SKIP() << fmt::format("Init failed on {}: {}", format_as(this->GetParam()), reason);
        }
        _layoutLibrary = make_unique<PipelineLayoutLibrary>(_ctx.GetDevicePtr());
        _variantCache = CreateShaderVariantLibrary(
                            _ctx.GetDevicePtr(), _ctx.GetDxc(), _layoutLibrary.get())
                            .Release();
        _psoCache = make_unique<GraphicsPipelineStateLibrary>(_ctx.GetDevicePtr());
        _samplerCache = make_unique<SamplerCache>(_ctx.GetDevicePtr());
    }

    bool SubmitAndWait(CommandBuffer* command, string* reason) {
        _hostWrites.Flush(*_ctx.GetDevicePtr());
        return _ctx.SubmitAndWait(command, reason);
    }

    ComputeTestContext _ctx{};
    HostWriteBatch _hostWrites{};
    unique_ptr<PipelineLayoutLibrary> _layoutLibrary{};
    unique_ptr<ShaderVariantLibrary> _variantCache{};
    unique_ptr<GraphicsPipelineStateLibrary> _psoCache{};
    unique_ptr<SamplerCache> _samplerCache{};
};

TEST_P(MeshPassExecutorTest, DrawsPerObjectColorIntoRenderTarget) {
    string reason;

    // 目标颜色 (r,g,b) 编码进 ObjectToWorld 首列。
    const float r = 0.25f, g = 0.5f, b = 0.75f;
    Eigen::Matrix4f m = Eigen::Matrix4f::Identity();
    m(0, 0) = r;
    m(1, 0) = g;
    m(2, 0) = b;
    m(3, 0) = 0.0f;

    // shader + material。
    ShaderPassDesc meshPass = MakeMeshPass();
    vector<ShaderPassDesc> passes;
    passes.push_back(meshPass);
    AssetManager mgr;
    auto shaderRef = mgr.AddReady<ShaderAsset>(Guid::NewGuid(),
        std::make_unique<ShaderAsset>(ShaderKeywordSet{}, std::move(passes)));
    MaterialAsset material{shaderRef};
    material.SetVector("gMaterial", Eigen::Vector4f{1.0f, 1.0f, 1.0f, 1.0f});  // Tint = 白, 不改色

    // proxy (自带全屏三角几何)。
    TriangleProxy proxy;
    ASSERT_TRUE(proxy.Init(_ctx, m, &reason)) << reason;

    // render target。
    TextureDescriptor rtDesc{};
    rtDesc.Dim = TextureDimension::Dim2D;
    rtDesc.Width = kRtSize;
    rtDesc.Height = kRtSize;
    rtDesc.DepthOrArraySize = 1;
    rtDesc.MipLevels = 1;
    rtDesc.SampleCount = 1;
    rtDesc.Format = kRtFormat;
    rtDesc.Memory = MemoryType::Device;
    rtDesc.Usage = TextureUse::RenderTarget | TextureUse::CopySource;
    auto rtOpt = _ctx.CreateTexture(rtDesc, &reason);
    ASSERT_TRUE(rtOpt.HasValue()) << reason;
    auto rt = rtOpt.Release();

    TextureViewDescriptor rtvDesc{};
    rtvDesc.Target = rt.get();
    rtvDesc.Dim = TextureDimension::Dim2D;
    rtvDesc.Format = kRtFormat;
    rtvDesc.Range = SubresourceRange{0, 1, 0, 1};
    rtvDesc.Usage = TextureViewUsage::RenderTarget;
    auto rtvOpt = _ctx.CreateTextureView(rtvDesc, &reason);
    ASSERT_TRUE(rtvOpt.HasValue()) << reason;
    auto rtv = rtvOpt.Release();
    RenderPassRegistry renderPassRegistry{_ctx.GetDevicePtr()};

    // readback buffer。
    const uint32_t bpp = GetTextureFormatBytesPerPixel(kRtFormat);
    const uint64_t rowAlign = std::max<uint64_t>(1, _ctx.GetDeviceDetail().TextureDataPitchAlignment);
    const uint64_t alignedRow = AlignUp<uint64_t>(static_cast<uint64_t>(kRtSize) * bpp, rowAlign);
    const uint64_t readbackSize = alignedRow * kRtSize;
    BufferDescriptor rbDesc{};
    rbDesc.Size = readbackSize;
    rbDesc.Memory = MemoryType::ReadBack;
    rbDesc.Usage = BufferUse::CopyDestination | BufferUse::MapRead;
    auto rbOpt = _ctx.CreateBuffer(rbDesc, &reason);
    ASSERT_TRUE(rbOpt.HasValue()) << reason;
    auto rb = rbOpt.Release();

    // draw list (提交材质渲染快照)。
    DrawList list;
    auto materialSnapshot = material.CreateSnapshot();
    ASSERT_TRUE(list.AddPrimitive(materialSnapshot, &proxy, "ForwardLit", 0, 1.0f));
    ASSERT_TRUE(list.AddPrimitive(materialSnapshot, &proxy, "ForwardLit", 0, 2.0f));
    list.SortOpaque();

    // executor (per-object cbuffer 变量名 gPerObject)。
    MeshPassExecutor executor{_ctx.GetDevicePtr(), _variantCache.get(), _psoCache.get(), _samplerCache.get(), "gPerObject"};
    FrameResources frameResources{_ctx.GetDevicePtr(), &_hostWrites};

    auto cmdOpt = _ctx.CreateCommandBuffer(&reason);
    ASSERT_TRUE(cmdOpt.HasValue()) << reason;
    auto cmd = cmdOpt.Release();

    _ctx.ClearCapturedErrors();
    cmd->Begin();

    // RT: Undefined -> RenderTarget。
    ResourceBarrierDescriptor toRt = BarrierTextureDescriptor{
        .Target = rt.get(),
        .Before = TextureState::Undefined,
        .After = TextureState::RenderTarget};
    cmd->ResourceBarrier(std::span{&toRt, 1});

    RenderPassColorAttachmentDescriptor color{
        .Format = kRtFormat,
        .SampleCount = 1,
        .Load = LoadAction::Clear,
        .Store = StoreAction::Store};
    RenderPassDescriptor rpDesc{
        .ColorAttachments = std::span{&color, 1}};
    auto renderPassOpt = renderPassRegistry.GetOrCreateRenderPass(rpDesc);
    ASSERT_TRUE(renderPassOpt.HasValue()) << _ctx.JoinCapturedErrors();
    TextureView* colorView = rtv.get();
    auto framebufferOpt = renderPassRegistry.GetOrCreateFramebuffer(
        renderPassOpt.Get(), std::span<TextureView* const>{&colorView, 1}, nullptr, kRtSize, kRtSize);
    ASSERT_TRUE(framebufferOpt.HasValue()) << _ctx.JoinCapturedErrors();
    const ColorClearValue clearValue{{0.0f, 0.0f, 0.0f, 1.0f}};
    RenderPassBeginDescriptor beginDesc{
        .Pass = renderPassOpt.Get(),
        .Target = framebufferOpt.Get(),
        .ColorClearValues = std::span{&clearValue, 1},
        .Name = "MeshPassExecutorTest"};
    auto encoderOpt = cmd->BeginRenderPass(beginDesc);
    ASSERT_TRUE(encoderOpt.HasValue()) << _ctx.JoinCapturedErrors();
    auto encoder = encoderOpt.Release();

    Viewport vp{.X = 0.0f, .Y = 0.0f, .Width = static_cast<float>(kRtSize), .Height = static_cast<float>(kRtSize), .MinDepth = 0.0f, .MaxDepth = 1.0f};
    if (_ctx.GetDevicePtr()->GetBackend() == RenderBackend::Vulkan) {
        vp.Y = static_cast<float>(kRtSize);
        vp.Height = -static_cast<float>(kRtSize);
    }
    encoder->SetViewport(vp);
    encoder->SetScissor(Rect{0, 0, kRtSize, kRtSize});

    executor.SetRenderPass(renderPassOpt.Get());
    const uint32_t submitted = executor.Execute(encoder.get(), list, frameResources);
    EXPECT_EQ(submitted, 2u) << _ctx.JoinCapturedErrors();
    EXPECT_EQ(frameResources.ObjectBindings.size(), 1u);
    EXPECT_EQ(frameResources.Counters.DescriptorGroupCreates, 2u);
    EXPECT_EQ(frameResources.Counters.DescriptorGroupUpdates, 2u);
    EXPECT_EQ(frameResources.Counters.DescriptorGroupBinds, 2u);
    EXPECT_EQ(frameResources.Counters.DynamicOffsetBinds, 1u);
    EXPECT_EQ(frameResources.Counters.PipelineBinds, 1u);
    EXPECT_EQ(frameResources.Counters.PipelineCacheMisses, 1u);
    EXPECT_EQ(frameResources.Counters.ShaderVariantCacheMisses, 1u);
    EXPECT_EQ(frameResources.Counters.DrawStateCacheHits, 1u);
    EXPECT_EQ(frameResources.Counters.DrawStateCacheMisses, 1u);
    EXPECT_EQ(frameResources.Counters.DrawCommandTemplateHits, 1u);
    EXPECT_EQ(frameResources.Counters.DrawCommandTemplateMisses, 1u);
    EXPECT_EQ(frameResources.Counters.MaterialGroupCacheHits, 1u);
    EXPECT_EQ(frameResources.Counters.MaterialGroupCacheMisses, 1u);
    EXPECT_EQ(frameResources.Counters.Draws, 2u);
    EXPECT_EQ(frameResources.Counters.DrawInstances, 2u);

    cmd->EndRenderPass(std::move(encoder));

    // RT -> CopySource, 拷到 readback。
    ResourceBarrierDescriptor toCopy = BarrierTextureDescriptor{
        .Target = rt.get(),
        .Before = TextureState::RenderTarget,
        .After = TextureState::CopySource};
    cmd->ResourceBarrier(std::span{&toCopy, 1});
    cmd->CopyTextureToBuffer(rb.get(), 0, rt.get(), SubresourceRange{0, 1, 0, 1});
    cmd->End();

    ASSERT_TRUE(SubmitAndWait(cmd.get(), &reason)) << reason << " " << _ctx.JoinCapturedErrors();
    EXPECT_EQ(renderPassRegistry.GetRenderPassCount(), 1u);
    EXPECT_EQ(renderPassRegistry.GetFramebufferCount(), 1u);

    // 模拟同一 flight fence 完成后的统一 FrameResources reset。
    frameResources.Reset();
    _ctx.ClearCapturedErrors();
    cmd->Begin();
    ResourceBarrierDescriptor toRtAgain = BarrierTextureDescriptor{
        .Target = rt.get(),
        .Before = TextureState::CopySource,
        .After = TextureState::RenderTarget};
    cmd->ResourceBarrier(std::span{&toRtAgain, 1});
    auto secondEncoderOpt = cmd->BeginRenderPass(beginDesc);
    ASSERT_TRUE(secondEncoderOpt.HasValue()) << _ctx.JoinCapturedErrors();
    auto secondEncoder = secondEncoderOpt.Release();
    secondEncoder->SetViewport(vp);
    secondEncoder->SetScissor(Rect{0, 0, kRtSize, kRtSize});
    EXPECT_EQ(executor.Execute(secondEncoder.get(), list, frameResources, 6), 2u) << _ctx.JoinCapturedErrors();
    EXPECT_EQ(frameResources.ObjectBindings.size(), 1u);
    EXPECT_EQ(frameResources.Counters.DescriptorGroupCreates, 0u);
    EXPECT_EQ(frameResources.Counters.DescriptorGroupUpdates, 0u);
    EXPECT_EQ(frameResources.Counters.DescriptorGroupBinds, 2u);
    EXPECT_EQ(frameResources.Counters.DynamicOffsetBinds, 1u);
    EXPECT_EQ(frameResources.Counters.PipelineBinds, 1u);
    EXPECT_EQ(frameResources.Counters.PipelineCacheHits, 0u);
    EXPECT_EQ(frameResources.Counters.PipelineCacheMisses, 0u);
    EXPECT_EQ(frameResources.Counters.ShaderVariantCacheHits, 0u);
    EXPECT_EQ(frameResources.Counters.ShaderVariantCacheMisses, 0u);
    EXPECT_EQ(frameResources.Counters.DrawStateCacheHits, 2u);
    EXPECT_EQ(frameResources.Counters.DrawCommandTemplateHits, 2u);
    EXPECT_EQ(frameResources.Counters.MaterialGroupCacheHits, 2u);
    EXPECT_EQ(frameResources.Counters.Draws, 2u);
    EXPECT_EQ(frameResources.Counters.DrawInstances, 12u);
    cmd->EndRenderPass(std::move(secondEncoder));
    cmd->End();
    ASSERT_TRUE(SubmitAndWait(cmd.get(), &reason)) << reason << " " << _ctx.JoinCapturedErrors();
    EXPECT_EQ(renderPassRegistry.GetRenderPassCount(), 1u);
    EXPECT_EQ(renderPassRegistry.GetFramebufferCount(), 1u);

    auto pixelsOpt = _ctx.ReadHostVisibleBuffer(rb.get(), readbackSize, &reason);
    ASSERT_TRUE(pixelsOpt.has_value()) << reason;
    const vector<byte>& pixels = pixelsOpt.value();

    // 取 (0,0) 像素, RGBA8_UNORM。
    auto px = [&](uint32_t x, uint32_t y) {
        const byte* row = pixels.data() + static_cast<size_t>(y) * alignedRow;
        const byte* p = row + static_cast<size_t>(x) * bpp;
        return std::array<uint8_t, 4>{
            static_cast<uint8_t>(p[0]),
            static_cast<uint8_t>(p[1]),
            static_cast<uint8_t>(p[2]),
            static_cast<uint8_t>(p[3])};
    };
    auto center = px(kRtSize / 2, kRtSize / 2);

    const uint8_t expR = static_cast<uint8_t>(r * 255.0f + 0.5f);
    const uint8_t expG = static_cast<uint8_t>(g * 255.0f + 0.5f);
    const uint8_t expB = static_cast<uint8_t>(b * 255.0f + 0.5f);
    // ±2 容差 (unorm 量化)。
    EXPECT_NEAR(center[0], expR, 2) << "R mismatch";
    EXPECT_NEAR(center[1], expG, 2) << "G mismatch";
    EXPECT_NEAR(center[2], expB, 2) << "B mismatch";
    EXPECT_EQ(_psoCache->Count(), 1u);
    EXPECT_EQ(_variantCache->Count(), 1u);
}

TEST_P(MeshPassExecutorTest, DrawsEmptyMaterialWithGenericMixedBindingsAndDefaults) {
    string reason;
    AssetManager assets;

    TextureDescriptor defaultTextureDesc{};
    defaultTextureDesc.Dim = TextureDimension::Dim2D;
    defaultTextureDesc.Width = 1;
    defaultTextureDesc.Height = 1;
    defaultTextureDesc.DepthOrArraySize = 1;
    defaultTextureDesc.MipLevels = 1;
    defaultTextureDesc.SampleCount = 1;
    defaultTextureDesc.Format = TextureFormat::RGBA8_UNORM;
    defaultTextureDesc.Memory = MemoryType::Device;
    defaultTextureDesc.Usage = TextureUse::Resource | TextureUse::CopyDestination;
    auto defaultTextureOpt = _ctx.CreateTexture(defaultTextureDesc, &reason);
    ASSERT_TRUE(defaultTextureOpt.HasValue()) << reason;
    auto defaultTexture = defaultTextureOpt.Release();
    const std::array<byte, 4> white{
        byte{255}, byte{255}, byte{255}, byte{255}};
    ASSERT_TRUE(_ctx.UploadTexture2D(defaultTexture.get(), white, &reason)) << reason;
    TextureViewDescriptor defaultSrvDesc{};
    defaultSrvDesc.Target = defaultTexture.get();
    defaultSrvDesc.Dim = TextureDimension::Dim2D;
    defaultSrvDesc.Format = TextureFormat::RGBA8_UNORM;
    defaultSrvDesc.Range = SubresourceRange::AllSub();
    defaultSrvDesc.Usage = TextureViewUsage::Resource;
    auto defaultSrvOpt = _ctx.CreateTextureView(defaultSrvDesc, &reason);
    ASSERT_TRUE(defaultSrvOpt.HasValue()) << reason;
    auto defaultSrv = defaultSrvOpt.Release();
    auto defaultTextureRef = assets.AddReady<TextureAsset>(
        Guid::NewGuid(),
        make_unique<TextureAsset>(
            _ctx.GetDevicePtr(),
            "generic_default",
            std::move(defaultTexture),
            std::move(defaultSrv)));

    SamplerDescriptor defaultSampler{};
    defaultSampler.AddressS = AddressMode::ClampToEdge;
    defaultSampler.AddressT = AddressMode::ClampToEdge;
    defaultSampler.AddressR = AddressMode::ClampToEdge;
    defaultSampler.MinFilter = FilterMode::Nearest;
    defaultSampler.MagFilter = FilterMode::Nearest;
    defaultSampler.MipmapFilter = FilterMode::Nearest;
    defaultSampler.LodMin = 0.0f;
    defaultSampler.LodMax = 0.0f;

    vector<ShaderPropertyDesc> properties{
        ShaderPropertyDesc{
            .Name = "Tint",
            .Kind = ShaderPropertyKind::Vector,
            .DefaultValue = Eigen::Vector4f{0.2f, 0.4f, 0.6f, 1.0f}},
        ShaderPropertyDesc{
            .Name = "DefaultTexture",
            .Kind = ShaderPropertyKind::Texture,
            .DefaultValue = defaultTextureRef},
        ShaderPropertyDesc{
            .Name = "DefaultSampler",
            .Kind = ShaderPropertyKind::Sampler,
            .DefaultValue = defaultSampler}};
    vector<ShaderPassDesc> passes;
    passes.push_back(MakeGenericBindingPass());
    auto shaderRef = assets.AddReady<ShaderAsset>(
        Guid::NewGuid(),
        make_unique<ShaderAsset>(
            ShaderKeywordSet{},
            std::move(passes),
            std::move(properties)));
    MaterialAsset material{shaderRef};

    TriangleProxy proxy;
    ASSERT_TRUE(proxy.Init(_ctx, Eigen::Matrix4f::Identity(), &reason)) << reason;
    DrawList list;
    auto materialSnapshot = material.CreateSnapshot();
    ASSERT_TRUE(list.AddPrimitive(materialSnapshot, &proxy, "ForwardLit"));

    TextureDescriptor rtDesc{};
    rtDesc.Dim = TextureDimension::Dim2D;
    rtDesc.Width = kRtSize;
    rtDesc.Height = kRtSize;
    rtDesc.DepthOrArraySize = 1;
    rtDesc.MipLevels = 1;
    rtDesc.SampleCount = 1;
    rtDesc.Format = kRtFormat;
    rtDesc.Memory = MemoryType::Device;
    rtDesc.Usage = TextureUse::RenderTarget | TextureUse::CopySource;
    auto rtOpt = _ctx.CreateTexture(rtDesc, &reason);
    ASSERT_TRUE(rtOpt.HasValue()) << reason;
    auto rt = rtOpt.Release();
    TextureViewDescriptor rtvDesc{};
    rtvDesc.Target = rt.get();
    rtvDesc.Dim = TextureDimension::Dim2D;
    rtvDesc.Format = kRtFormat;
    rtvDesc.Range = SubresourceRange{0, 1, 0, 1};
    rtvDesc.Usage = TextureViewUsage::RenderTarget;
    auto rtvOpt = _ctx.CreateTextureView(rtvDesc, &reason);
    ASSERT_TRUE(rtvOpt.HasValue()) << reason;
    auto rtv = rtvOpt.Release();

    const uint32_t bytesPerPixel = GetTextureFormatBytesPerPixel(kRtFormat);
    const uint64_t rowAlignment = std::max<uint64_t>(
        1, _ctx.GetDeviceDetail().TextureDataPitchAlignment);
    const uint64_t alignedRow = AlignUp<uint64_t>(
        static_cast<uint64_t>(kRtSize) * bytesPerPixel,
        rowAlignment);
    BufferDescriptor readbackDesc{};
    readbackDesc.Size = alignedRow * kRtSize;
    readbackDesc.Memory = MemoryType::ReadBack;
    readbackDesc.Usage = BufferUse::CopyDestination | BufferUse::MapRead;
    auto readbackOpt = _ctx.CreateBuffer(readbackDesc, &reason);
    ASSERT_TRUE(readbackOpt.HasValue()) << reason;
    auto readback = readbackOpt.Release();

    RenderPassRegistry registry{_ctx.GetDevicePtr()};
    RenderPassColorAttachmentDescriptor color{
        .Format = kRtFormat,
        .SampleCount = 1,
        .Load = LoadAction::Clear,
        .Store = StoreAction::Store};
    RenderPassDescriptor passDesc{.ColorAttachments = std::span{&color, 1}};
    auto renderPass = registry.GetOrCreateRenderPass(passDesc);
    ASSERT_TRUE(renderPass.HasValue());
    TextureView* colorView = rtv.get();
    auto framebuffer = registry.GetOrCreateFramebuffer(
        renderPass.Get(),
        std::span<TextureView* const>{&colorView, 1},
        nullptr,
        kRtSize,
        kRtSize);
    ASSERT_TRUE(framebuffer.HasValue());

    auto commandOpt = _ctx.CreateCommandBuffer(&reason);
    ASSERT_TRUE(commandOpt.HasValue()) << reason;
    auto command = commandOpt.Release();
    _ctx.ClearCapturedErrors();
    command->Begin();
    ResourceBarrierDescriptor toRenderTarget = BarrierTextureDescriptor{
        .Target = rt.get(),
        .Before = TextureState::Undefined,
        .After = TextureState::RenderTarget};
    command->ResourceBarrier(std::span{&toRenderTarget, 1});
    const ColorClearValue clear{{0.0f, 0.0f, 0.0f, 1.0f}};
    RenderPassBeginDescriptor begin{
        .Pass = renderPass.Get(),
        .Target = framebuffer.Get(),
        .ColorClearValues = std::span{&clear, 1},
        .Name = "GenericBindingDefaults"};
    auto encoderOpt = command->BeginRenderPass(begin);
    ASSERT_TRUE(encoderOpt.HasValue()) << _ctx.JoinCapturedErrors();
    auto encoder = encoderOpt.Release();
    Viewport viewport{
        .X = 0.0f,
        .Y = 0.0f,
        .Width = static_cast<float>(kRtSize),
        .Height = static_cast<float>(kRtSize),
        .MinDepth = 0.0f,
        .MaxDepth = 1.0f};
    if (_ctx.GetDevicePtr()->GetBackend() == RenderBackend::Vulkan) {
        viewport.Y = static_cast<float>(kRtSize);
        viewport.Height = -static_cast<float>(kRtSize);
    }
    encoder->SetViewport(viewport);
    encoder->SetScissor(Rect{0, 0, kRtSize, kRtSize});

    MeshPassExecutor executor{
        _ctx.GetDevicePtr(),
        _variantCache.get(),
        _psoCache.get(),
        _samplerCache.get()};
    FrameResources resources{_ctx.GetDevicePtr(), &_hostWrites};
    executor.SetRenderPass(renderPass.Get());
    EXPECT_EQ(executor.Execute(encoder.get(), list, resources), 1u)
        << _ctx.JoinCapturedErrors();
    EXPECT_EQ(resources.Counters.BindingPlanCacheMisses, 1u);
    EXPECT_EQ(resources.Counters.BindingResolutionFailures, 0u);
    EXPECT_EQ(resources.Counters.ErrorFallbackDraws, 0u);
    EXPECT_TRUE(std::ranges::any_of(
        resources.RetainedObjects,
        [&](const shared_ptr<const void>& retained) noexcept {
            return retained.get() == materialSnapshot.get();
        }));
    command->EndRenderPass(std::move(encoder));
    ResourceBarrierDescriptor toCopySource = BarrierTextureDescriptor{
        .Target = rt.get(),
        .Before = TextureState::RenderTarget,
        .After = TextureState::CopySource};
    command->ResourceBarrier(std::span{&toCopySource, 1});
    command->CopyTextureToBuffer(
        readback.get(),
        0,
        rt.get(),
        SubresourceRange{0, 1, 0, 1});
    command->End();
    ASSERT_TRUE(SubmitAndWait(command.get(), &reason))
        << reason << " " << _ctx.JoinCapturedErrors();
    EXPECT_TRUE(_ctx.GetCapturedErrors().empty()) << _ctx.JoinCapturedErrors();

    auto pixels = _ctx.ReadHostVisibleBuffer(readback.get(), readbackDesc.Size, &reason);
    ASSERT_TRUE(pixels.has_value()) << reason;
    const byte* center = pixels->data() +
                         static_cast<size_t>(kRtSize / 2) * alignedRow +
                         static_cast<size_t>(kRtSize / 2) * bytesPerPixel;
    EXPECT_NEAR(static_cast<uint8_t>(center[0]), 51, 2);
    EXPECT_NEAR(static_cast<uint8_t>(center[1]), 102, 2);
    EXPECT_NEAR(static_cast<uint8_t>(center[2]), 153, 2);
    EXPECT_EQ(static_cast<uint8_t>(center[3]), 255);

    DrawList invalidList;
    const auto addInvalidMaterial = [&](std::string_view source, bool pushConstant = false) {
        ShaderPassDesc invalidPass = MakeSimpleUnlitPass(source);
        if (pushConstant) {
            invalidPass.PushConstantBindings.push_back(
                PushConstantBinding{.Group = 0, .Binding = 0});
        }
        auto invalidShader = assets.AddReady<ShaderAsset>(
            Guid::NewGuid(),
            make_unique<ShaderAsset>(
                ShaderKeywordSet{},
                vector<ShaderPassDesc>{std::move(invalidPass)}));
        MaterialAsset invalidMaterial{invalidShader};
        return invalidList.AddPrimitive(
            invalidMaterial.CreateSnapshot(),
            &proxy,
            "ForwardLit");
    };
    ASSERT_TRUE(addInvalidMaterial(kMissingTextureSource));
    ASSERT_TRUE(addInvalidMaterial(kUnsupportedBufferSource));
    ASSERT_TRUE(addInvalidMaterial(kUnsupportedUavSource));
    ASSERT_TRUE(addInvalidMaterial(kUnsupportedArraySource));
    ASSERT_TRUE(addInvalidMaterial(kUnsupportedBindlessSource));
    ASSERT_TRUE(addInvalidMaterial(kUnsupportedPushConstantSource, true));

    auto wrongTypeShader = assets.AddReady<ShaderAsset>(
        Guid::NewGuid(),
        make_unique<ShaderAsset>(
            ShaderKeywordSet{},
            vector<ShaderPassDesc>{MakeMeshPass()}));
    MaterialAsset wrongTypeMaterial{wrongTypeShader};
    wrongTypeMaterial.SetFloat("gMaterial", 1.0f);
    ASSERT_TRUE(invalidList.AddPrimitive(
        wrongTypeMaterial.CreateSnapshot(),
        &proxy,
        "ForwardLit"));

    ShaderPassDesc missingProviderPass = MakeSimpleUnlitPass(kMissingViewProviderSource);
    missingProviderPass.ParameterSources.push_back(ShaderParameterSourceDesc{
        .Name = "MissingView",
        .Scope = ShaderParameterScope::View,
        .ProviderName = "MissingView"});
    auto missingProviderShader = assets.AddReady<ShaderAsset>(
        Guid::NewGuid(),
        make_unique<ShaderAsset>(
            ShaderKeywordSet{},
            vector<ShaderPassDesc>{std::move(missingProviderPass)}));
    MaterialAsset missingProviderMaterial{missingProviderShader};
    ASSERT_TRUE(invalidList.AddPrimitive(
        missingProviderMaterial.CreateSnapshot(),
        &proxy,
        "ForwardLit"));

    auto errorShader = assets.AddReady<ShaderAsset>(
        Guid::NewGuid(),
        make_unique<ShaderAsset>(
            ShaderKeywordSet{},
            vector<ShaderPassDesc>{MakeSimpleUnlitPass(kErrorMaterialSource, true)}));
    MaterialAsset errorMaterial{errorShader};
    auto errorSnapshot = errorMaterial.CreateSnapshot();

    _ctx.ClearCapturedErrors();
    command->Begin();
    ResourceBarrierDescriptor backToRenderTarget = BarrierTextureDescriptor{
        .Target = rt.get(),
        .Before = TextureState::CopySource,
        .After = TextureState::RenderTarget};
    command->ResourceBarrier(std::span{&backToRenderTarget, 1});
    auto errorEncoderOpt = command->BeginRenderPass(begin);
    ASSERT_TRUE(errorEncoderOpt.HasValue()) << _ctx.JoinCapturedErrors();
    auto errorEncoder = errorEncoderOpt.Release();
    errorEncoder->SetViewport(viewport);
    errorEncoder->SetScissor(Rect{0, 0, kRtSize, kRtSize});

    MeshPassExecutor fallbackExecutor{
        _ctx.GetDevicePtr(),
        _variantCache.get(),
        _psoCache.get(),
        _samplerCache.get(),
        "PerObject",
        nullptr,
        errorSnapshot};
    FrameResources fallbackResources{_ctx.GetDevicePtr(), &_hostWrites};
    fallbackExecutor.SetRenderPass(renderPass.Get());
    EXPECT_EQ(
        fallbackExecutor.Execute(errorEncoder.get(), invalidList, fallbackResources),
        invalidList.Size());
    EXPECT_EQ(
        fallbackResources.Counters.BindingResolutionFailures,
        invalidList.Size());
    EXPECT_EQ(
        fallbackResources.Counters.ErrorFallbackDraws,
        invalidList.Size());
    EXPECT_EQ(fallbackResources.Counters.Draws, invalidList.Size());
    _ctx.ClearCapturedErrors();
    command->EndRenderPass(std::move(errorEncoder));
    ResourceBarrierDescriptor errorToCopySource = BarrierTextureDescriptor{
        .Target = rt.get(),
        .Before = TextureState::RenderTarget,
        .After = TextureState::CopySource};
    command->ResourceBarrier(std::span{&errorToCopySource, 1});
    command->CopyTextureToBuffer(
        readback.get(),
        0,
        rt.get(),
        SubresourceRange{0, 1, 0, 1});
    command->End();
    ASSERT_TRUE(SubmitAndWait(command.get(), &reason))
        << reason << " " << _ctx.JoinCapturedErrors();
    EXPECT_TRUE(_ctx.GetCapturedErrors().empty()) << _ctx.JoinCapturedErrors();

    auto fallbackPixels = _ctx.ReadHostVisibleBuffer(
        readback.get(), readbackDesc.Size, &reason);
    ASSERT_TRUE(fallbackPixels.has_value()) << reason;
    const byte* fallbackCenter = fallbackPixels->data() +
                                 static_cast<size_t>(kRtSize / 2) * alignedRow +
                                 static_cast<size_t>(kRtSize / 2) * bytesPerPixel;
    EXPECT_EQ(static_cast<uint8_t>(fallbackCenter[0]), 255);
    EXPECT_EQ(static_cast<uint8_t>(fallbackCenter[1]), 0);
    EXPECT_EQ(static_cast<uint8_t>(fallbackCenter[2]), 255);
    EXPECT_EQ(static_cast<uint8_t>(fallbackCenter[3]), 255);
}

TEST_P(MeshPassExecutorTest, ConstantPoolsAlignGrowAndDelayReuseUntilRelease) {
    const uint64_t alignment = std::max<uint64_t>(
        256, _ctx.GetDeviceDetail().CBufferAlignment);

    DynamicCBufferArena arena{
        _ctx.GetDevicePtr(),
        &_hostWrites,
        DynamicCBufferArena::Descriptor{
            .BasicSize = alignment * 2,
            .Alignment = alignment,
            .MaxResetSize = alignment * 16,
            .NamePrefix = "arena_test"}};
    auto firstReservation = arena.Reserve(1);
    auto secondReservation = arena.Reserve(1);
    auto thirdReservation = arena.Reserve(1);
    const auto first = firstReservation.Commit(1);
    const auto second = secondReservation.Commit(1);
    const auto third = thirdReservation.Commit(1);
    ASSERT_NE(first.Target, nullptr);
    ASSERT_NE(second.Target, nullptr);
    ASSERT_NE(third.Target, nullptr);
    EXPECT_EQ(first.Offset, 0u);
    EXPECT_EQ(second.Offset, alignment);
    EXPECT_EQ(first.Target, second.Target);
    EXPECT_NE(first.Target, third.Target);
    EXPECT_EQ(third.Target->GetDesc().Size, alignment * 4);
    EXPECT_EQ(arena.GetHighWatermark(), alignment * 3);

    arena.Reset();
    auto afterResetReservation = arena.Reserve(1);
    const auto afterReset = afterResetReservation.Commit(1);
    ASSERT_NE(afterReset.Target, nullptr);
    EXPECT_EQ(afterReset.Offset, 0u);
    EXPECT_EQ(afterReset.Target->GetDesc().Size, alignment * 8);

    MaterialConstantPool materialPool{_ctx.GetDevicePtr(), alignment * 2, alignment};
    auto materialAReservation = materialPool.Reserve(16, _hostWrites);
    auto materialBReservation = materialPool.Reserve(16, _hostWrites);
    auto materialCReservation = materialPool.Reserve(16, _hostWrites);
    const auto materialA = materialAReservation.Commit(16);
    const auto materialB = materialBReservation.Commit(16);
    const auto materialC = materialCReservation.Commit(16);
    ASSERT_TRUE(materialA.IsValid());
    ASSERT_TRUE(materialB.IsValid());
    ASSERT_TRUE(materialC.IsValid());
    EXPECT_EQ(materialA.Offset, 0u);
    EXPECT_EQ(materialB.Offset, alignment);
    EXPECT_EQ(materialA.Target, materialB.Target);
    EXPECT_NE(materialA.Target, materialC.Target);
    EXPECT_EQ(materialC.Target->GetDesc().Size, alignment * 4);

    auto whileRetainedReservation = materialPool.Reserve(16, _hostWrites);
    const auto whileRetained = whileRetainedReservation.Commit(16);
    EXPECT_NE(whileRetained.Target, materialA.Target);
    materialPool.Release(materialA);
    auto afterReleaseReservation = materialPool.Reserve(16, _hostWrites);
    const auto afterRelease = afterReleaseReservation.Commit(16);
    EXPECT_EQ(afterRelease.Target, materialA.Target);
    EXPECT_EQ(afterRelease.Offset, materialA.Offset);
}

TEST_P(MeshPassExecutorTest, RenderPassRegistryReusesAndInvalidatesObjects) {
    string reason{};
    TextureDescriptor textureDesc{
        .Dim = TextureDimension::Dim2D,
        .Width = 8,
        .Height = 8,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleCount = 1,
        .Format = kRtFormat,
        .Memory = MemoryType::Device,
        .Usage = TextureUse::RenderTarget};
    auto textureOpt = _ctx.CreateTexture(textureDesc, &reason);
    ASSERT_TRUE(textureOpt.HasValue()) << reason;
    auto texture = textureOpt.Release();
    TextureViewDescriptor viewDesc{
        .Target = texture.get(),
        .Dim = TextureDimension::Dim2D,
        .Format = kRtFormat,
        .Range = SubresourceRange{0, 1, 0, 1},
        .Usage = TextureViewUsage::RenderTarget};
    auto viewOpt = _ctx.CreateTextureView(viewDesc, &reason);
    ASSERT_TRUE(viewOpt.HasValue()) << reason;
    auto view = viewOpt.Release();
    RenderPassRegistry registry{_ctx.GetDevicePtr()};

    RenderPassColorAttachmentDescriptor color{
        .Format = kRtFormat,
        .SampleCount = 1,
        .Load = LoadAction::Clear,
        .Store = StoreAction::Store};
    RenderPassDescriptor passDesc{.ColorAttachments = std::span{&color, 1}};
    auto passA = registry.GetOrCreateRenderPass(passDesc);
    auto passB = registry.GetOrCreateRenderPass(passDesc);
    ASSERT_TRUE(passA.HasValue());
    ASSERT_TRUE(passB.HasValue());
    EXPECT_EQ(passA.Get(), passB.Get());
    EXPECT_EQ(registry.GetRenderPassCount(), 1u);
    EXPECT_EQ(registry.GetRenderPassMissCount(), 1u);
    EXPECT_EQ(registry.GetRenderPassHitCount(), 1u);

    TextureView* colorView = view.get();
    auto framebufferA = registry.GetOrCreateFramebuffer(
        passA.Get(), std::span<TextureView* const>{&colorView, 1}, nullptr, 8, 8);
    auto framebufferB = registry.GetOrCreateFramebuffer(
        passA.Get(), std::span<TextureView* const>{&colorView, 1}, nullptr, 8, 8);
    ASSERT_TRUE(framebufferA.HasValue());
    ASSERT_TRUE(framebufferB.HasValue());
    EXPECT_EQ(framebufferA.Get(), framebufferB.Get());
    EXPECT_EQ(registry.GetFramebufferCount(), 1u);

    registry.RemoveFramebuffersUsing(view.get());
    EXPECT_EQ(registry.GetFramebufferCount(), 0u);
    auto recreated = registry.GetOrCreateFramebuffer(
        passA.Get(), std::span<TextureView* const>{&colorView, 1}, nullptr, 8, 8);
    EXPECT_TRUE(recreated.HasValue());
    EXPECT_EQ(registry.GetFramebufferCount(), 1u);

    color.Load = LoadAction::Load;
    auto loadPass = registry.GetOrCreateRenderPass(passDesc);
    ASSERT_TRUE(loadPass.HasValue());
    EXPECT_NE(loadPass.Get(), passA.Get());
    EXPECT_EQ(registry.GetRenderPassCount(), 2u);
}

INSTANTIATE_TEST_SUITE_P(
    RenderBackends,
    MeshPassExecutorTest,
    ::testing::ValuesIn(GetEnabledTestBackends()),
    [](const ::testing::TestParamInfo<TestBackend>& info) {
        return string{fmt::format("{}", info.param)};
    });

}  // namespace
}  // namespace radray::render::test
