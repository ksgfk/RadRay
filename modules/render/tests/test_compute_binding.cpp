#include "render_test_framework.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <utility>

#include <gtest/gtest.h>
#include <fmt/format.h>

namespace radray::render::test {
namespace {

constexpr std::string_view kMultiSetBufferShader = R"(
struct PushData {
    uint Addend;
    uint _Pad0;
    uint _Pad1;
    uint _Pad2;
};

struct ScaleData {
    uint Scale;
    uint _Pad0;
    uint _Pad1;
    uint _Pad2;
};

[[vk::push_constant]] ConstantBuffer<PushData> gPush : register(b0, space0);
[[vk::binding(1, 0)]] ConstantBuffer<ScaleData> gScale : register(b1, space0);
[[vk::binding(0, 1)]] RWStructuredBuffer<uint> gOut : register(u0, space1);

[numthreads(1, 1, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    const uint index = tid.x;
    gOut[index] = index * gScale.Scale + gPush.Addend;
}
)";

constexpr std::string_view kTextureSamplerShader = R"(
[[vk::binding(0, 0)]] Texture2D<float4> gTex : register(t0, space0);
[[vk::binding(1, 0)]] SamplerState gSamp : register(s1, space0);
[[vk::binding(0, 1)]] RWStructuredBuffer<uint> gOut : register(u0, space1);

[numthreads(1, 1, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    const float4 color = gTex.SampleLevel(gSamp, float2(0.25, 0.25), 0.0);
    const uint r = (uint)round(saturate(color.x) * 255.0);
    const uint g = (uint)round(saturate(color.y) * 255.0);
    const uint b = (uint)round(saturate(color.z) * 255.0);
    const uint a = (uint)round(saturate(color.w) * 255.0);
    gOut[tid.x] = r | (g << 8) | (b << 16) | (a << 24);
}
)";

constexpr std::string_view kBindlessBufferShader = R"(
struct SelectData {
    uint Slot;
    uint _Pad0;
    uint _Pad1;
    uint _Pad2;
};

[[vk::binding(0, 0)]] StructuredBuffer<uint> gInputs[] : register(t0, space0);
[[vk::binding(0, 1)]] ConstantBuffer<SelectData> gSelect : register(b0, space1);
[[vk::binding(0, 2)]] RWStructuredBuffer<uint> gOut : register(u0, space2);

[numthreads(1, 1, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    gOut[0] = gInputs[gSelect.Slot][0];
}
)";

constexpr std::string_view kBindlessTextureShader = R"(
struct SampleData {
    uint Slot;
    float U;
    float V;
    uint _Pad0;
};

[[vk::binding(0, 0)]] Texture2D<float4> gTextures[] : register(t0, space0);
[[vk::binding(0, 1)]] ConstantBuffer<SampleData> gSample : register(b0, space1);
[[vk::binding(1, 1)]] SamplerState gSamp : register(s1, space1);
[[vk::binding(0, 2)]] RWStructuredBuffer<uint> gOut : register(u0, space2);

[numthreads(1, 1, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    const float4 color = gTextures[gSample.Slot].SampleLevel(gSamp, float2(gSample.U, gSample.V), 0.0);
    const uint r = (uint)round(saturate(color.x) * 255.0);
    const uint g = (uint)round(saturate(color.y) * 255.0);
    const uint b = (uint)round(saturate(color.z) * 255.0);
    const uint a = (uint)round(saturate(color.w) * 255.0);
    gOut[0] = r | (g << 8) | (b << 16) | (a << 24);
}
)";

struct PushDataCpu {
    uint32_t Addend{0};
    uint32_t Pad0{0};
    uint32_t Pad1{0};
    uint32_t Pad2{0};
};

struct ScaleDataCpu {
    uint32_t Scale{0};
    uint32_t Pad0{0};
    uint32_t Pad1{0};
    uint32_t Pad2{0};
};

struct SelectDataCpu {
    uint32_t Slot{0};
    uint32_t Pad0{0};
    uint32_t Pad1{0};
    uint32_t Pad2{0};
};

struct SampleDataCpu {
    uint32_t Slot{0};
    float U{0.0f};
    float V{0.0f};
    uint32_t Pad0{0};
};

static_assert(sizeof(PushDataCpu) == 16);
static_assert(sizeof(ScaleDataCpu) == 16);
static_assert(sizeof(SelectDataCpu) == 16);
static_assert(sizeof(SampleDataCpu) == 16);

class ComputeBindingRuntimeTest : public ::testing::TestWithParam<TestBackend> {
protected:
    void SetUp() override {
        string reason{};
        if (!_ctx.Initialize(this->GetParam(), &reason)) {
            GTEST_SKIP() << reason;
        }
    }

    unique_ptr<Buffer> CreateBufferOrNull(const BufferDescriptor& desc, std::string_view label) {
        string reason{};
        auto opt = _ctx.CreateBuffer(desc, &reason);
        if (!opt.HasValue()) {
            ADD_FAILURE() << fmt::format("{} failed on {}: {}", label, _ctx.GetBackendName(), reason);
            return nullptr;
        }
        return opt.Release();
    }

    unique_ptr<BufferView> CreateBufferViewOrNull(const BufferViewDescriptor& desc, std::string_view label) {
        string reason{};
        auto opt = _ctx.CreateBufferView(desc, &reason);
        if (!opt.HasValue()) {
            ADD_FAILURE() << fmt::format("{} failed on {}: {}", label, _ctx.GetBackendName(), reason);
            return nullptr;
        }
        return opt.Release();
    }

    unique_ptr<Texture> CreateTextureOrNull(const TextureDescriptor& desc, std::string_view label) {
        string reason{};
        auto opt = _ctx.CreateTexture(desc, &reason);
        if (!opt.HasValue()) {
            ADD_FAILURE() << fmt::format("{} failed on {}: {}", label, _ctx.GetBackendName(), reason);
            return nullptr;
        }
        return opt.Release();
    }

    unique_ptr<TextureView> CreateTextureViewOrNull(const TextureViewDescriptor& desc, std::string_view label) {
        string reason{};
        auto opt = _ctx.CreateTextureView(desc, &reason);
        if (!opt.HasValue()) {
            ADD_FAILURE() << fmt::format("{} failed on {}: {}", label, _ctx.GetBackendName(), reason);
            return nullptr;
        }
        return opt.Release();
    }

    unique_ptr<Sampler> CreateSamplerOrNull(const SamplerDescriptor& desc, std::string_view label) {
        string reason{};
        auto opt = _ctx.CreateSampler(desc, &reason);
        if (!opt.HasValue()) {
            ADD_FAILURE() << fmt::format("{} failed on {}: {}", label, _ctx.GetBackendName(), reason);
            return nullptr;
        }
        return opt.Release();
    }

    unique_ptr<DescriptorSet> CreateDescriptorSetOrNull(RootSignature* rootSig, DescriptorSetIndex setIndex) {
        string reason{};
        auto opt = _ctx.CreateDescriptorSet(rootSig, setIndex, &reason);
        if (!opt.HasValue()) {
            ADD_FAILURE() << fmt::format(
                "CreateDescriptorSet(set={}) failed on {}: {}",
                setIndex.Value,
                _ctx.GetBackendName(),
                reason);
            return nullptr;
        }
        return opt.Release();
    }

    unique_ptr<BindlessArray> CreateBindlessArrayOrNull(const BindlessArrayDescriptor& desc) {
        string reason{};
        auto opt = _ctx.CreateBindlessArray(desc, &reason);
        if (!opt.HasValue()) {
            ADD_FAILURE() << fmt::format(
                "CreateBindlessArray(size={}, slotType={}) failed on {}: {}",
                desc.Size,
                static_cast<uint32_t>(desc.SlotType),
                _ctx.GetBackendName(),
                reason);
            return nullptr;
        }
        return opt.Release();
    }

    template <typename T>
    vector<T> ReadBufferVectorOrEmpty(Buffer* buffer, size_t count) {
        string reason{};
        auto bytesOpt = _ctx.ReadHostVisibleBuffer(buffer, sizeof(T) * count, &reason);
        if (!bytesOpt.has_value()) {
            ADD_FAILURE() << fmt::format(
                "ReadHostVisibleBuffer failed on {}: {}",
                _ctx.GetBackendName(),
                reason);
            return {};
        }
        vector<T> result(count);
        std::memcpy(result.data(), bytesOpt->data(), sizeof(T) * count);
        return result;
    }

    uint64_t GetCBufferSize(uint64_t payloadSize) const {
        const uint64_t alignment = std::max<uint64_t>(16, _ctx.GetDeviceDetail().CBufferAlignment);
        return AlignUp(payloadSize, alignment);
    }

    void ExpectNoCapturedErrors() {
        EXPECT_TRUE(_ctx.GetCapturedErrors().empty())
            << fmt::format("Captured errors on {}:\n{}", _ctx.GetBackendName(), _ctx.JoinCapturedErrors());
    }

    ComputeTestContext _ctx{};
};

TEST_P(ComputeBindingRuntimeTest, MultiSetBufferBindingAndPushConstantsWorks) {
    string reason{};
    auto programOpt = _ctx.CreateComputeProgram(kMultiSetBufferShader, "CSMain", false, &reason);
    ASSERT_TRUE(programOpt.has_value())
        << fmt::format("CreateComputeProgram failed on {}: {}\n{}", _ctx.GetBackendName(), reason, _ctx.JoinCapturedErrors());
    auto program = std::move(programOpt.value());

    constexpr size_t kElementCount = 8;
    constexpr uint32_t kScale = 3;
    constexpr uint32_t kAddend = 5;

    const uint64_t cbufferSize = this->GetCBufferSize(sizeof(ScaleDataCpu));
    const uint64_t outputSize = sizeof(uint32_t) * kElementCount;

    BufferDescriptor scaleBufferDesc{};
    scaleBufferDesc.Size = cbufferSize;
    scaleBufferDesc.Memory = MemoryType::Device;
    scaleBufferDesc.Usage = BufferUse::CopyDestination | BufferUse::CBuffer;
    auto scaleBuffer = this->CreateBufferOrNull(scaleBufferDesc, "Create scale buffer");
    ASSERT_NE(scaleBuffer, nullptr);

    BufferDescriptor outputBufferDesc{};
    outputBufferDesc.Size = outputSize;
    outputBufferDesc.Memory = MemoryType::Device;
    outputBufferDesc.Usage = BufferUse::CopySource | BufferUse::UnorderedAccess;
    auto outputBuffer = this->CreateBufferOrNull(outputBufferDesc, "Create output buffer");
    ASSERT_NE(outputBuffer, nullptr);

    BufferDescriptor readbackBufferDesc{};
    readbackBufferDesc.Size = outputSize;
    readbackBufferDesc.Memory = MemoryType::ReadBack;
    readbackBufferDesc.Usage = BufferUse::CopyDestination | BufferUse::MapRead;
    auto readbackBuffer = this->CreateBufferOrNull(readbackBufferDesc, "Create readback buffer");
    ASSERT_NE(readbackBuffer, nullptr);

    ScaleDataCpu scaleData{.Scale = kScale};
    vector<byte> paddedScale(cbufferSize, byte{0});
    std::memcpy(paddedScale.data(), &scaleData, sizeof(scaleData));
    ASSERT_TRUE(_ctx.UploadBufferData(scaleBuffer.get(), paddedScale, BufferState::CBuffer, &reason))
        << fmt::format("Upload scale buffer failed on {}: {}", _ctx.GetBackendName(), reason);

    BufferViewDescriptor scaleViewDesc{};
    scaleViewDesc.Target = scaleBuffer.get();
    scaleViewDesc.Range = BufferRange{0, cbufferSize};
    scaleViewDesc.Usage = BufferViewUsage::CBuffer;
    auto scaleView = this->CreateBufferViewOrNull(scaleViewDesc, "Create scale CBV");
    ASSERT_NE(scaleView, nullptr);

    BufferViewDescriptor outputViewDesc{};
    outputViewDesc.Target = outputBuffer.get();
    outputViewDesc.Range = BufferRange{0, outputSize};
    outputViewDesc.Stride = sizeof(uint32_t);
    outputViewDesc.Usage = BufferViewUsage::ReadWriteStorage;
    auto outputView = this->CreateBufferViewOrNull(outputViewDesc, "Create output UAV");
    ASSERT_NE(outputView, nullptr);

    auto set0 = this->CreateDescriptorSetOrNull(program.RootSignatureObject.get(), DescriptorSetIndex{0});
    ASSERT_NE(set0, nullptr);
    ASSERT_TRUE(set0->WriteResource("gScale", scaleView.get()));

    auto set1 = this->CreateDescriptorSetOrNull(program.RootSignatureObject.get(), DescriptorSetIndex{1});
    ASSERT_NE(set1, nullptr);
    ASSERT_TRUE(set1->WriteResource("gOut", outputView.get()));

    auto pushIdOpt = program.RootSignatureObject->FindParameterId("gPush");
    ASSERT_TRUE(pushIdOpt.has_value());
    PushDataCpu pushData{.Addend = kAddend};

    auto cmdOpt = _ctx.CreateCommandBuffer(&reason);
    ASSERT_TRUE(cmdOpt.HasValue())
        << fmt::format("CreateCommandBuffer failed on {}: {}", _ctx.GetBackendName(), reason);
    auto cmd = cmdOpt.Release();

    _ctx.ClearCapturedErrors();
    cmd->Begin();
    vector<ResourceBarrierDescriptor> preDispatch{};
    preDispatch.push_back(BarrierBufferDescriptor{
        .Target = outputBuffer.get(),
        .Before = BufferState::Common,
        .After = BufferState::UnorderedAccess,
    });
    cmd->ResourceBarrier(preDispatch);

    auto encoderOpt = cmd->BeginComputePass();
    ASSERT_TRUE(encoderOpt.HasValue())
        << fmt::format("BeginComputePass failed on {}", _ctx.GetBackendName());
    auto encoder = encoderOpt.Release();
    encoder->BindRootSignature(program.RootSignatureObject.get());
    encoder->BindComputePipelineState(program.PipelineObject.get());
    encoder->BindDescriptorSet(DescriptorSetIndex{0}, set0.get());
    encoder->BindDescriptorSet(DescriptorSetIndex{1}, set1.get());
    encoder->PushConstants(pushIdOpt.value(), &pushData, sizeof(pushData));
    encoder->Dispatch(static_cast<uint32_t>(kElementCount), 1, 1);
    cmd->EndComputePass(std::move(encoder));

    vector<ResourceBarrierDescriptor> preCopy{};
    preCopy.push_back(BarrierBufferDescriptor{
        .Target = outputBuffer.get(),
        .Before = BufferState::UnorderedAccess,
        .After = BufferState::CopySource,
    });
    _ctx.AppendReadbackPreCopyBarrier(preCopy, readbackBuffer.get());
    cmd->ResourceBarrier(preCopy);
    cmd->CopyBufferToBuffer(readbackBuffer.get(), 0, outputBuffer.get(), 0, outputSize);
    vector<ResourceBarrierDescriptor> postCopy{};
    _ctx.AppendReadbackPostCopyBarrier(postCopy, readbackBuffer.get());
    if (!postCopy.empty()) {
        cmd->ResourceBarrier(postCopy);
    }
    cmd->End();

    ASSERT_TRUE(_ctx.SubmitAndWait(cmd.get(), &reason))
        << fmt::format("SubmitAndWait failed on {}: {}", _ctx.GetBackendName(), reason);

    vector<uint32_t> actual = this->ReadBufferVectorOrEmpty<uint32_t>(readbackBuffer.get(), kElementCount);
    ASSERT_EQ(actual.size(), kElementCount);
    vector<uint32_t> expected{};
    expected.reserve(kElementCount);
    for (uint32_t index = 0; index < kElementCount; ++index) {
        expected.push_back(index * kScale + kAddend);
    }

    EXPECT_EQ(actual, expected)
        << fmt::format("Unexpected compute output on {}", _ctx.GetBackendName());
    this->ExpectNoCapturedErrors();
}

TEST_P(ComputeBindingRuntimeTest, TextureAndSamplerBindingWorks) {
    string reason{};
    auto programOpt = _ctx.CreateComputeProgram(kTextureSamplerShader, "CSMain", false, &reason);
    ASSERT_TRUE(programOpt.has_value())
        << fmt::format("CreateComputeProgram failed on {}: {}\n{}", _ctx.GetBackendName(), reason, _ctx.JoinCapturedErrors());
    auto program = std::move(programOpt.value());

    TextureDescriptor textureDesc{};
    textureDesc.Dim = TextureDimension::Dim2D;
    textureDesc.Width = 2;
    textureDesc.Height = 2;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.SampleCount = 1;
    textureDesc.Format = TextureFormat::RGBA8_UNORM;
    textureDesc.Memory = MemoryType::Device;
    textureDesc.Usage = TextureUse::CopyDestination | TextureUse::Resource;
    auto texture = this->CreateTextureOrNull(textureDesc, "Create test texture");
    ASSERT_NE(texture, nullptr);
    //clang-format off
    std::array<byte, 16> texels = {
        byte{0x11}, byte{0x22}, byte{0x33}, byte{0xFF},
        byte{0x99}, byte{0x88}, byte{0x77}, byte{0xFF},
        byte{0x44}, byte{0x55}, byte{0x66}, byte{0xFF},
        byte{0xAA}, byte{0xBB}, byte{0xCC}, byte{0xFF}};
    //clang-format on
    ASSERT_TRUE(_ctx.UploadTexture2D(texture.get(), texels, &reason))
        << fmt::format("UploadTexture2D failed on {}: {}", _ctx.GetBackendName(), reason);

    TextureViewDescriptor textureViewDesc{};
    textureViewDesc.Target = texture.get();
    textureViewDesc.Dim = TextureDimension::Dim2D;
    textureViewDesc.Format = TextureFormat::RGBA8_UNORM;
    textureViewDesc.Range = SubresourceRange{0, 1, 0, 1};
    textureViewDesc.Usage = TextureViewUsage::Resource;
    auto textureView = this->CreateTextureViewOrNull(textureViewDesc, "Create texture SRV");
    ASSERT_NE(textureView, nullptr);

    SamplerDescriptor samplerDesc{};
    samplerDesc.AddressS = AddressMode::ClampToEdge;
    samplerDesc.AddressT = AddressMode::ClampToEdge;
    samplerDesc.AddressR = AddressMode::ClampToEdge;
    samplerDesc.MinFilter = FilterMode::Nearest;
    samplerDesc.MagFilter = FilterMode::Nearest;
    samplerDesc.MipmapFilter = FilterMode::Nearest;
    samplerDesc.LodMin = 0.0f;
    samplerDesc.LodMax = 0.0f;
    samplerDesc.Compare = std::nullopt;
    samplerDesc.AnisotropyClamp = 1;
    auto sampler = this->CreateSamplerOrNull(samplerDesc, "Create sampler");
    ASSERT_NE(sampler, nullptr);

    constexpr uint64_t kOutputSize = sizeof(uint32_t);
    BufferDescriptor outputBufferDesc{};
    outputBufferDesc.Size = kOutputSize;
    outputBufferDesc.Memory = MemoryType::Device;
    outputBufferDesc.Usage = BufferUse::CopySource | BufferUse::UnorderedAccess;
    auto outputBuffer = this->CreateBufferOrNull(outputBufferDesc, "Create output buffer");
    ASSERT_NE(outputBuffer, nullptr);

    BufferDescriptor readbackBufferDesc{};
    readbackBufferDesc.Size = kOutputSize;
    readbackBufferDesc.Memory = MemoryType::ReadBack;
    readbackBufferDesc.Usage = BufferUse::MapRead | BufferUse::CopyDestination;
    auto readbackBuffer = this->CreateBufferOrNull(readbackBufferDesc, "Create readback buffer");
    ASSERT_NE(readbackBuffer, nullptr);

    BufferViewDescriptor outputViewDesc{};
    outputViewDesc.Target = outputBuffer.get();
    outputViewDesc.Range = BufferRange{0, kOutputSize};
    outputViewDesc.Stride = sizeof(uint32_t);
    outputViewDesc.Usage = BufferViewUsage::ReadWriteStorage;
    auto outputView = this->CreateBufferViewOrNull(outputViewDesc, "Create output UAV");
    ASSERT_NE(outputView, nullptr);

    auto set0 = this->CreateDescriptorSetOrNull(program.RootSignatureObject.get(), DescriptorSetIndex{0});
    ASSERT_NE(set0, nullptr);
    ASSERT_TRUE(set0->WriteResource("gTex", textureView.get()));
    ASSERT_TRUE(set0->WriteSampler("gSamp", sampler.get()));

    auto set1 = this->CreateDescriptorSetOrNull(program.RootSignatureObject.get(), DescriptorSetIndex{1});
    ASSERT_NE(set1, nullptr);
    ASSERT_TRUE(set1->WriteResource("gOut", outputView.get()));

    auto cmdOpt = _ctx.CreateCommandBuffer(&reason);
    ASSERT_TRUE(cmdOpt.HasValue())
        << fmt::format("CreateCommandBuffer failed on {}: {}", _ctx.GetBackendName(), reason);
    auto cmd = cmdOpt.Release();

    _ctx.ClearCapturedErrors();
    cmd->Begin();
    ResourceBarrierDescriptor toUav = BarrierBufferDescriptor{
        .Target = outputBuffer.get(),
        .Before = BufferState::Common,
        .After = BufferState::UnorderedAccess,
    };
    cmd->ResourceBarrier(std::span{&toUav, 1});

    auto encoderOpt = cmd->BeginComputePass();
    ASSERT_TRUE(encoderOpt.HasValue())
        << fmt::format("BeginComputePass failed on {}", _ctx.GetBackendName());
    auto encoder = encoderOpt.Release();
    encoder->BindRootSignature(program.RootSignatureObject.get());
    encoder->BindComputePipelineState(program.PipelineObject.get());
    encoder->BindDescriptorSet(DescriptorSetIndex{0}, set0.get());
    encoder->BindDescriptorSet(DescriptorSetIndex{1}, set1.get());
    encoder->Dispatch(1, 1, 1);
    cmd->EndComputePass(std::move(encoder));

    vector<ResourceBarrierDescriptor> preCopy{};
    preCopy.push_back(BarrierBufferDescriptor{
        .Target = outputBuffer.get(),
        .Before = BufferState::UnorderedAccess,
        .After = BufferState::CopySource,
    });
    _ctx.AppendReadbackPreCopyBarrier(preCopy, readbackBuffer.get());
    cmd->ResourceBarrier(preCopy);
    cmd->CopyBufferToBuffer(readbackBuffer.get(), 0, outputBuffer.get(), 0, kOutputSize);
    vector<ResourceBarrierDescriptor> postCopy{};
    _ctx.AppendReadbackPostCopyBarrier(postCopy, readbackBuffer.get());
    if (!postCopy.empty()) {
        cmd->ResourceBarrier(postCopy);
    }
    cmd->End();

    ASSERT_TRUE(_ctx.SubmitAndWait(cmd.get(), &reason))
        << fmt::format("SubmitAndWait failed on {}: {}", _ctx.GetBackendName(), reason);

    vector<uint32_t> actual = this->ReadBufferVectorOrEmpty<uint32_t>(readbackBuffer.get(), 1);
    ASSERT_EQ(actual.size(), 1u);
    constexpr uint32_t kExpectedPacked = 0xFF332211u;
    EXPECT_EQ(actual[0], kExpectedPacked)
        << fmt::format("Unexpected sampled texel on {}", _ctx.GetBackendName());
    this->ExpectNoCapturedErrors();
}

TEST_P(ComputeBindingRuntimeTest, BindlessBufferBindingWorks) {
    if (!_ctx.GetDeviceDetail().IsBindlessArraySupported) {
        GTEST_SKIP() << fmt::format("Bindless arrays are not supported on {}", _ctx.GetBackendName());
    }

    string reason{};
    auto programOpt = _ctx.CreateComputeProgram(kBindlessBufferShader, "CSMain", true, &reason);
    ASSERT_TRUE(programOpt.has_value())
        << fmt::format("CreateComputeProgram failed on {}: {}\n{}", _ctx.GetBackendName(), reason, _ctx.JoinCapturedErrors());
    auto program = std::move(programOpt.value());

    constexpr uint32_t kValue0 = 17;
    constexpr uint32_t kValue1 = 29;
    const uint64_t cbufferSize = this->GetCBufferSize(sizeof(SelectDataCpu));

    auto makeInputBuffer = [&](uint32_t value, std::string_view label) -> std::pair<unique_ptr<Buffer>, unique_ptr<BufferView>> {
        BufferDescriptor bufferDesc{};
        bufferDesc.Size = sizeof(uint32_t);
        bufferDesc.Memory = MemoryType::Device;
        bufferDesc.Usage = BufferUse::CopyDestination | BufferUse::Resource;
        auto buffer = this->CreateBufferOrNull(bufferDesc, fmt::format("Create {}", label));
        EXPECT_NE(buffer, nullptr);
        if (buffer == nullptr) {
            return {};
        }

        vector<byte> bytes(sizeof(uint32_t), byte{0});
        std::memcpy(bytes.data(), &value, sizeof(value));
        EXPECT_TRUE(_ctx.UploadBufferData(buffer.get(), bytes, BufferState::ShaderRead, &reason))
            << fmt::format("Upload {} failed on {}: {}", label, _ctx.GetBackendName(), reason);

        BufferViewDescriptor viewDesc{};
        viewDesc.Target = buffer.get();
        viewDesc.Range = BufferRange{0, sizeof(uint32_t)};
        viewDesc.Stride = sizeof(uint32_t);
        viewDesc.Usage = BufferViewUsage::ReadOnlyStorage;
        auto view = this->CreateBufferViewOrNull(viewDesc, fmt::format("Create {}", label));
        EXPECT_NE(view, nullptr);
        return {std::move(buffer), std::move(view)};
    };

    auto [inputBuffer0, inputView0] = makeInputBuffer(kValue0, "input view 0");
    ASSERT_NE(inputBuffer0, nullptr);
    ASSERT_NE(inputView0, nullptr);
    auto [inputBuffer1, inputView1] = makeInputBuffer(kValue1, "input view 1");
    ASSERT_NE(inputBuffer1, nullptr);
    ASSERT_NE(inputView1, nullptr);

    BufferDescriptor selectBufferDesc{};
    selectBufferDesc.Size = cbufferSize;
    selectBufferDesc.Memory = MemoryType::Device;
    selectBufferDesc.Usage = BufferUse::CopyDestination | BufferUse::CBuffer;
    auto selectBuffer = this->CreateBufferOrNull(selectBufferDesc, "Create select buffer");
    ASSERT_NE(selectBuffer, nullptr);

    SelectDataCpu selectData{.Slot = 1};
    vector<byte> paddedSelect(cbufferSize, byte{0});
    std::memcpy(paddedSelect.data(), &selectData, sizeof(selectData));
    ASSERT_TRUE(_ctx.UploadBufferData(selectBuffer.get(), paddedSelect, BufferState::CBuffer, &reason))
        << fmt::format("Upload select buffer failed on {}: {}", _ctx.GetBackendName(), reason);

    BufferViewDescriptor selectViewDesc{};
    selectViewDesc.Target = selectBuffer.get();
    selectViewDesc.Range = BufferRange{0, cbufferSize};
    selectViewDesc.Usage = BufferViewUsage::CBuffer;
    auto selectView = this->CreateBufferViewOrNull(selectViewDesc, "Create select CBV");
    ASSERT_NE(selectView, nullptr);

    constexpr uint64_t kOutputSize = sizeof(uint32_t);
    BufferDescriptor outputBufferDesc{};
    outputBufferDesc.Size = kOutputSize;
    outputBufferDesc.Memory = MemoryType::Device;
    outputBufferDesc.Usage = BufferUse::CopySource | BufferUse::UnorderedAccess;
    auto outputBuffer = this->CreateBufferOrNull(outputBufferDesc, "Create bindless output buffer");
    ASSERT_NE(outputBuffer, nullptr);

    BufferDescriptor readbackBufferDesc{};
    readbackBufferDesc.Size = kOutputSize;
    readbackBufferDesc.Memory = MemoryType::ReadBack;
    readbackBufferDesc.Usage = BufferUse::CopyDestination | BufferUse::MapRead;
    auto readbackBuffer = this->CreateBufferOrNull(readbackBufferDesc, "Create bindless readback buffer");
    ASSERT_NE(readbackBuffer, nullptr);

    BufferViewDescriptor outputViewDesc{};
    outputViewDesc.Target = outputBuffer.get();
    outputViewDesc.Range = BufferRange{0, kOutputSize};
    outputViewDesc.Stride = sizeof(uint32_t);
    outputViewDesc.Usage = BufferViewUsage::ReadWriteStorage;
    auto outputView = this->CreateBufferViewOrNull(outputViewDesc, "Create bindless output UAV");
    ASSERT_NE(outputView, nullptr);

    BindlessArrayDescriptor bindlessDesc{};
    bindlessDesc.Size = 2;
    bindlessDesc.SlotType = BindlessSlotType::BufferOnly;
    auto bindless = this->CreateBindlessArrayOrNull(bindlessDesc);
    ASSERT_NE(bindless, nullptr);
    bindless->SetBuffer(0, inputView0.get());
    bindless->SetBuffer(1, inputView1.get());

    auto set1 = this->CreateDescriptorSetOrNull(program.RootSignatureObject.get(), DescriptorSetIndex{1});
    ASSERT_NE(set1, nullptr);
    ASSERT_TRUE(set1->WriteResource("gSelect", selectView.get()));

    auto set2 = this->CreateDescriptorSetOrNull(program.RootSignatureObject.get(), DescriptorSetIndex{2});
    ASSERT_NE(set2, nullptr);
    ASSERT_TRUE(set2->WriteResource("gOut", outputView.get()));

    auto cmdOpt = _ctx.CreateCommandBuffer(&reason);
    ASSERT_TRUE(cmdOpt.HasValue())
        << fmt::format("CreateCommandBuffer failed on {}: {}", _ctx.GetBackendName(), reason);
    auto cmd = cmdOpt.Release();

    _ctx.ClearCapturedErrors();
    cmd->Begin();
    ResourceBarrierDescriptor toUav = BarrierBufferDescriptor{
        .Target = outputBuffer.get(),
        .Before = BufferState::Common,
        .After = BufferState::UnorderedAccess,
    };
    cmd->ResourceBarrier(std::span{&toUav, 1});

    auto encoderOpt = cmd->BeginComputePass();
    ASSERT_TRUE(encoderOpt.HasValue());
    auto encoder = encoderOpt.Release();
    encoder->BindRootSignature(program.RootSignatureObject.get());
    encoder->BindComputePipelineState(program.PipelineObject.get());
    encoder->BindBindlessArray(DescriptorSetIndex{0}, bindless.get());
    encoder->BindDescriptorSet(DescriptorSetIndex{1}, set1.get());
    encoder->BindDescriptorSet(DescriptorSetIndex{2}, set2.get());
    encoder->Dispatch(1, 1, 1);
    cmd->EndComputePass(std::move(encoder));

    vector<ResourceBarrierDescriptor> preCopy{};
    preCopy.push_back(BarrierBufferDescriptor{
        .Target = outputBuffer.get(),
        .Before = BufferState::UnorderedAccess,
        .After = BufferState::CopySource,
    });
    _ctx.AppendReadbackPreCopyBarrier(preCopy, readbackBuffer.get());
    cmd->ResourceBarrier(preCopy);
    cmd->CopyBufferToBuffer(readbackBuffer.get(), 0, outputBuffer.get(), 0, kOutputSize);
    vector<ResourceBarrierDescriptor> postCopy{};
    _ctx.AppendReadbackPostCopyBarrier(postCopy, readbackBuffer.get());
    if (!postCopy.empty()) {
        cmd->ResourceBarrier(postCopy);
    }
    cmd->End();

    ASSERT_TRUE(_ctx.SubmitAndWait(cmd.get(), &reason))
        << fmt::format("SubmitAndWait failed on {}: {}", _ctx.GetBackendName(), reason);

    vector<uint32_t> actual = this->ReadBufferVectorOrEmpty<uint32_t>(readbackBuffer.get(), 1);
    ASSERT_EQ(actual.size(), 1u);
    EXPECT_EQ(actual[0], kValue1)
        << fmt::format("Unexpected bindless buffer result on {}", _ctx.GetBackendName());
    this->ExpectNoCapturedErrors();
}

TEST_P(ComputeBindingRuntimeTest, BindlessTextureAndSamplerWorks) {
    if (!_ctx.GetDeviceDetail().IsBindlessArraySupported) {
        GTEST_SKIP() << fmt::format("Bindless arrays are not supported on {}", _ctx.GetBackendName());
    }

    string reason{};
    auto programOpt = _ctx.CreateComputeProgram(kBindlessTextureShader, "CSMain", true, &reason);
    ASSERT_TRUE(programOpt.has_value())
        << fmt::format("CreateComputeProgram failed on {}: {}\n{}", _ctx.GetBackendName(), reason, _ctx.JoinCapturedErrors());
    auto program = std::move(programOpt.value());

    auto makeTexture = [&](std::array<byte, 16> texels, std::string_view label) -> std::pair<unique_ptr<Texture>, unique_ptr<TextureView>> {
        TextureDescriptor textureDesc{};
        textureDesc.Dim = TextureDimension::Dim2D;
        textureDesc.Width = 2;
        textureDesc.Height = 2;
        textureDesc.DepthOrArraySize = 1;
        textureDesc.MipLevels = 1;
        textureDesc.SampleCount = 1;
        textureDesc.Format = TextureFormat::RGBA8_UNORM;
        textureDesc.Memory = MemoryType::Device;
        textureDesc.Usage = TextureUse::CopyDestination | TextureUse::Resource;
        auto texture = this->CreateTextureOrNull(textureDesc, fmt::format("Create {}", label));
        EXPECT_NE(texture, nullptr);
        if (texture == nullptr) {
            return {};
        }
        EXPECT_TRUE(_ctx.UploadTexture2D(texture.get(), texels, &reason))
            << fmt::format("Upload {} failed on {}: {}", label, _ctx.GetBackendName(), reason);

        TextureViewDescriptor textureViewDesc{};
        textureViewDesc.Target = texture.get();
        textureViewDesc.Dim = TextureDimension::Dim2D;
        textureViewDesc.Format = TextureFormat::RGBA8_UNORM;
        textureViewDesc.Range = SubresourceRange{0, 1, 0, 1};
        textureViewDesc.Usage = TextureViewUsage::Resource;
        auto view = this->CreateTextureViewOrNull(textureViewDesc, fmt::format("Create {}", label));
        EXPECT_NE(view, nullptr);
        return {std::move(texture), std::move(view)};
    };

    //clang-format off
    auto [texture0, textureView0] = makeTexture(
        {byte{0x11}, byte{0x22}, byte{0x33}, byte{0xFF},
         byte{0x11}, byte{0x22}, byte{0x33}, byte{0xFF},
         byte{0x11}, byte{0x22}, byte{0x33}, byte{0xFF},
         byte{0x11}, byte{0x22}, byte{0x33}, byte{0xFF}},
        "bindless texture 0");
    //clang-format on
    ASSERT_NE(texture0, nullptr);
    ASSERT_NE(textureView0, nullptr);

    //clang-format off
    auto [texture1, textureView1] = makeTexture(
        {byte{0x44}, byte{0x55}, byte{0x66}, byte{0xFF},
         byte{0x44}, byte{0x55}, byte{0x66}, byte{0xFF},
         byte{0x44}, byte{0x55}, byte{0x66}, byte{0xFF},
         byte{0x44}, byte{0x55}, byte{0x66}, byte{0xFF}},
        "bindless texture 1");
    //clang-format on
    ASSERT_NE(texture1, nullptr);
    ASSERT_NE(textureView1, nullptr);

    SamplerDescriptor samplerDesc{};
    samplerDesc.AddressS = AddressMode::ClampToEdge;
    samplerDesc.AddressT = AddressMode::ClampToEdge;
    samplerDesc.AddressR = AddressMode::ClampToEdge;
    samplerDesc.MinFilter = FilterMode::Nearest;
    samplerDesc.MagFilter = FilterMode::Nearest;
    samplerDesc.MipmapFilter = FilterMode::Nearest;
    samplerDesc.LodMin = 0.0f;
    samplerDesc.LodMax = 0.0f;
    samplerDesc.Compare = std::nullopt;
    samplerDesc.AnisotropyClamp = 1;
    auto sampler = this->CreateSamplerOrNull(samplerDesc, "Create bindless sampler");
    ASSERT_NE(sampler, nullptr);

    const uint64_t cbufferSize = this->GetCBufferSize(sizeof(SampleDataCpu));
    BufferDescriptor sampleBufferDesc{};
    sampleBufferDesc.Size = cbufferSize;
    sampleBufferDesc.Memory = MemoryType::Device;
    sampleBufferDesc.Usage = BufferUse::CopyDestination | BufferUse::CBuffer;
    auto sampleBuffer = this->CreateBufferOrNull(sampleBufferDesc, "Create sample buffer");
    ASSERT_NE(sampleBuffer, nullptr);

    SampleDataCpu sampleData{};
    sampleData.Slot = 1;
    sampleData.U = 0.25f;
    sampleData.V = 0.25f;
    vector<byte> paddedSample(cbufferSize, byte{0});
    std::memcpy(paddedSample.data(), &sampleData, sizeof(sampleData));
    ASSERT_TRUE(_ctx.UploadBufferData(sampleBuffer.get(), paddedSample, BufferState::CBuffer, &reason))
        << fmt::format("Upload sample buffer failed on {}: {}", _ctx.GetBackendName(), reason);

    BufferViewDescriptor sampleViewDesc{};
    sampleViewDesc.Target = sampleBuffer.get();
    sampleViewDesc.Range = BufferRange{0, cbufferSize};
    sampleViewDesc.Usage = BufferViewUsage::CBuffer;
    auto sampleView = this->CreateBufferViewOrNull(sampleViewDesc, "Create sample CBV");
    ASSERT_NE(sampleView, nullptr);

    constexpr uint64_t kOutputSize = sizeof(uint32_t);
    BufferDescriptor outputBufferDesc{};
    outputBufferDesc.Size = kOutputSize;
    outputBufferDesc.Memory = MemoryType::Device;
    outputBufferDesc.Usage = BufferUse::CopySource | BufferUse::UnorderedAccess;
    auto outputBuffer = this->CreateBufferOrNull(outputBufferDesc, "Create bindless texture output buffer");
    ASSERT_NE(outputBuffer, nullptr);

    BufferDescriptor readbackBufferDesc{};
    readbackBufferDesc.Size = kOutputSize;
    readbackBufferDesc.Memory = MemoryType::ReadBack;
    readbackBufferDesc.Usage = BufferUse::CopyDestination | BufferUse::MapRead;
    auto readbackBuffer = this->CreateBufferOrNull(readbackBufferDesc, "Create bindless texture readback buffer");
    ASSERT_NE(readbackBuffer, nullptr);

    BufferViewDescriptor outputViewDesc{};
    outputViewDesc.Target = outputBuffer.get();
    outputViewDesc.Range = BufferRange{0, kOutputSize};
    outputViewDesc.Stride = sizeof(uint32_t);
    outputViewDesc.Usage = BufferViewUsage::ReadWriteStorage;
    auto outputView = this->CreateBufferViewOrNull(outputViewDesc, "Create bindless texture output UAV");
    ASSERT_NE(outputView, nullptr);

    BindlessArrayDescriptor bindlessDesc{};
    bindlessDesc.Size = 2;
    bindlessDesc.SlotType = BindlessSlotType::Texture2DOnly;
    auto bindless = this->CreateBindlessArrayOrNull(bindlessDesc);
    ASSERT_NE(bindless, nullptr);
    bindless->SetTexture(0, textureView0.get(), nullptr);
    bindless->SetTexture(1, textureView1.get(), nullptr);

    auto set1 = this->CreateDescriptorSetOrNull(program.RootSignatureObject.get(), DescriptorSetIndex{1});
    ASSERT_NE(set1, nullptr);
    ASSERT_TRUE(set1->WriteResource("gSample", sampleView.get()));
    ASSERT_TRUE(set1->WriteSampler("gSamp", sampler.get()));

    auto set2 = this->CreateDescriptorSetOrNull(program.RootSignatureObject.get(), DescriptorSetIndex{2});
    ASSERT_NE(set2, nullptr);
    ASSERT_TRUE(set2->WriteResource("gOut", outputView.get()));

    auto cmdOpt = _ctx.CreateCommandBuffer(&reason);
    ASSERT_TRUE(cmdOpt.HasValue())
        << fmt::format("CreateCommandBuffer failed on {}: {}", _ctx.GetBackendName(), reason);
    auto cmd = cmdOpt.Release();

    _ctx.ClearCapturedErrors();
    cmd->Begin();
    ResourceBarrierDescriptor toUav = BarrierBufferDescriptor{
        .Target = outputBuffer.get(),
        .Before = BufferState::Common,
        .After = BufferState::UnorderedAccess,
    };
    cmd->ResourceBarrier(std::span{&toUav, 1});

    auto encoderOpt = cmd->BeginComputePass();
    ASSERT_TRUE(encoderOpt.HasValue());
    auto encoder = encoderOpt.Release();
    encoder->BindRootSignature(program.RootSignatureObject.get());
    encoder->BindComputePipelineState(program.PipelineObject.get());
    encoder->BindBindlessArray(DescriptorSetIndex{0}, bindless.get());
    encoder->BindDescriptorSet(DescriptorSetIndex{1}, set1.get());
    encoder->BindDescriptorSet(DescriptorSetIndex{2}, set2.get());
    encoder->Dispatch(1, 1, 1);
    cmd->EndComputePass(std::move(encoder));

    vector<ResourceBarrierDescriptor> preCopy{};
    preCopy.push_back(BarrierBufferDescriptor{
        .Target = outputBuffer.get(),
        .Before = BufferState::UnorderedAccess,
        .After = BufferState::CopySource,
    });
    _ctx.AppendReadbackPreCopyBarrier(preCopy, readbackBuffer.get());
    cmd->ResourceBarrier(preCopy);
    cmd->CopyBufferToBuffer(readbackBuffer.get(), 0, outputBuffer.get(), 0, kOutputSize);
    vector<ResourceBarrierDescriptor> postCopy{};
    _ctx.AppendReadbackPostCopyBarrier(postCopy, readbackBuffer.get());
    if (!postCopy.empty()) {
        cmd->ResourceBarrier(postCopy);
    }
    cmd->End();

    ASSERT_TRUE(_ctx.SubmitAndWait(cmd.get(), &reason))
        << fmt::format("SubmitAndWait failed on {}: {}", _ctx.GetBackendName(), reason);

    vector<uint32_t> actual = this->ReadBufferVectorOrEmpty<uint32_t>(readbackBuffer.get(), 1);
    ASSERT_EQ(actual.size(), 1u);
    constexpr uint32_t kExpectedPacked = 0xFF665544u;
    EXPECT_EQ(actual[0], kExpectedPacked)
        << fmt::format("Unexpected bindless texture result on {}", _ctx.GetBackendName());
    this->ExpectNoCapturedErrors();
}

TEST_P(ComputeBindingRuntimeTest, BindBindlessArrayFailsWhenSetIsNotBindless) {
    if (!_ctx.GetDeviceDetail().IsBindlessArraySupported) {
        GTEST_SKIP() << fmt::format("Bindless arrays are not supported on {}", _ctx.GetBackendName());
    }

    string reason{};
    auto programOpt = _ctx.CreateComputeProgram(kMultiSetBufferShader, "CSMain", false, &reason);
    ASSERT_TRUE(programOpt.has_value())
        << fmt::format("CreateComputeProgram failed on {}: {}\n{}", _ctx.GetBackendName(), reason, _ctx.JoinCapturedErrors());
    auto program = std::move(programOpt.value());

    BindlessArrayDescriptor bindlessDesc{};
    bindlessDesc.Size = 1;
    bindlessDesc.SlotType = BindlessSlotType::BufferOnly;
    auto bindless = this->CreateBindlessArrayOrNull(bindlessDesc);
    ASSERT_NE(bindless, nullptr);

    auto cmdOpt = _ctx.CreateCommandBuffer(&reason);
    ASSERT_TRUE(cmdOpt.HasValue());
    auto cmd = cmdOpt.Release();
    cmd->Begin();
    auto encoderOpt = cmd->BeginComputePass();
    ASSERT_TRUE(encoderOpt.HasValue());
    auto encoder = encoderOpt.Release();
    encoder->BindRootSignature(program.RootSignatureObject.get());
    encoder->BindComputePipelineState(program.PipelineObject.get());

    _ctx.ClearCapturedErrors();
    encoder->BindBindlessArray(DescriptorSetIndex{0}, bindless.get());
    cmd->EndComputePass(std::move(encoder));
    cmd->End();

    const string captured = _ctx.JoinCapturedErrors();
    EXPECT_NE(captured.find("not declared as a bindless set"), string::npos)
        << fmt::format("Expected bindless set declaration error on {}, log:\n{}", _ctx.GetBackendName(), captured);
}

TEST_P(ComputeBindingRuntimeTest, BindBindlessArrayFailsWhenSlotTypeMismatches) {
    if (!_ctx.GetDeviceDetail().IsBindlessArraySupported) {
        GTEST_SKIP() << fmt::format("Bindless arrays are not supported on {}", _ctx.GetBackendName());
    }

    string reason{};
    auto programOpt = _ctx.CreateComputeProgram(kBindlessTextureShader, "CSMain", true, &reason);
    ASSERT_TRUE(programOpt.has_value())
        << fmt::format("CreateComputeProgram failed on {}: {}\n{}", _ctx.GetBackendName(), reason, _ctx.JoinCapturedErrors());
    auto program = std::move(programOpt.value());

    BindlessArrayDescriptor bindlessDesc{};
    bindlessDesc.Size = 1;
    bindlessDesc.SlotType = BindlessSlotType::BufferOnly;
    auto bindless = this->CreateBindlessArrayOrNull(bindlessDesc);
    ASSERT_NE(bindless, nullptr);

    auto cmdOpt = _ctx.CreateCommandBuffer(&reason);
    ASSERT_TRUE(cmdOpt.HasValue());
    auto cmd = cmdOpt.Release();
    cmd->Begin();
    auto encoderOpt = cmd->BeginComputePass();
    ASSERT_TRUE(encoderOpt.HasValue());
    auto encoder = encoderOpt.Release();
    encoder->BindRootSignature(program.RootSignatureObject.get());
    encoder->BindComputePipelineState(program.PipelineObject.get());

    _ctx.ClearCapturedErrors();
    encoder->BindBindlessArray(DescriptorSetIndex{0}, bindless.get());
    cmd->EndComputePass(std::move(encoder));
    cmd->End();

    const string captured = _ctx.JoinCapturedErrors();
    EXPECT_NE(captured.find("slot type mismatch"), string::npos)
        << fmt::format("Expected bindless slot type mismatch on {}, log:\n{}", _ctx.GetBackendName(), captured);
}

TEST_P(ComputeBindingRuntimeTest, BindDescriptorSetFailsWhenSetIndexMismatch) {
    string reason{};
    auto programOpt = _ctx.CreateComputeProgram(kMultiSetBufferShader, "CSMain", false, &reason);
    ASSERT_TRUE(programOpt.has_value())
        << fmt::format("CreateComputeProgram failed on {}: {}\n{}", _ctx.GetBackendName(), reason, _ctx.JoinCapturedErrors());
    auto program = std::move(programOpt.value());

    const uint64_t cbufferSize = this->GetCBufferSize(sizeof(ScaleDataCpu));
    BufferDescriptor scaleBufferDesc{};
    scaleBufferDesc.Size = cbufferSize;
    scaleBufferDesc.Memory = MemoryType::Upload;
    scaleBufferDesc.Usage = BufferUse::MapWrite | BufferUse::CBuffer;
    auto scaleBuffer = this->CreateBufferOrNull(scaleBufferDesc, "Create scale buffer");
    ASSERT_NE(scaleBuffer, nullptr);

    ScaleDataCpu scaleData{.Scale = 9};
    vector<byte> paddedScale(cbufferSize, byte{0});
    std::memcpy(paddedScale.data(), &scaleData, sizeof(scaleData));
    ASSERT_TRUE(_ctx.WriteHostVisibleBuffer(scaleBuffer.get(), paddedScale, &reason))
        << fmt::format("Write scale buffer failed on {}: {}", _ctx.GetBackendName(), reason);

    BufferViewDescriptor scaleViewDesc{};
    scaleViewDesc.Target = scaleBuffer.get();
    scaleViewDesc.Range = BufferRange{0, cbufferSize};
    scaleViewDesc.Usage = BufferViewUsage::CBuffer;
    auto scaleView = this->CreateBufferViewOrNull(scaleViewDesc, "Create scale CBV");
    ASSERT_NE(scaleView, nullptr);

    auto set0 = this->CreateDescriptorSetOrNull(program.RootSignatureObject.get(), DescriptorSetIndex{0});
    ASSERT_NE(set0, nullptr);
    ASSERT_TRUE(set0->WriteResource("gScale", scaleView.get()));

    auto cmdOpt = _ctx.CreateCommandBuffer(&reason);
    ASSERT_TRUE(cmdOpt.HasValue())
        << fmt::format("CreateCommandBuffer failed on {}: {}", _ctx.GetBackendName(), reason);
    auto cmd = cmdOpt.Release();
    cmd->Begin();
    auto encoderOpt = cmd->BeginComputePass();
    ASSERT_TRUE(encoderOpt.HasValue());
    auto encoder = encoderOpt.Release();
    encoder->BindRootSignature(program.RootSignatureObject.get());
    encoder->BindComputePipelineState(program.PipelineObject.get());

    _ctx.ClearCapturedErrors();
    encoder->BindDescriptorSet(DescriptorSetIndex{1}, set0.get());
    cmd->EndComputePass(std::move(encoder));
    cmd->End();

    const string captured = _ctx.JoinCapturedErrors();
    EXPECT_NE(captured.find("descriptor set index mismatch"), string::npos)
        << fmt::format("Expected descriptor set mismatch error on {}, log:\n{}", _ctx.GetBackendName(), captured);
}

TEST_P(ComputeBindingRuntimeTest, BindDescriptorSetFailsWhenRequiredBindingMissing) {
    string reason{};
    auto programOpt = _ctx.CreateComputeProgram(kTextureSamplerShader, "CSMain", false, &reason);
    ASSERT_TRUE(programOpt.has_value())
        << fmt::format("CreateComputeProgram failed on {}: {}\n{}", _ctx.GetBackendName(), reason, _ctx.JoinCapturedErrors());
    auto program = std::move(programOpt.value());

    TextureDescriptor textureDesc{};
    textureDesc.Dim = TextureDimension::Dim2D;
    textureDesc.Width = 2;
    textureDesc.Height = 2;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.SampleCount = 1;
    textureDesc.Format = TextureFormat::RGBA8_UNORM;
    textureDesc.Memory = MemoryType::Device;
    textureDesc.Usage = TextureUse::CopyDestination | TextureUse::Resource;
    auto texture = this->CreateTextureOrNull(textureDesc, "Create test texture");
    ASSERT_NE(texture, nullptr);

    //clang-format off
    std::array<byte, 16> texels = {
        byte{0x11}, byte{0x22}, byte{0x33}, byte{0xFF},
        byte{0x99}, byte{0x88}, byte{0x77}, byte{0xFF},
        byte{0x44}, byte{0x55}, byte{0x66}, byte{0xFF},
        byte{0xAA}, byte{0xBB}, byte{0xCC}, byte{0xFF}};
    //clang-format on
    ASSERT_TRUE(_ctx.UploadTexture2D(texture.get(), texels, &reason))
        << fmt::format("UploadTexture2D failed on {}: {}", _ctx.GetBackendName(), reason);

    TextureViewDescriptor textureViewDesc{};
    textureViewDesc.Target = texture.get();
    textureViewDesc.Dim = TextureDimension::Dim2D;
    textureViewDesc.Format = TextureFormat::RGBA8_UNORM;
    textureViewDesc.Range = SubresourceRange{0, 1, 0, 1};
    textureViewDesc.Usage = TextureViewUsage::Resource;
    auto textureView = this->CreateTextureViewOrNull(textureViewDesc, "Create texture SRV");
    ASSERT_NE(textureView, nullptr);

    auto set0 = this->CreateDescriptorSetOrNull(program.RootSignatureObject.get(), DescriptorSetIndex{0});
    ASSERT_NE(set0, nullptr);
    ASSERT_TRUE(set0->WriteResource("gTex", textureView.get()));

    auto cmdOpt = _ctx.CreateCommandBuffer(&reason);
    ASSERT_TRUE(cmdOpt.HasValue())
        << fmt::format("CreateCommandBuffer failed on {}: {}", _ctx.GetBackendName(), reason);
    auto cmd = cmdOpt.Release();
    cmd->Begin();
    auto encoderOpt = cmd->BeginComputePass();
    ASSERT_TRUE(encoderOpt.HasValue());
    auto encoder = encoderOpt.Release();
    encoder->BindRootSignature(program.RootSignatureObject.get());
    encoder->BindComputePipelineState(program.PipelineObject.get());

    _ctx.ClearCapturedErrors();
    encoder->BindDescriptorSet(DescriptorSetIndex{0}, set0.get());
    cmd->EndComputePass(std::move(encoder));
    cmd->End();

    const string captured = _ctx.JoinCapturedErrors();
    EXPECT_NE(captured.find("descriptor set is missing parameter"), string::npos)
        << fmt::format("Expected missing binding error on {}, log:\n{}", _ctx.GetBackendName(), captured);
}

TEST_P(ComputeBindingRuntimeTest, PushConstantsFailsWhenSizeMismatch) {
    string reason{};
    auto programOpt = _ctx.CreateComputeProgram(kMultiSetBufferShader, "CSMain", false, &reason);
    ASSERT_TRUE(programOpt.has_value())
        << fmt::format("CreateComputeProgram failed on {}: {}\n{}", _ctx.GetBackendName(), reason, _ctx.JoinCapturedErrors());
    auto program = std::move(programOpt.value());

    auto pushIdOpt = program.RootSignatureObject->FindParameterId("gPush");
    ASSERT_TRUE(pushIdOpt.has_value());

    uint32_t wrongSizeData = 123;
    auto cmdOpt = _ctx.CreateCommandBuffer(&reason);
    ASSERT_TRUE(cmdOpt.HasValue())
        << fmt::format("CreateCommandBuffer failed on {}: {}", _ctx.GetBackendName(), reason);
    auto cmd = cmdOpt.Release();
    cmd->Begin();
    auto encoderOpt = cmd->BeginComputePass();
    ASSERT_TRUE(encoderOpt.HasValue());
    auto encoder = encoderOpt.Release();
    encoder->BindRootSignature(program.RootSignatureObject.get());
    encoder->BindComputePipelineState(program.PipelineObject.get());

    _ctx.ClearCapturedErrors();
    encoder->PushConstants(pushIdOpt.value(), &wrongSizeData, sizeof(wrongSizeData));
    cmd->EndComputePass(std::move(encoder));
    cmd->End();

    const string captured = _ctx.JoinCapturedErrors();
    EXPECT_NE(captured.find("push constant size mismatch"), string::npos)
        << fmt::format("Expected push constant size mismatch on {}, log:\n{}", _ctx.GetBackendName(), captured);
}

INSTANTIATE_TEST_SUITE_P(
    RenderBackends,
    ComputeBindingRuntimeTest,
    ::testing::ValuesIn(GetEnabledTestBackends()),
    [](const ::testing::TestParamInfo<TestBackend>& info) {
        return string{fmt::format("{}", info.param)};
    });

}  // namespace
}  // namespace radray::render::test
