#include "gpu_test_fixture.h"
#include "shader_contract_fixtures.h"

#include <radray/render/backend_shader_artifact.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/components/primitive_component.h>
#include <radray/runtime/material.h>
#include <radray/runtime/render_framework/mesh_draw.h>
#include <radray/runtime/render_framework/scene.h>
#include <radray/runtime/render_framework/viewport.h>
#include <radray/runtime/shader_program.h>
#include <radray/runtime/texture_asset.h>
#include <radray/runtime/wait_frame.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <utility>

namespace radray {
namespace {

constexpr uint32_t kWidth = 64;
constexpr uint32_t kHeight = 64;
constexpr render::TextureFormat kFormat = render::TextureFormat::RGBA8_UNORM;
// Looked up by name so inserting a fixture cannot silently repoint these at the wrong
// golden GPU artifact hash.
constexpr size_t FixtureIndex(std::string_view name) noexcept {
    const std::span<const render::test::ShaderContractFixture> fixtures =
        render::test::GetShaderContractFixtures();
    for (size_t index = 0; index < fixtures.size(); ++index) {
        if (fixtures[index].Name == name) {
            return index;
        }
    }
    return std::numeric_limits<size_t>::max();
}

constexpr size_t kTextureSamplerFixtureIndex = FixtureIndex("texture_sampler");
constexpr size_t kNestedTypesFixtureIndex = FixtureIndex("nested_types");
static_assert(kTextureSamplerFixtureIndex != std::numeric_limits<size_t>::max());
static_assert(kNestedTypesFixtureIndex != std::numeric_limits<size_t>::max());

vector<byte> ReadBinary(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size <= 0) {
        return {};
    }
    file.seekg(0, std::ios::beg);
    vector<byte> result(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(result.data()), size);
    return file.good() || file.eof() ? result : vector<byte>{};
}

Nullable<unique_ptr<ShaderProgram>> CreateFixtureProgram(
    render::Device& device,
    render::RenderBackend backend,
    std::string_view fixtureName,
    size_t fixtureIndex,
    const render::ShaderProgramLayoutRecipe& layoutRecipe) {
    const std::optional<shader::ShaderTarget> target =
        render::GetShaderTargetForBackend(backend);
    if (!target.has_value()) {
        return nullptr;
    }
    const string suffix = target.value() == shader::ShaderTarget::DXIL
                              ? ".dxil.bin"
                              : ".spirv.bin";
    const vector<byte> blob = ReadBinary(
        std::filesystem::path{RADRAY_PROJECT_DIR} /
        "modules/render/tests/data/shader_artifacts" /
        (string{fixtureName} + suffix));
    if (blob.empty()) {
        return nullptr;
    }
    std::optional<render::BackendShaderArtifact> artifact =
        render::CreateBackendShaderArtifact(
            device,
            blob,
            shader::ShaderArtifactDecodeOptions{
                .Target = target.value(),
                .ExpectedGpuArtifact = render::test::ExpectedGpuArtifact(
                    fixtureIndex,
                    target.value()),
                .ExpectedToolchainIdentity = 0x0000000001090211ull},
            layoutRecipe);
    if (!artifact.has_value()) {
        return nullptr;
    }
    return ShaderProgram::Create(
        device.GetBackend() == backend ? &device : nullptr,
        std::move(artifact.value()));
}

Nullable<unique_ptr<ShaderProgram>> CreateNestedTypesProgram(
    render::Device& device,
    render::RenderBackend backend) {
    // nested_types declares a single constant buffer named Constants; the draw path uploads it from
      // a per-frame arena, so that one declaration takes its offset at bind time on both targets.
    const render::ShaderLayoutSelector selector{
        .DeclarationName = "Constants",
        .ExpectedLogicalResourceKind = shader::ShaderBindingKind::CBuffer};
    render::ShaderProgramLayoutRecipe recipe;
    recipe.D3D12.BufferPlacements.push_back(
        {.Selector = selector, .Placement = render::D3D12BufferPlacement::RootDescriptor});
    recipe.Vulkan.BufferDescriptors.push_back(
        {.Selector = selector, .Placement = render::VulkanBufferDescriptorPlacement::Dynamic});
    return CreateFixtureProgram(
        device,
        backend,
        "nested_types",
        kNestedTypesFixtureIndex,
        recipe);
}

class ImmediateWaitFrame final : public IWaitFrameProcessor {
public:
    task<void> Wait() override { co_return; }
};

constexpr AssetId MakeTextureId(uint32_t value) noexcept {
    return AssetId{
        value,
        0x0102,
        0x4304,
        0x85,
        0x06,
        0x07,
        0x08,
        0x09,
        0x0a,
        0x0b,
        0x0c};
}

StreamingAssetRef<TextureAsset> AddTestTexture(
    AssetManager& assets,
    render::Device& device,
    const AssetId& id,
    string name) {
    Nullable<unique_ptr<render::Texture>> texture = device.CreateTexture(
        render::TextureDescriptor{
            .Dim = render::TextureDimension::Dim2D,
            .Width = 1,
            .Height = 1,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .SampleCount = 1,
            .Format = kFormat,
            .Memory = render::MemoryType::Device,
            .Usage = render::TextureUse::Resource,
            .Hints = render::ResourceHint::None});
    if (!texture.HasValue()) {
        return nullptr;
    }
    unique_ptr<render::Texture> textureObject = texture.Release();
    Nullable<unique_ptr<render::TextureView>> view = device.CreateTextureView(
        render::TextureViewDescriptor{
            .Target = textureObject.get(),
            .Dim = render::TextureDimension::Dim2D,
            .Format = kFormat,
            .Range = render::SubresourceRange{0, 1, 0, 1},
            .Usage = render::TextureViewUsage::Resource});
    if (!view.HasValue()) {
        return nullptr;
    }
    return assets.AddReady<TextureAsset>(
        id,
        make_unique<TextureAsset>(
            &device,
            std::move(name),
            std::move(textureObject),
            view.Release()));
}

PrimitiveVertexLayout MakePositionLayout(uint32_t stride = sizeof(float) * 3) {
    PrimitiveVertexLayout layout;
    layout.Buffers.push_back(render::VertexBufferLayout{
        .Binding = 0,
        .ArrayStride = stride,
        .StepMode = render::VertexStepMode::Vertex});
    layout.Attributes.push_back(PrimitiveVertexAttribute{
        .Semantic = "POSITION",
        .SemanticIndex = 0,
        .BufferBinding = 0,
        .Offset = 0,
        .Format = render::VertexFormat::FLOAT32X3});
    return layout;
}

void ExpectPixel(
    const uint8_t* pixels,
    uint64_t rowPitch,
    uint32_t x,
    uint32_t y,
    uint8_t red,
    uint8_t green,
    uint8_t blue) {
    const uint8_t* pixel = pixels + rowPitch * y + sizeof(uint32_t) * x;
    EXPECT_EQ(pixel[0], red);
    EXPECT_EQ(pixel[1], green);
    EXPECT_EQ(pixel[2], blue);
    EXPECT_EQ(pixel[3], 255);
}

void RunMaterialResourceResidency(
    render::test::DeviceContext& context,
    render::RenderBackend backend) {
    render::Device& device = *context.Device;
    // texture_sampler needs no placement modifier: the compiler's table entries are exactly what
    // this path binds.
    Nullable<unique_ptr<ShaderProgram>> programResult = CreateFixtureProgram(
        device,
        backend,
        "texture_sampler",
        kTextureSamplerFixtureIndex,
        render::ShaderProgramLayoutRecipe{});
    ASSERT_TRUE(programResult.HasValue());
    unique_ptr<ShaderProgram> program = programResult.Release();

    const uint32_t materialGroup =
        backend == render::RenderBackend::D3D12 ? 0u : 1u;
    Nullable<unique_ptr<Material>> materialResult = Material::Create(
        program.get(),
        BindingGroupPlan{
            materialGroup == 0 ? 1u : 0u,
            materialGroup,
            2u},
        2);
    ASSERT_TRUE(materialResult.HasValue());
    unique_ptr<Material> material = materialResult.Release();

    ImmediateWaitFrame waitFrame;
    AssetManager assets;
    assets.SetWaitFrameProcessor(&waitFrame);
    StreamingAssetRef<TextureAsset> textureA =
        AddTestTexture(assets, device, MakeTextureId(1), "texture-a");
    StreamingAssetRef<TextureAsset> textureB =
        AddTestTexture(assets, device, MakeTextureId(2), "texture-b");
    ASSERT_TRUE(textureA.IsReady());
    ASSERT_TRUE(textureB.IsReady());

    render::SamplerDescriptor sampler;
    ASSERT_TRUE(material->SetTexture("AlbedoTexture", textureA));
    ASSERT_TRUE(material->SetSampler("LinearSampler", sampler));
    const uint64_t initialVersion = material->GetResourceVersion();
    const Nullable<render::ShaderParameterSet*> flight0 =
        material->PrepareParameterSet(0, {});
    const Nullable<render::ShaderParameterSet*> flight1 =
        material->PrepareParameterSet(1, {});
    ASSERT_TRUE(flight0.HasValue());
    ASSERT_TRUE(flight1.HasValue());
    EXPECT_NE(flight0.Get(), flight1.Get());
    EXPECT_EQ(material->GetResidentResourceVersion(0), initialVersion);
    EXPECT_EQ(material->GetResidentResourceVersion(1), initialVersion);

    ASSERT_TRUE(material->SetTexture("AlbedoTexture", textureB));
    const uint64_t changedVersion = material->GetResourceVersion();
    ASSERT_GT(changedVersion, initialVersion);
    EXPECT_EQ(material->GetResidentParameterSet(0).Get(), flight0.Get());
    EXPECT_EQ(material->GetResidentParameterSet(1).Get(), flight1.Get());
    EXPECT_EQ(material->GetResidentResourceVersion(0), initialVersion);
    EXPECT_EQ(material->GetResidentResourceVersion(1), initialVersion);

    const Nullable<render::ShaderParameterSet*> rebuiltFlight0 =
        material->PrepareParameterSet(0, {});
    ASSERT_TRUE(rebuiltFlight0.HasValue());
    EXPECT_NE(rebuiltFlight0.Get(), flight0.Get());
    EXPECT_EQ(material->GetResidentResourceVersion(0), changedVersion);
    EXPECT_EQ(material->GetResidentParameterSet(1).Get(), flight1.Get());
    EXPECT_EQ(material->GetResidentResourceVersion(1), initialVersion);

    const Nullable<render::ShaderParameterSet*> rebuiltFlight1 =
        material->PrepareParameterSet(1, {});
    ASSERT_TRUE(rebuiltFlight1.HasValue());
    EXPECT_NE(rebuiltFlight1.Get(), flight1.Get());
    EXPECT_EQ(material->GetResidentResourceVersion(1), changedVersion);

    material.reset();
    textureB.Reset();
    textureA.Reset();
    assets.Pump();
    EXPECT_EQ(assets.GetAssetCount(), 0u);
}

void RunMeshDraw(
    render::test::DeviceContext& context,
    render::RenderBackend backend) {
    render::Device& device = *context.Device;
    Nullable<unique_ptr<ShaderProgram>> programResult =
        CreateNestedTypesProgram(device, backend);
    ASSERT_TRUE(programResult.HasValue());
    unique_ptr<ShaderProgram> program = programResult.Release();
    ASSERT_TRUE(program->IsBufferDynamic("Constants"));
    // Dynamic-ness is a property of one declaration, so an unrelated name is not dynamic.
    EXPECT_FALSE(program->IsBufferDynamic("NotDeclared"));
    ASSERT_EQ(program->GetParameterLayout().Buffers().size(), 1u);
    const ShaderParameterBufferLayout& parameterBufferLayout =
        program->GetParameterLayout().Buffers().front();
    ASSERT_EQ(parameterBufferLayout.Group, 0u);

    auto target = render::test::MakeRenderTarget(
        &device,
        kFormat,
        kWidth,
        kHeight,
        render::TextureUse::RenderTarget | render::TextureUse::CopySource);
    ASSERT_TRUE(target.has_value());
    const render::RenderPassColorAttachmentDescriptor colorAttachment{
        .Format = kFormat,
        .SampleCount = 1,
        .Load = render::LoadAction::Clear,
        .Store = render::StoreAction::Store};
    Nullable<unique_ptr<render::RenderPass>> passResult =
        device.CreateRenderPass(render::RenderPassDescriptor{
            .ColorAttachments = std::span{&colorAttachment, 1}});
    ASSERT_TRUE(passResult.HasValue());
    unique_ptr<render::RenderPass> pass = passResult.Release();
    render::TextureView* colorView = target->View.get();
    Nullable<unique_ptr<render::Framebuffer>> framebufferResult =
        device.CreateFramebuffer(render::FramebufferDescriptor{
            .Pass = pass.get(),
            .ColorAttachments =
                std::span<render::TextureView* const>{&colorView, 1},
            .Width = kWidth,
            .Height = kHeight,
            .Layers = 1});
    ASSERT_TRUE(framebufferResult.HasValue());
    unique_ptr<render::Framebuffer> framebuffer = framebufferResult.Release();

    const PrimitiveVertexLayout vertexLayout = MakePositionLayout();
    Nullable<unique_ptr<Material>> materialResult = Material::Create(
        program.get(),
        BindingGroupPlan{1, 0, 2},
        2);
    ASSERT_TRUE(materialResult.HasValue());
    unique_ptr<Material> material = materialResult.Release();
    material->GetPipelineState().Primitive.Cull = render::CullMode::None;
    const GraphicsPassState passState{
        vector<render::TextureFormat>{kFormat},
        std::nullopt,
        1,
        pass.get()};
    const Nullable<render::GraphicsPipelineState*> pso =
        program->GetOrCreateGraphicsPipelineState(
            material->GetPipelineState(),
            vertexLayout,
            PrimitiveTopology::TriangleList,
            passState);
    ASSERT_TRUE(pso.HasValue());
    const Nullable<render::GraphicsPipelineState*> repeatedPso =
        program->GetOrCreateGraphicsPipelineState(
            material->GetPipelineState(),
            vertexLayout,
            PrimitiveTopology::TriangleList,
            passState);
    ASSERT_TRUE(repeatedPso.HasValue());
    EXPECT_EQ(pso.Get(), repeatedPso.Get());
    EXPECT_EQ(program->GetGraphicsPipelineStateCount(), 1u);

    MaterialPipelineState changedMaterialState = material->GetPipelineState();
    changedMaterialState.Primitive.Cull = render::CullMode::Front;
    EXPECT_TRUE(program->GetOrCreateGraphicsPipelineState(
                           changedMaterialState,
                           vertexLayout,
                           PrimitiveTopology::TriangleList,
                           passState)
                    .HasValue());
    EXPECT_TRUE(program->GetOrCreateGraphicsPipelineState(
                           material->GetPipelineState(),
                           MakePositionLayout(sizeof(float) * 4),
                           PrimitiveTopology::TriangleList,
                           passState)
                    .HasValue());
    EXPECT_TRUE(program->GetOrCreateGraphicsPipelineState(
                           material->GetPipelineState(),
                           vertexLayout,
                           PrimitiveTopology::LineList,
                           passState)
                    .HasValue());

    const render::RenderPassColorAttachmentDescriptor alternateAttachment{
        .Format = render::TextureFormat::BGRA8_UNORM,
        .SampleCount = 1,
        .Load = render::LoadAction::Clear,
        .Store = render::StoreAction::Store};
    Nullable<unique_ptr<render::RenderPass>> alternatePassResult =
        device.CreateRenderPass(render::RenderPassDescriptor{
            .ColorAttachments = std::span{&alternateAttachment, 1}});
    ASSERT_TRUE(alternatePassResult.HasValue());
    unique_ptr<render::RenderPass> alternatePass = alternatePassResult.Release();
    EXPECT_TRUE(program->GetOrCreateGraphicsPipelineState(
                           material->GetPipelineState(),
                           vertexLayout,
                           PrimitiveTopology::TriangleList,
                           GraphicsPassState{
                               vector<render::TextureFormat>{render::TextureFormat::BGRA8_UNORM},
                               std::nullopt,
                               1,
                               alternatePass.get()})
                    .HasValue());
    EXPECT_EQ(program->GetGraphicsPipelineStateCount(), 5u);

    constexpr array<float, 18> vertices{
        -0.95f, -0.8f, 0.0f,
        -0.05f, -0.8f, 0.0f,
        -0.5f, 0.8f, 0.0f,
        0.05f, -0.8f, 0.0f,
        0.95f, -0.8f, 0.0f,
        0.5f, 0.8f, 0.0f};
    constexpr array<uint16_t, 6> indices{0, 1, 2, 3, 4, 5};
    Nullable<unique_ptr<render::Buffer>> vertexBuffer =
        render::test::MakeUploadBuffer(
            device,
            std::as_bytes(std::span{vertices}),
            render::BufferUse::Vertex);
    Nullable<unique_ptr<render::Buffer>> indexBuffer =
        render::test::MakeUploadBuffer(
            device,
            std::as_bytes(std::span{indices}),
            render::BufferUse::Index);
    ASSERT_TRUE(vertexBuffer.HasValue());
    ASSERT_TRUE(indexBuffer.HasValue());

    const uint64_t parameterStride = Align(
        static_cast<uint64_t>(parameterBufferLayout.Size),
        std::max<uint64_t>(device.GetDetail().CBufferAlignment, 1));
    vector<byte> parameterBytes(
        static_cast<size_t>(parameterStride + parameterBufferLayout.Size),
        byte{0});
    ShaderParameterStorage redParameters{&program->GetParameterLayout()};
    ShaderParameterStorage greenParameters{&program->GetParameterLayout()};
    ASSERT_TRUE(redParameters.SetMatrix4x4("Transform", Eigen::Matrix4f::Identity()));
    ASSERT_TRUE(redParameters.SetFloat3("Direction", Eigen::Vector3f{1.0f, 0.0f, 0.0f}));
    ASSERT_TRUE(greenParameters.SetMatrix4x4("Transform", Eigen::Matrix4f::Identity()));
    ASSERT_TRUE(greenParameters.SetFloat3("Direction", Eigen::Vector3f{0.0f, 1.0f, 0.0f}));
    const std::span<const byte> redData = redParameters.GetBufferData(0);
    const std::span<const byte> greenData = greenParameters.GetBufferData(0);
    std::memcpy(parameterBytes.data(), redData.data(), redData.size());
    std::memcpy(
        parameterBytes.data() + parameterStride,
        greenData.data(),
        greenData.size());
    Nullable<unique_ptr<render::Buffer>> parameterBuffer =
        render::test::MakeUploadBuffer(
            device,
            parameterBytes,
            render::BufferUse::CBuffer);
    ASSERT_TRUE(parameterBuffer.HasValue());
    const MaterialBufferBinding materialBuffer{
        .BufferIndex = 0,
        .Value = render::ShaderBufferBinding{
            .Target = parameterBuffer.Get(),
            .Range = render::BufferRange{0, parameterBufferLayout.Size}}};
    const Nullable<render::ShaderParameterSet*> flightZeroSet =
        material->PrepareParameterSet(0, std::span{&materialBuffer, 1});
    const Nullable<render::ShaderParameterSet*> flightOneSet =
        material->PrepareParameterSet(1, std::span{&materialBuffer, 1});
    ASSERT_TRUE(flightZeroSet.HasValue());
    ASSERT_TRUE(flightOneSet.HasValue());
    EXPECT_NE(flightZeroSet.Get(), flightOneSet.Get());
    EXPECT_EQ(material->GetResidentResourceVersion(0), material->GetResourceVersion());
    EXPECT_EQ(material->GetResidentResourceVersion(1), material->GetResourceVersion());
    ASSERT_TRUE(material->SetMatrix4x4("Transform", Eigen::Matrix4f::Identity()));
    const Nullable<render::ShaderParameterSet*> repeatedFlightZeroSet =
        material->PrepareParameterSet(0, std::span{&materialBuffer, 1});
    ASSERT_TRUE(repeatedFlightZeroSet.HasValue());
    EXPECT_EQ(repeatedFlightZeroSet.Get(), flightZeroSet.Get());

    const uint32_t bytesPerPixel = render::GetTextureFormatBytesPerPixel(kFormat);
    const uint64_t rowPitch = Align(
        static_cast<uint64_t>(kWidth) * bytesPerPixel,
        device.GetDetail().TextureDataPitchAlignment);
    const uint64_t readbackSize = rowPitch * kHeight;
    Nullable<unique_ptr<render::Buffer>> readbackResult =
        device.CreateBuffer(render::BufferDescriptor{
            .Size = readbackSize,
            .Memory = render::MemoryType::ReadBack,
            .Usage = render::BufferUse::CopyDestination |
                     render::BufferUse::MapRead});
    ASSERT_TRUE(readbackResult.HasValue());
    unique_ptr<render::Buffer> readback = readbackResult.Release();
    Nullable<unique_ptr<render::CommandBuffer>> commandResult =
        device.CreateCommandBuffer(context.Queue);
    ASSERT_TRUE(commandResult.HasValue());
    unique_ptr<render::CommandBuffer> command = commandResult.Release();
    command->Begin();
    const render::ResourceBarrierDescriptor toTarget =
        render::BarrierTextureDescriptor{
            .Target = target->Tex.get(),
            .Before = render::TextureState::Undefined,
            .After = render::TextureState::RenderTarget};
    command->ResourceBarrier(std::span{&toTarget, 1});
    const render::ColorClearValue clear{{0.0f, 0.0f, 0.0f, 1.0f}};
    Nullable<unique_ptr<render::GraphicsCommandEncoder>> encoderResult =
        command->BeginRenderPass(render::RenderPassBeginDescriptor{
            .Pass = pass.get(),
            .Target = framebuffer.get(),
            .ColorClearValues = std::span{&clear, 1},
            .Name = "runtime mesh draw dynamic offsets"});
    ASSERT_TRUE(encoderResult.HasValue());
    unique_ptr<render::GraphicsCommandEncoder> encoder = encoderResult.Release();
    encoder->SetViewport(MakeViewport(backend, kWidth, kHeight));
    encoder->SetScissor(Rect{0, 0, kWidth, kHeight});
    encoder->BindGraphicsPipelineState(pso.Get());
    const render::VertexBufferBinding vertexBinding{
        .Binding = 0,
        .View = render::VertexBufferView{
            .Target = vertexBuffer.Get(),
            .Offset = 0,
            .Size = sizeof(vertices)}};
    encoder->BindVertexBuffers(std::span{&vertexBinding, 1});
    encoder->BindIndexBuffer(render::IndexBufferView{
        .Target = indexBuffer.Get(),
        .Offset = 0,
        .Stride = sizeof(uint16_t)});
    render::ShaderParameterDynamicOffset dynamicOffset{
        .Binding = parameterBufferLayout.Binding,
        .Offset = 0};
    encoder->BindShaderParameterSet(
        0,
        flightZeroSet.Get(),
        std::span{&dynamicOffset, 1});
    encoder->DrawIndexed(3, 1, 0, 0, 0);
    ASSERT_LE(parameterStride, std::numeric_limits<uint32_t>::max());
    dynamicOffset.Offset = static_cast<uint32_t>(parameterStride);
    encoder->BindShaderParameterSet(
        0,
        flightZeroSet.Get(),
        std::span{&dynamicOffset, 1});
    encoder->DrawIndexed(3, 1, 3, 0, 0);
    command->EndRenderPass(std::move(encoder));
    const render::ResourceBarrierDescriptor toCopy =
        render::BarrierTextureDescriptor{
            .Target = target->Tex.get(),
            .Before = render::TextureState::RenderTarget,
            .After = render::TextureState::CopySource};
    command->ResourceBarrier(std::span{&toCopy, 1});
    command->CopyTextureToBuffer(
        readback.get(),
        0,
        target->Tex.get(),
        render::SubresourceRange{0, 1, 0, 1});
    command->End();
    render::CommandBuffer* commands[]{command.get()};
    context.Queue->Submit(render::CommandQueueSubmitDescriptor{
        .CmdBuffers = commands});
    context.Queue->Wait();

    void* mapped = readback->Map(0, readbackSize);
    ASSERT_NE(mapped, nullptr);
    readback->InvalidateMappedRange(render::BufferRange{0, readbackSize});
    const auto* pixels = static_cast<const uint8_t*>(mapped);
    ExpectPixel(pixels, rowPitch, kWidth / 4, kHeight / 2, 255, 0, 0);
    ExpectPixel(pixels, rowPitch, kWidth * 3 / 4, kHeight / 2, 0, 255, 0);
    ExpectPixel(pixels, rowPitch, 0, 0, 0, 0, 0);
    readback->Unmap();
}

class TestPrimitiveProxy final : public PrimitiveSceneProxy {
public:
    TestPrimitiveProxy(
        vector<MeshDrawArgs> draws,
        vector<Nullable<Material*>> materials,
        const Eigen::Matrix4f& localToWorld)
        : _draws(std::move(draws)),
          _materials(std::move(materials)),
          _localToWorld(localToWorld) {}

    Eigen::Matrix4f GetLocalToWorld() const noexcept override {
        return _localToWorld;
    }
    MeshDrawArgs GetDrawArgs(uint32_t sectionIndex) const noexcept override {
        return sectionIndex < _draws.size() ? _draws[sectionIndex] : MeshDrawArgs{};
    }
    uint32_t GetSectionCount() const noexcept override {
        return static_cast<uint32_t>(_draws.size());
    }
    Nullable<Material*> GetMaterial(uint32_t sectionIndex) const noexcept override {
        return sectionIndex < _materials.size() ? _materials[sectionIndex] : nullptr;
    }

private:
    vector<MeshDrawArgs> _draws;
    vector<Nullable<Material*>> _materials;
    Eigen::Matrix4f _localToWorld;
};

class TestPrimitiveComponent final : public PrimitiveComponent {
public:
    TestPrimitiveComponent(
        vector<MeshDrawArgs> draws,
        vector<Nullable<Material*>> materials,
        float viewDepth)
        : _draws(std::move(draws)),
          _materials(std::move(materials)),
          _localToWorld(Eigen::Matrix4f::Identity()) {
        _localToWorld(2, 3) = viewDepth;
    }

    unique_ptr<PrimitiveSceneProxy> CreateSceneProxy() override {
        return make_unique<TestPrimitiveProxy>(
            _draws,
            _materials,
            _localToWorld);
    }

private:
    vector<MeshDrawArgs> _draws;
    vector<Nullable<Material*>> _materials;
    Eigen::Matrix4f _localToWorld;
};

void RunDrawListSort(render::test::DeviceContext& context) {
    const render::RenderBackend backend = context.Device->GetBackend();
    Nullable<unique_ptr<ShaderProgram>> firstProgramResult =
        CreateNestedTypesProgram(*context.Device, backend);
    Nullable<unique_ptr<ShaderProgram>> secondProgramResult =
        CreateNestedTypesProgram(*context.Device, backend);
    ASSERT_TRUE(firstProgramResult.HasValue());
    ASSERT_TRUE(secondProgramResult.HasValue());
    unique_ptr<ShaderProgram> firstProgram = firstProgramResult.Release();
    unique_ptr<ShaderProgram> secondProgram = secondProgramResult.Release();
    const BindingGroupPlan groups{1, 0, 2};
    Nullable<unique_ptr<Material>> materialAResult =
        Material::Create(firstProgram.get(), groups, 1);
    Nullable<unique_ptr<Material>> materialA2Result =
        Material::Create(firstProgram.get(), groups, 1);
    Nullable<unique_ptr<Material>> materialBResult =
        Material::Create(secondProgram.get(), groups, 1);
    Nullable<unique_ptr<Material>> transparentResult =
        Material::Create(firstProgram.get(), groups, 1);
    ASSERT_TRUE(materialAResult.HasValue());
    ASSERT_TRUE(materialA2Result.HasValue());
    ASSERT_TRUE(materialBResult.HasValue());
    ASSERT_TRUE(transparentResult.HasValue());
    unique_ptr<Material> materialA = materialAResult.Release();
    unique_ptr<Material> materialA2 = materialA2Result.Release();
    unique_ptr<Material> materialB = materialBResult.Release();
    unique_ptr<Material> transparent = transparentResult.Release();
    transparent->SetRenderQueue(RenderQueue::Transparent);

    GpuMesh::DrawData geometry;
    const auto draw = [&](uint32_t firstIndex, uint32_t count, int32_t vertexOffset) {
        return MeshDrawArgs{
            .Geometry = &geometry,
            .FirstIndex = firstIndex,
            .IndexCount = count,
            .VertexOffset = vertexOffset};
    };
    TestPrimitiveComponent opaqueA(
        {draw(11, 3, -1)},
        {materialA.get()},
        0.0f);
    TestPrimitiveComponent opaqueB(
        {draw(22, 4, -2)},
        {materialB.get()},
        0.0f);
    TestPrimitiveComponent opaqueA2(
        {draw(33, 5, -3)},
        {materialA2.get()},
        0.0f);
    TestPrimitiveComponent opaqueARepeat(
        {draw(44, 6, -4)},
        {materialA.get()},
        0.0f);
    TestPrimitiveComponent farTransparent(
        {draw(55, 7, -5), draw(56, 8, -6)},
        {transparent.get(), transparent.get()},
        5.0f);
    TestPrimitiveComponent nearTransparent(
        {draw(66, 9, -7)},
        {transparent.get()},
        2.0f);

    Scene scene;
    ASSERT_NE(scene.AddPrimitive(&opaqueA), nullptr);
    ASSERT_NE(scene.AddPrimitive(&opaqueB), nullptr);
    ASSERT_NE(scene.AddPrimitive(&opaqueA2), nullptr);
    ASSERT_NE(scene.AddPrimitive(&opaqueARepeat), nullptr);
    ASSERT_NE(scene.AddPrimitive(&farTransparent), nullptr);
    ASSERT_NE(scene.AddPrimitive(&nearTransparent), nullptr);
    MeshDrawList list;
    list.Collect(&scene, Eigen::Matrix4f::Identity());
    ASSERT_EQ(list.Size(), 7u);
    list.Sort();
    const std::span<const MeshDrawItem> items = list.Items();

    for (size_t index = 0; index < 4; ++index) {
        EXPECT_LT(
            static_cast<int32_t>(items[index].DrawMaterial->GetRenderQueue()),
            static_cast<int32_t>(RenderQueue::GeometryLast));
    }
    size_t programTransitions = 0;
    for (size_t index = 1; index < 4; ++index) {
        programTransitions +=
            items[index - 1].DrawMaterial->GetProgram() !=
                    items[index].DrawMaterial->GetProgram()
                ? 1
                : 0;
    }
    EXPECT_EQ(programTransitions, 1u);

    std::optional<size_t> firstA;
    std::optional<size_t> repeatedA;
    for (size_t index = 0; index < 4; ++index) {
        if (items[index].FirstIndex == 11) {
            firstA = index;
        } else if (items[index].FirstIndex == 44) {
            repeatedA = index;
        }
    }
    ASSERT_TRUE(firstA.has_value());
    ASSERT_TRUE(repeatedA.has_value());
    EXPECT_EQ(repeatedA.value(), firstA.value() + 1);

    EXPECT_FLOAT_EQ(items[4].ViewDepth, 5.0f);
    EXPECT_FLOAT_EQ(items[5].ViewDepth, 5.0f);
    EXPECT_FLOAT_EQ(items[6].ViewDepth, 2.0f);
    EXPECT_EQ(items[4].SectionIndex, 0u);
    EXPECT_EQ(items[5].SectionIndex, 1u);
    EXPECT_EQ(items[4].FirstIndex, 55u);
    EXPECT_EQ(items[4].IndexCount, 7u);
    EXPECT_EQ(items[4].VertexOffset, -5);
    EXPECT_EQ(items[5].FirstIndex, 56u);
    EXPECT_EQ(items[6].FirstIndex, 66u);
}

TEST(RadRayRuntimeMeshDraw, DrawListClustersOpaqueAndSortsTransparentStably) {
    render::test::DeviceContext context;
    if (!render::test::TryCreateAnyDevice(context)) {
        GTEST_SKIP() << "No render backend is available";
    }
    RunDrawListSort(context);
}

TEST(RadRayRuntimeMeshDraw, D3D12DynamicOffsetsAndIndexedDraw) {
#if defined(RADRAY_ENABLE_D3D12)
    render::test::DeviceContext context;
    if (!render::test::TryCreateDevice(render::RenderBackend::D3D12, context)) {
        GTEST_SKIP() << "D3D12 is unavailable";
    }
    RunMeshDraw(context, render::RenderBackend::D3D12);
#else
    GTEST_SKIP() << "D3D12 is disabled";
#endif
}

TEST(RadRayRuntimeMeshDraw, D3D12MaterialResourceResidencyRotatesByFlight) {
#if defined(RADRAY_ENABLE_D3D12)
    render::test::DeviceContext context;
    if (!render::test::TryCreateDevice(render::RenderBackend::D3D12, context)) {
        GTEST_SKIP() << "D3D12 is unavailable";
    }
    RunMaterialResourceResidency(context, render::RenderBackend::D3D12);
#else
    GTEST_SKIP() << "D3D12 is disabled";
#endif
}

TEST(RadRayRuntimeMeshDraw, VulkanDynamicOffsetsAndIndexedDraw) {
#if defined(RADRAY_ENABLE_VULKAN)
    render::test::DeviceContext context;
    if (!render::test::TryCreateDevice(
            render::RenderBackend::Vulkan,
            context,
            true)) {
        GTEST_SKIP() << "Vulkan is unavailable";
    }
    RunMeshDraw(context, render::RenderBackend::Vulkan);
#else
    GTEST_SKIP() << "Vulkan is disabled";
#endif
}

TEST(RadRayRuntimeMeshDraw, VulkanMaterialResourceResidencyRotatesByFlight) {
#if defined(RADRAY_ENABLE_VULKAN)
    render::test::DeviceContext context;
    if (!render::test::TryCreateDevice(
            render::RenderBackend::Vulkan,
            context,
            true)) {
        GTEST_SKIP() << "Vulkan is unavailable";
    }
    RunMaterialResourceResidency(context, render::RenderBackend::Vulkan);
#else
    GTEST_SKIP() << "Vulkan is disabled";
#endif
}

}  // namespace
}  // namespace radray
