#include "render_test_framework.h"

#include <gtest/gtest.h>
#include <fmt/format.h>

#include <array>
#include <cstring>

#include <radray/basic_math.h>
#include <radray/runtime/gpu_resource.h>
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

    ComputeTestContext _ctx{};
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
    FrameResources frameResources{_ctx.GetDevicePtr()};

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

    ASSERT_TRUE(_ctx.SubmitAndWait(cmd.get(), &reason)) << reason << " " << _ctx.JoinCapturedErrors();
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
    ASSERT_TRUE(_ctx.SubmitAndWait(cmd.get(), &reason)) << reason << " " << _ctx.JoinCapturedErrors();
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

TEST_P(MeshPassExecutorTest, ConstantPoolsAlignGrowAndDelayReuseUntilRelease) {
    const uint64_t alignment = std::max<uint64_t>(
        256, _ctx.GetDeviceDetail().CBufferAlignment);

    DynamicCBufferArena arena{
        _ctx.GetDevicePtr(),
        DynamicCBufferArena::Descriptor{
            .BasicSize = alignment * 2,
            .Alignment = alignment,
            .MaxResetSize = alignment * 16,
            .NamePrefix = "arena_test"}};
    const auto first = arena.Allocate(1);
    const auto second = arena.Allocate(1);
    const auto third = arena.Allocate(1);
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
    const auto afterReset = arena.Allocate(1);
    ASSERT_NE(afterReset.Target, nullptr);
    EXPECT_EQ(afterReset.Offset, 0u);
    EXPECT_EQ(afterReset.Target->GetDesc().Size, alignment * 8);

    MaterialConstantPool materialPool{_ctx.GetDevicePtr(), alignment * 2, alignment};
    const auto materialA = materialPool.Allocate(16);
    const auto materialB = materialPool.Allocate(16);
    const auto materialC = materialPool.Allocate(16);
    ASSERT_TRUE(materialA.IsValid());
    ASSERT_TRUE(materialB.IsValid());
    ASSERT_TRUE(materialC.IsValid());
    EXPECT_EQ(materialA.Offset, 0u);
    EXPECT_EQ(materialB.Offset, alignment);
    EXPECT_EQ(materialA.Target, materialB.Target);
    EXPECT_NE(materialA.Target, materialC.Target);
    EXPECT_EQ(materialC.Target->GetDesc().Size, alignment * 4);

    const auto whileRetained = materialPool.Allocate(16);
    EXPECT_NE(whileRetained.Target, materialA.Target);
    materialPool.Release(materialA);
    const auto afterRelease = materialPool.Allocate(16);
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
