#define _CRT_SECURE_NO_WARNINGS

#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>

#include <gtest/gtest.h>

#include <radray/image_data.h>
#include <radray/render/common.h>
#include <radray/render/debug.h>
#include <radray/render/dxc.h>

using namespace radray;
using namespace radray::render;

namespace {

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

[[noreturn]] void ThrowTest(const std::string& where, const std::string& msg) {
    throw std::runtime_error(where + ": " + msg);
}

std::filesystem::path GetSourceDir() {
    if (auto env = std::getenv("RADRAY_SOURCE_DIR")) {
        return std::filesystem::path{env};
    }
    return std::filesystem::current_path();
}

std::filesystem::path GetGoldenPath() {
    return GetSourceDir() / "tests" / "test_hello_world_d3d12_offscreen" / "golden" / "triangle_gradient_rgba8_256x256.png";
}

std::filesystem::path GetArtifactsDir() {
    if (auto env = std::getenv("RADRAY_TEST_ARTIFACTS_DIR")) {
        return std::filesystem::path{env} / "hello_world_d3d12_offscreen";
    }
    return std::filesystem::current_path() / "test_artifacts" / "hello_world_d3d12_offscreen";
}

bool ShouldUpdateGolden() {
    if (auto env = std::getenv("RADRAY_UPDATE_GOLDEN")) {
        return std::string_view{env} == "1";
    }
    return false;
}

std::optional<ImageData> NormalizeToRGBA8(ImageData img) {
    if (img.Format == ImageFormat::RGBA8_BYTE) {
        return img;
    }
    if (img.Format == ImageFormat::RGB8_BYTE) {
        return img.RGB8ToRGBA8(0xFF);
    }
    return std::nullopt;
}

class GradientTrianglePass final : public DebugPass {
public:
    explicit GradientTrianglePass(DebugContext& ctx) { Init(ctx); }

    void Record(DebugContext&, CommandBuffer* cmd, DebugOffscreenTarget& target) override {
        ColorClearValue clear{{{0.0f, 0.0f, 0.0f, 1.0f}}};
        ColorAttachment colorAtt[] = {{target.RTV.get(), LoadAction::Clear, StoreAction::Store, clear}};
        RenderPassDescriptor rpDesc{};
        rpDesc.ColorAttachments = colorAtt;
        rpDesc.DepthStencilAttachment = std::nullopt;
        auto rp = cmd->BeginRenderPass(rpDesc);
        if (!rp.HasValue()) {
            ThrowTest("GradientTrianglePass::Record", "BeginRenderPass failed");
        }
        auto encoder = rp.Release();
        encoder->BindRootSignature(_rs.get());
        encoder->BindGraphicsPipelineState(_pso.get());
        encoder->SetViewport({0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, 1.0f});
        encoder->SetScissor({0, 0, kWidth, kHeight});
        VertexBufferView vbv[] = {{_vertBuf.get(), 0, _vertexSize}};
        encoder->BindVertexBuffer(vbv);
        encoder->BindIndexBuffer({_idxBuf.get(), 0, 2});
        encoder->DrawIndexed(3, 1, 0, 0, 0);
        cmd->EndRenderPass(std::move(encoder));
    }

private:
    void Init(DebugContext& ctx) {
        Device* device = ctx.GetDevice();
        CommandQueue* queue = ctx.GetQueue();
        if (device == nullptr || queue == nullptr) {
            ThrowTest("GradientTrianglePass::Init", "invalid debug context");
        }
        Dxc* dxc = ctx.GetDxc();
        if (dxc == nullptr) {
            ThrowTest("GradientTrianglePass::Init", "DXC is unavailable");
        }

        auto vsOut = dxc->Compile(kShaderSrc, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, false);
        auto psOut = dxc->Compile(kShaderSrc, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, false);
        if (!vsOut.has_value() || !psOut.has_value()) {
            ThrowTest("GradientTrianglePass::Init", "shader compile failed");
        }

        auto vs = device->CreateShader({vsOut->Data, vsOut->Category});
        auto ps = device->CreateShader({psOut->Data, psOut->Category});
        if (!vs.HasValue() || !ps.HasValue()) {
            ThrowTest("GradientTrianglePass::Init", "CreateShader failed");
        }
        _vsShader = vs.Release();
        _psShader = ps.Release();

        RootSignatureDescriptor rsDesc{};
        auto rsOpt = device->CreateRootSignature(rsDesc);
        if (!rsOpt.HasValue()) {
            ThrowTest("GradientTrianglePass::Init", "CreateRootSignature failed");
        }
        _rs = rsOpt.Release();

        VertexElement elems[] = {
            {0, "POSITION", 0, VertexFormat::FLOAT32X3, 0}};
        VertexBufferLayout layout{12, VertexStepMode::Vertex, elems};
        ColorTargetState colorTarget = ColorTargetState::Default(TextureFormat::RGBA8_UNORM);
        GraphicsPipelineStateDescriptor psoDesc{};
        psoDesc.RootSig = _rs.get();
        psoDesc.VS = ShaderEntry{_vsShader.get(), "VSMain"};
        psoDesc.PS = ShaderEntry{_psShader.get(), "PSMain"};
        psoDesc.VertexLayouts = std::span{&layout, 1};
        psoDesc.Primitive = PrimitiveState::Default();
        psoDesc.DepthStencil = std::nullopt;
        psoDesc.MultiSample = MultiSampleState::Default();
        psoDesc.ColorTargets = std::span{&colorTarget, 1};
        auto psoOpt = device->CreateGraphicsPipelineState(psoDesc);
        if (!psoOpt.HasValue()) {
            ThrowTest("GradientTrianglePass::Init", "CreateGraphicsPipelineState failed");
        }
        _pso = psoOpt.Release();

        constexpr float vertices[] = {
            0.0f, 0.5f, 0.0f,
            -0.5f, -0.366f, 0.0f,
            0.5f, -0.366f, 0.0f};
        constexpr uint16_t indices[] = {0, 2, 1};
        _vertexSize = sizeof(vertices);
        _indexSize = sizeof(indices);

        auto vertUploadOpt = device->CreateBuffer({_vertexSize, MemoryType::Upload, BufferUse::CopySource | BufferUse::MapWrite, ResourceHint::None, "vb_upload"});
        auto idxUploadOpt = device->CreateBuffer({_indexSize, MemoryType::Upload, BufferUse::CopySource | BufferUse::MapWrite, ResourceHint::None, "ib_upload"});
        auto vertBufOpt = device->CreateBuffer({_vertexSize, MemoryType::Device, BufferUse::CopyDestination | BufferUse::Vertex, ResourceHint::None, "vb"});
        auto idxBufOpt = device->CreateBuffer({_indexSize, MemoryType::Device, BufferUse::CopyDestination | BufferUse::Index, ResourceHint::None, "ib"});
        if (!vertUploadOpt.HasValue() || !idxUploadOpt.HasValue() || !vertBufOpt.HasValue() || !idxBufOpt.HasValue()) {
            ThrowTest("GradientTrianglePass::Init", "CreateBuffer failed");
        }
        _vertUpload = vertUploadOpt.Release();
        _idxUpload = idxUploadOpt.Release();
        _vertBuf = vertBufOpt.Release();
        _idxBuf = idxBufOpt.Release();

        {
            void* ptr = _vertUpload->Map(0, _vertexSize);
            if (ptr == nullptr) {
                ThrowTest("GradientTrianglePass::Init", "vertex upload map failed");
            }
            std::memcpy(ptr, vertices, static_cast<size_t>(_vertexSize));
            _vertUpload->Unmap(0, _vertexSize);
        }
        {
            void* ptr = _idxUpload->Map(0, _indexSize);
            if (ptr == nullptr) {
                ThrowTest("GradientTrianglePass::Init", "index upload map failed");
            }
            std::memcpy(ptr, indices, static_cast<size_t>(_indexSize));
            _idxUpload->Unmap(0, _indexSize);
        }

        auto uploadCmdOpt = device->CreateCommandBuffer(queue);
        auto uploadFenceOpt = device->CreateFence();
        if (!uploadCmdOpt.HasValue() || !uploadFenceOpt.HasValue()) {
            ThrowTest("GradientTrianglePass::Init", "upload command setup failed");
        }
        auto uploadCmd = uploadCmdOpt.Release();
        auto uploadFence = uploadFenceOpt.Release();
        uploadCmd->Begin();
        {
            ResourceBarrierDescriptor b[] = {
                BarrierBufferDescriptor{_vertBuf.get(), BufferState::Common, BufferState::CopyDestination, nullptr, false},
                BarrierBufferDescriptor{_idxBuf.get(), BufferState::Common, BufferState::CopyDestination, nullptr, false}};
            uploadCmd->ResourceBarrier(b);
        }
        uploadCmd->CopyBufferToBuffer(_vertBuf.get(), 0, _vertUpload.get(), 0, _vertexSize);
        uploadCmd->CopyBufferToBuffer(_idxBuf.get(), 0, _idxUpload.get(), 0, _indexSize);
        {
            ResourceBarrierDescriptor b[] = {
                BarrierBufferDescriptor{_vertBuf.get(), BufferState::CopyDestination, BufferState::Vertex, nullptr, false},
                BarrierBufferDescriptor{_idxBuf.get(), BufferState::CopyDestination, BufferState::Index, nullptr, false}};
            uploadCmd->ResourceBarrier(b);
        }
        uploadCmd->End();

        CommandBuffer* submitBuffers[] = {uploadCmd.get()};
        CommandQueueSubmitDescriptor submitDesc{};
        submitDesc.CmdBuffers = submitBuffers;
        submitDesc.SignalFence = uploadFence.get();
        queue->Submit(submitDesc);
        uploadFence->Wait();
    }

private:
    unique_ptr<Shader> _vsShader{};
    unique_ptr<Shader> _psShader{};
    unique_ptr<RootSignature> _rs{};
    unique_ptr<GraphicsPipelineState> _pso{};
    unique_ptr<Buffer> _vertUpload{};
    unique_ptr<Buffer> _idxUpload{};
    unique_ptr<Buffer> _vertBuf{};
    unique_ptr<Buffer> _idxBuf{};
    uint64_t _vertexSize{0};
    uint64_t _indexSize{0};
};

DebugContext CreateTestContext() {
    D3D12DeviceDescriptor deviceDesc{};
    deviceDesc.IsEnableDebugLayer = false;
    deviceDesc.IsEnableGpuBasedValid = false;

    DebugContextDescriptor ctxDesc{};
    ctxDesc.DeviceDesc = deviceDesc;
    ctxDesc.Queue = QueueType::Direct;
    ctxDesc.QueueIndex = 0;
    ctxDesc.CreateDxc = true;
    return DebugContext::Create(ctxDesc);
}

ImageData RenderGradientTriangle(DebugContext& ctx) {
    auto target = ctx.CreateOffscreenTarget({kWidth, kHeight, TextureFormat::RGBA8_UNORM, "offscreen_rt"});
    GradientTrianglePass pass{ctx};
    ctx.ExecutePass(target, pass, TextureState::Undefined, TextureState::CopySource);
    return ctx.ReadbackRGBA8(target, TextureState::CopySource, 0, 0);
}

}  // namespace

TEST(D3D12Offscreen, GradientTriangleMatchesGoldenPng) {
    std::optional<DebugContext> ctx;
    ImageData actual{};
    try {
        ctx.emplace(CreateTestContext());
        actual = RenderGradientTriangle(*ctx);
    } catch (const std::exception& e) {
        GTEST_SKIP() << "D3D12 offscreen render setup unavailable on this machine: " << e.what();
    }

    ASSERT_EQ(actual.Format, ImageFormat::RGBA8_BYTE);

    const auto goldenPath = GetGoldenPath();
    if (ShouldUpdateGolden()) {
        std::error_code ec;
        std::filesystem::create_directories(goldenPath.parent_path(), ec);
        ASSERT_NO_THROW(ctx->WritePNG(actual, {(goldenPath).string(), false}));
    }

    std::ifstream file{goldenPath, std::ios::binary};
    ASSERT_TRUE(file.is_open()) << "golden png not found: " << goldenPath.string();
    auto goldenLoad = ImageData::LoadPNG(file, PNGLoadSettings{.AddAlphaIfRGB = 0xFF, .IsFlipY = false});
    ASSERT_TRUE(goldenLoad.has_value()) << "failed to load golden png: " << goldenPath.string();
    auto goldenNorm = NormalizeToRGBA8(std::move(*goldenLoad));
    ASSERT_TRUE(goldenNorm.has_value());
    auto expected = std::move(*goldenNorm);

    ASSERT_EQ(actual.Width, expected.Width);
    ASSERT_EQ(actual.Height, expected.Height);
    ASSERT_EQ(actual.Format, expected.Format);

    const PixelCompareResult compare = CompareImageRGBA8(actual, expected, 1);
    if (!compare.IsMatch()) {
        auto artifactsDir = GetArtifactsDir();
        ASSERT_NO_THROW(WriteImageComparisonArtifacts(*ctx, actual, expected, artifactsDir.string()));
    }

    ASSERT_EQ(compare.MismatchCount, 0u)
        << "pixel mismatch count=" << compare.MismatchCount
        << ", first mismatch pixel=(" << (compare.FirstMismatchPixel % actual.Width) << ","
        << (compare.FirstMismatchPixel / actual.Width) << ")"
        << ", channel=" << compare.FirstMismatchChannel << ", actual=" << static_cast<uint32_t>(compare.ActualValue)
        << ", expected=" << static_cast<uint32_t>(compare.ExpectedValue)
        << ", artifacts_dir=" << GetArtifactsDir().string();
}
