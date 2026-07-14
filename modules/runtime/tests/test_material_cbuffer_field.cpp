#include "render_test_framework.h"

#include <gtest/gtest.h>
#include <fmt/format.h>

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
#include <radray/runtime/render_framework/material_property_block.h>
#include <radray/runtime/render_framework/mesh_pass_executor.h>
#include <radray/runtime/render_framework/primitive_scene_proxy.h>
#include <radray/runtime/render_framework/render_queue.h>

namespace radray::render::test {
namespace {

// 全屏三角 mesh pass, 材质参数走【真实 cbuffer】(非 push constant), 且按【字段名】设置。
// - Material cbuffer (b0, space2): 普通 cbuffer, 含两个字段 BaseColor / Extra。
//   材质用 SetVector("BaseColor", ...) 按字段名设置; ShaderBindingPlan 应把它按反射
//   偏移打进 Material 块并整块 (CBV) 提交。
// - PerObject cbuffer (b1, space0): 系统块, 承载 ObjectToWorld, 由执行器单独填充, 打包器跳过。
// PS 输出 = BaseColor.rgb, 从而验证按字段名设置的值确实通到了 GPU。
constexpr std::string_view kFieldPackSource = R"(
[[vk::binding(0, 2)]] cbuffer Material : register(b0, space2) {
    float4 BaseColor;
    float4 Extra;
};

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
    o.Pos = mul(gPerObject.ObjectToWorld, float4(i.Pos, 0.0, 1.0));
    return o;
}

float4 PSMain(VSOut i) : SV_TARGET {
    return float4(BaseColor.rgb, 1.0);
}
)";

constexpr uint32_t kRtSize = 4;
constexpr TextureFormat kRtFormat = TextureFormat::RGBA8_UNORM;

class TriangleProxy : public PrimitiveSceneProxy {
public:
    bool Init(ComputeTestContext& ctx, string* reason) {
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

    Eigen::Matrix4f GetLocalToWorld() const noexcept override { return Eigen::Matrix4f::Identity(); }

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
    unique_ptr<Buffer> _vb;
    unique_ptr<Buffer> _ib;
    GpuMesh::DrawData _draw{};
};

ShaderPassDesc MakeFieldPackPass() {
    ShaderPassDesc pass{};
    pass.PassTag = "ForwardLit";
    pass.Source = string{kFieldPackSource};
    pass.VertexEntry = "VSMain";
    pass.PixelEntry = "PSMain";
    pass.Primitive = PrimitiveState::Default();
    pass.Primitive.Cull = CullMode::None;
    pass.MultiSample = MultiSampleState::Default();
    pass.ColorTargets.push_back(ColorTargetState::Default(kRtFormat));
    pass.DynamicBufferBindings.push_back(DynamicBufferBinding{.Group = 0, .Binding = 1});
    pass.ParameterSources.push_back(ShaderParameterSourceDesc{
        .Name = "gPerObject",
        .Scope = ShaderParameterScope::Object,
        .ProviderName = "gPerObject"});
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

class MaterialCBufferFieldTest : public ::testing::TestWithParam<TestBackend> {
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

    // 用给定的材质快照渲染全屏三角, 回读中心像素 RGB。失败时通过 ok=false 返回。
    std::array<uint8_t, 4> RenderCenterPixel(
        shared_ptr<const MaterialRenderSnapshot> snapshot,
        bool& ok,
        string& reason) {
        ok = false;
        std::array<uint8_t, 4> result{0, 0, 0, 0};

        TriangleProxy proxy;
        if (!proxy.Init(_ctx, &reason)) {
            return result;
        }

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
        if (!rtOpt.HasValue()) {
            return result;
        }
        auto rt = rtOpt.Release();

        TextureViewDescriptor rtvDesc{};
        rtvDesc.Target = rt.get();
        rtvDesc.Dim = TextureDimension::Dim2D;
        rtvDesc.Format = kRtFormat;
        rtvDesc.Range = SubresourceRange{0, 1, 0, 1};
        rtvDesc.Usage = TextureViewUsage::RenderTarget;
        auto rtvOpt = _ctx.CreateTextureView(rtvDesc, &reason);
        if (!rtvOpt.HasValue()) {
            return result;
        }
        auto rtv = rtvOpt.Release();
        RenderPassRegistry renderPassRegistry{_ctx.GetDevicePtr()};

        const uint32_t bpp = GetTextureFormatBytesPerPixel(kRtFormat);
        const uint64_t rowAlign = std::max<uint64_t>(1, _ctx.GetDeviceDetail().TextureDataPitchAlignment);
        const uint64_t alignedRow = AlignUp<uint64_t>(static_cast<uint64_t>(kRtSize) * bpp, rowAlign);
        const uint64_t readbackSize = alignedRow * kRtSize;
        BufferDescriptor rbDesc{};
        rbDesc.Size = readbackSize;
        rbDesc.Memory = MemoryType::ReadBack;
        rbDesc.Usage = BufferUse::CopyDestination | BufferUse::MapRead;
        auto rbOpt = _ctx.CreateBuffer(rbDesc, &reason);
        if (!rbOpt.HasValue()) {
            return result;
        }
        auto rb = rbOpt.Release();

        DrawList list;
        if (!list.AddPrimitive(std::move(snapshot), &proxy, "ForwardLit", 0, 1.0f)) {
            reason = "AddPrimitive failed";
            return result;
        }
        list.SortOpaque();

        MeshPassExecutor executor{_ctx.GetDevicePtr(), _variantCache.get(), _psoCache.get(), _samplerCache.get(), "gPerObject"};
        FrameResources frameResources{_ctx.GetDevicePtr(), &_hostWrites};

        auto cmdOpt = _ctx.CreateCommandBuffer(&reason);
        if (!cmdOpt.HasValue()) {
            return result;
        }
        auto cmd = cmdOpt.Release();

        _ctx.ClearCapturedErrors();
        cmd->Begin();

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
        TextureView* colorView = rtv.get();
        auto framebufferOpt = renderPassOpt.HasValue()
                                  ? renderPassRegistry.GetOrCreateFramebuffer(
                                        renderPassOpt.Get(), std::span<TextureView* const>{&colorView, 1},
                                        nullptr, kRtSize, kRtSize)
                                  : Nullable<Framebuffer*>{};
        if (!renderPassOpt.HasValue() || !framebufferOpt.HasValue()) {
            reason = _ctx.JoinCapturedErrors();
            return result;
        }
        const ColorClearValue clearValue{{0.0f, 0.0f, 0.0f, 1.0f}};
        RenderPassBeginDescriptor beginDesc{
            .Pass = renderPassOpt.Get(),
            .Target = framebufferOpt.Get(),
            .ColorClearValues = std::span{&clearValue, 1},
            .Name = "MaterialCBufferFieldTest"};
        auto encoderOpt = cmd->BeginRenderPass(beginDesc);
        if (!encoderOpt.HasValue()) {
            reason = _ctx.JoinCapturedErrors();
            return result;
        }
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
        if (submitted != 1u) {
            reason = "submitted != 1: " + _ctx.JoinCapturedErrors();
            cmd->EndRenderPass(std::move(encoder));
            return result;
        }

        cmd->EndRenderPass(std::move(encoder));

        ResourceBarrierDescriptor toCopy = BarrierTextureDescriptor{
            .Target = rt.get(),
            .Before = TextureState::RenderTarget,
            .After = TextureState::CopySource};
        cmd->ResourceBarrier(std::span{&toCopy, 1});
        cmd->CopyTextureToBuffer(rb.get(), 0, rt.get(), SubresourceRange{0, 1, 0, 1});
        cmd->End();

        if (!SubmitAndWait(cmd.get(), &reason)) {
            return result;
        }

        auto pixelsOpt = _ctx.ReadHostVisibleBuffer(rb.get(), readbackSize, &reason);
        if (!pixelsOpt.has_value()) {
            return result;
        }
        const vector<byte>& pixels = pixelsOpt.value();
        const uint32_t x = kRtSize / 2, y = kRtSize / 2;
        const byte* row = pixels.data() + static_cast<size_t>(y) * alignedRow;
        const byte* p = row + static_cast<size_t>(x) * bpp;
        result = {
            static_cast<uint8_t>(p[0]),
            static_cast<uint8_t>(p[1]),
            static_cast<uint8_t>(p[2]),
            static_cast<uint8_t>(p[3])};
        ok = true;
        return result;
    }

    static StreamingAssetRef<ShaderAsset> MakeFieldPackShader(AssetManager& mgr) {
        ShaderPassDesc pass = MakeFieldPackPass();
        vector<ShaderPassDesc> passes;
        passes.push_back(pass);
        return mgr.AddReady<ShaderAsset>(Guid::NewGuid(),
            std::make_unique<ShaderAsset>(ShaderKeywordSet{}, std::move(passes)));
    }

    ComputeTestContext _ctx{};
    HostWriteBatch _hostWrites{};
    unique_ptr<PipelineLayoutLibrary> _layoutLibrary{};
    unique_ptr<ShaderVariantLibrary> _variantCache{};
    unique_ptr<GraphicsPipelineStateLibrary> _psoCache{};
    unique_ptr<SamplerCache> _samplerCache{};
};

TEST_P(MaterialCBufferFieldTest, SetFieldByNamePacksIntoCBuffer) {
    string reason;
    // 目标颜色: 按【字段名】BaseColor 设置 (不是块名 Material)。
    const float r = 0.25f, g = 0.5f, b = 0.75f;

    AssetManager mgr;
    auto shaderRef = MakeFieldPackShader(mgr);
    MaterialAsset material{shaderRef};
    // 关键: 按字段名设置, 且 Extra 不设 (应零填充)。
    material.SetVector("BaseColor", Eigen::Vector4f{r, g, b, 1.0f});

    bool ok = false;
    auto center = RenderCenterPixel(material.CreateSnapshot(), ok, reason);
    ASSERT_TRUE(ok) << reason;

    const uint8_t expR = static_cast<uint8_t>(r * 255.0f + 0.5f);
    const uint8_t expG = static_cast<uint8_t>(g * 255.0f + 0.5f);
    const uint8_t expB = static_cast<uint8_t>(b * 255.0f + 0.5f);
    EXPECT_NEAR(center[0], expR, 2) << "R mismatch (field-packed BaseColor.r)";
    EXPECT_NEAR(center[1], expG, 2) << "G mismatch (field-packed BaseColor.g)";
    EXPECT_NEAR(center[2], expB, 2) << "B mismatch (field-packed BaseColor.b)";
}

TEST_P(MaterialCBufferFieldTest, PropertyBlockOverridesFieldValue) {
    string reason;
    AssetManager mgr;
    auto shaderRef = MakeFieldPackShader(mgr);

    // 共享材质: BaseColor = 红。
    MaterialAsset material{shaderRef};
    material.SetVector("BaseColor", Eigen::Vector4f{1.0f, 0.0f, 0.0f, 1.0f});

    // per-primitive 覆盖: BaseColor = 目标绿蓝色 (按字段名覆盖)。
    const float r = 0.0f, g = 0.6f, bl = 0.8f;
    MaterialPropertyBlock block;
    block.SetVector("BaseColor", Eigen::Vector4f{r, g, bl, 1.0f});

    bool ok = false;
    auto center = RenderCenterPixel(material.CreateSnapshot(&block), ok, reason);
    ASSERT_TRUE(ok) << reason;

    // 应看到覆盖色, 而非共享材质的红色。
    const uint8_t expR = static_cast<uint8_t>(r * 255.0f + 0.5f);
    const uint8_t expG = static_cast<uint8_t>(g * 255.0f + 0.5f);
    const uint8_t expB = static_cast<uint8_t>(bl * 255.0f + 0.5f);
    EXPECT_NEAR(center[0], expR, 2) << "R mismatch (overridden BaseColor.r)";
    EXPECT_NEAR(center[1], expG, 2) << "G mismatch (overridden BaseColor.g)";
    EXPECT_NEAR(center[2], expB, 2) << "B mismatch (overridden BaseColor.b)";

    // 共享材质模板未被覆盖污染。
    EXPECT_TRUE(material.GetVector("BaseColor").value().isApprox(Eigen::Vector4f{1.0f, 0.0f, 0.0f, 1.0f}));
}

INSTANTIATE_TEST_SUITE_P(
    RenderBackends,
    MaterialCBufferFieldTest,
    ::testing::ValuesIn(GetEnabledTestBackends()),
    [](const ::testing::TestParamInfo<TestBackend>& info) {
        return string{fmt::format("{}", info.param)};
    });

}  // namespace
}  // namespace radray::render::test
