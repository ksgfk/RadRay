#include "render_test_framework.h"

#include <gtest/gtest.h>
#include <fmt/format.h>

#include <array>
#include <cstring>

#include <radray/basic_math.h>
#include <radray/render/gpu_resource.h>
#include <radray/render/pipeline_state_cache.h>
#include <radray/render/shader_variant_cache.h>
#include <radray/render/shader_compiler/dxc.h>
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
[[vk::push_constant]] ConstantBuffer<MaterialParams> gMaterial : register(b0, space0);

struct PerObject {
    float4x4 ObjectToWorld;
};
[[vk::binding(0, 1)]] ConstantBuffer<PerObject> gPerObject : register(b1, space1);

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
    RenderMesh::DrawData _draw{};
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
        _variantCache = CreateShaderVariantCache(
                            _ctx.GetDevicePtr(), _ctx.GetDxc(), _ctx.GetShaderBindingLayoutCache())
                            .Release();
        _psoCache = _ctx.GetDevicePtr()->CreateGraphicsPipelineStateCache().Release();
    }

    ComputeTestContext _ctx{};
    unique_ptr<ShaderVariantCache> _variantCache{};
    unique_ptr<GraphicsPipelineStateCache> _psoCache{};
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
    ShaderAsset shader{ShaderKeywordSet{}, std::move(passes)};
    MaterialAsset material{&shader};
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

    // draw list。
    DrawList list;
    ASSERT_TRUE(list.AddPrimitive(&material, &proxy, "ForwardLit", 0, 1.0f));
    list.SortOpaque();

    // executor (per-object cbuffer 变量名 gPerObject)。
    MeshPassExecutor executor{_ctx.GetDevicePtr(), _variantCache.get(), _psoCache.get(), "gPerObject"};
    executor.BeginFrame();

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

    ColorAttachment color{
        .Target = rtv.get(),
        .Load = LoadAction::Clear,
        .Store = StoreAction::Store,
        .ClearValue = ColorClearValue{{0.0f, 0.0f, 0.0f, 1.0f}}};
    RenderPassDescriptor rpDesc{
        .ColorAttachments = std::span{&color, 1},
        .Name = "MeshPassExecutorTest"};
    auto encoderOpt = cmd->BeginRenderPass(rpDesc);
    ASSERT_TRUE(encoderOpt.HasValue()) << _ctx.JoinCapturedErrors();
    auto encoder = encoderOpt.Release();

    Viewport vp{.X = 0.0f, .Y = 0.0f, .Width = static_cast<float>(kRtSize), .Height = static_cast<float>(kRtSize), .MinDepth = 0.0f, .MaxDepth = 1.0f};
    if (_ctx.GetDevicePtr()->GetBackend() == RenderBackend::Vulkan) {
        vp.Y = static_cast<float>(kRtSize);
        vp.Height = -static_cast<float>(kRtSize);
    }
    encoder->SetViewport(vp);
    encoder->SetScissor(Rect{0, 0, kRtSize, kRtSize});

    const uint32_t submitted = executor.Execute(encoder.get(), list);
    EXPECT_EQ(submitted, 1u) << _ctx.JoinCapturedErrors();

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

INSTANTIATE_TEST_SUITE_P(
    RenderBackends,
    MeshPassExecutorTest,
    ::testing::ValuesIn(GetEnabledTestBackends()),
    [](const ::testing::TestParamInfo<TestBackend>& info) {
        return string{fmt::format("{}", info.param)};
    });

}  // namespace
}  // namespace radray::render::test
