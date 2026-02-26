#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>

#include <gtest/gtest.h>

#include <radray/image_data.h>
#include <radray/render/common.h>
#include <radray/render/debug.h>
#include <radray/render/dxc.h>

using namespace radray;
using namespace radray::render;

constexpr uint32_t kWidth = 256;
constexpr uint32_t kHeight = 256;

const char* kShaderSrc = R"(const static float3 g_Color[3] = {
    float3(1, 0, 0),
    float3(0, 1, 0),
    float3(0, 0, 1)};

struct V2P {
    float4 pos : SV_POSITION;
    float3 color: COLOR0;
};

V2P VSMain(float3 v_vert: POSITION, uint vertId: SV_VertexID) {
    V2P v2p;
    v2p.pos = float4(v_vert, 1);
    v2p.color = g_Color[vertId % 3];
    return v2p;
}

float4 PSMain(V2P v2p) : SV_Target {
    return float4(v2p.color, 1);
})";

class TestTriangle final : public OffScreenTestContext {
public:
    using OffScreenTestContext::OffScreenTestContext;

    ~TestTriangle() noexcept {
        _vertBuf.reset();
        _idxBuf.reset();
        _pso.reset();
        _rs.reset();
    }

    void Init(CommandBuffer* cmd, Fence* fence) override {
        auto shaders = CompileRasterShaders(kShaderSrc);
        auto rs = _device->CreateRootSignature({}).Unwrap();
        VertexElement elems[] = {
            {0, "POSITION", 0, VertexFormat::FLOAT32X3, 0}};
        VertexBufferLayout layout{12, VertexStepMode::Vertex, elems};
        ColorTargetState colorTarget = ColorTargetState::Default(TextureFormat::RGBA8_UNORM);
        RootSignatureDescriptor rsDesc{};
        _rs = _device->CreateRootSignature(rsDesc).Unwrap();
        GraphicsPipelineStateDescriptor psoDesc{
            _rs.get(),
            ShaderEntry{shaders.VS.get(), shaders.VSEntry},
            ShaderEntry{shaders.PS.get(), shaders.PSEntry},
            std::span{&layout, 1},
            PrimitiveState::Default(),
            std::nullopt,
            MultiSampleState::Default(),
            std::span{&colorTarget, 1}};
        _pso = _device->CreateGraphicsPipelineState(psoDesc).Unwrap();
        constexpr float vertices[] = {
            0.0f, 0.5f, 0.0f,
            -0.5f, -0.366f, 0.0f,
            0.5f, -0.366f, 0.0f};
        constexpr uint16_t indices[] = {0, 2, 1};
        constexpr auto vertexSize = sizeof(vertices);
        constexpr auto indexSize = sizeof(indices);
        _vertBuf = _device->CreateBuffer({vertexSize, MemoryType::Device, BufferUse::CopyDestination | BufferUse::Vertex, ResourceHint::None, "vb"}).Unwrap();
        _idxBuf = _device->CreateBuffer({indexSize, MemoryType::Device, BufferUse::CopyDestination | BufferUse::Index, ResourceHint::None, "ib"}).Unwrap();
        UploadBuffer(_vertBuf.get(), std::as_bytes(std::span{vertices}));
        UploadBuffer(_idxBuf.get(), std::as_bytes(std::span{indices}));
    }

    void ExecutePass(CommandBuffer* cmd, Fence* fence) override {
        ColorClearValue clear{{{0.0f, 0.0f, 0.0f, 1.0f}}};
        ColorAttachment colorAtt[] = {{_rtv.get(), LoadAction::Clear, StoreAction::Store, clear}};
        RenderPassDescriptor rpDesc{};
        rpDesc.ColorAttachments = colorAtt;
        rpDesc.DepthStencilAttachment = std::nullopt;
        auto encoder = cmd->BeginRenderPass(rpDesc).Unwrap();
        encoder->BindRootSignature(_rs.get());
        encoder->BindGraphicsPipelineState(_pso.get());
        if (_device->GetBackend() == RenderBackend::Vulkan) {
            encoder->SetViewport({0, static_cast<float>(kHeight), static_cast<float>(kWidth), -static_cast<float>(kHeight), 0.0f, 1.0f});
        } else {
            encoder->SetViewport({0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, 1.0f});
        }
        encoder->SetScissor({0, 0, kWidth, kHeight});
        VertexBufferView vbv[] = {{_vertBuf.get(), 0, 36}};
        encoder->BindVertexBuffer(vbv);
        encoder->BindIndexBuffer({_idxBuf.get(), 0, 2});
        encoder->DrawIndexed(3, 1, 0, 0, 0);
        cmd->EndRenderPass(std::move(encoder));
    }

    unique_ptr<RootSignature> _rs{};
    unique_ptr<GraphicsPipelineState> _pso{};
    unique_ptr<Buffer> _vertBuf{};
    unique_ptr<Buffer> _idxBuf{};
};

TEST(HelloWorldTriangle, D3D12) {
#if !defined(RADRAY_PLATFORM_WINDOWS) || !defined(RADRAY_ENABLE_D3D12)
    GTEST_SKIP() << "D3D12 is not supported on this platform or not enabled.";
#endif
    D3D12DeviceDescriptor desc{std::nullopt, true, true};
    TestTriangle test{"d3d12", desc, true, {kWidth, kHeight}, TextureFormat::RGBA8_UNORM};
    ImageData actual = test.Run();
    ASSERT_TRUE(test.GetCapturedRenderErrors().empty());
    ImageData expected = test.LoadBaseline("baseline.png");
    ASSERT_EQ(actual.Width, expected.Width);
    ASSERT_EQ(actual.Height, expected.Height);
    ASSERT_EQ(actual.Format, expected.Format);
    auto compare = ImageData::CompareImageRGBA8(actual, expected, 1);
    ASSERT_EQ(compare.MismatchCount, 0u)
        << "pixel mismatch count=" << compare.MismatchCount
        << ", first mismatch pixel=(" << (compare.FirstMismatchPixel % actual.Width) << ","
        << (compare.FirstMismatchPixel / actual.Width) << ")"
        << ", channel=" << compare.FirstMismatchChannel << ", actual=" << static_cast<uint32_t>(compare.ActualValue)
        << ", expected=" << static_cast<uint32_t>(compare.ExpectedValue)
        << ", artifacts_dir=" << test._testArtifactsDir.string();
}

TEST(HelloWorldTriangle, VK) {
#if !defined(RADRAY_ENABLE_VULKAN)
    GTEST_SKIP() << "Vulkan is not supported on this platform or not enabled.";
#endif
    render::VulkanCommandQueueDescriptor queueDesc = {render::QueueType::Direct, 1};
    render::VulkanDeviceDescriptor devDesc{};
    devDesc.Queues = std::span{&queueDesc, 1};
    TestTriangle test{"vulkan", devDesc, true, {kWidth, kHeight}, TextureFormat::RGBA8_UNORM};
    ImageData actual = test.Run();
    ASSERT_TRUE(test.GetCapturedRenderErrors().empty());
    ImageData expected = test.LoadBaseline("baseline.png");
    ASSERT_EQ(actual.Width, expected.Width);
    ASSERT_EQ(actual.Height, expected.Height);
    ASSERT_EQ(actual.Format, expected.Format);
    auto compare = ImageData::CompareImageRGBA8(actual, expected, 1);
    ASSERT_EQ(compare.MismatchCount, 0u)
        << "pixel mismatch count=" << compare.MismatchCount
        << ", first mismatch pixel=(" << (compare.FirstMismatchPixel % actual.Width) << ","
        << (compare.FirstMismatchPixel / actual.Width) << ")"
        << ", channel=" << compare.FirstMismatchChannel << ", actual=" << static_cast<uint32_t>(compare.ActualValue)
        << ", expected=" << static_cast<uint32_t>(compare.ExpectedValue)
        << ", artifacts_dir=" << test._testArtifactsDir.string();
}
