#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>

#include <radray/file.h>
#include <radray/runtime/gpu_resource.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/shader_asset.h>
#include <radray/runtime/texture_asset.h>

#if defined(RADRAY_ENABLE_DXC)
#include <radray/render/shader_compiler/dxc.h>
#endif

using namespace radray;

namespace {

class FakeBuffer final : public render::Buffer {
public:
    FakeBuffer(const render::BufferDescriptor& desc, uint32_t* destroyedCount)
        : _desc(desc), _storage(desc.Size), _destroyedCount(destroyedCount) {}

    ~FakeBuffer() noexcept override {
        if (_destroyedCount != nullptr) {
            ++*_destroyedCount;
        }
    }

    bool IsValid() const noexcept override { return _valid; }

    void Destroy() noexcept override {
        _valid = false;
        _storage.clear();
    }

    void* Map(uint64_t offset, uint64_t size) noexcept override {
        if (offset > _storage.size() || size > _storage.size() - offset) {
            return nullptr;
        }
        ++MapCount;
        return _storage.data() + offset;
    }

    void Unmap() noexcept override { ++UnmapCount; }

    void FlushMappedRange(render::BufferRange range) noexcept override {
        Flushes.push_back(render::MappedBufferRange{this, range});
    }

    void InvalidateMappedRange(render::BufferRange range) noexcept override {
        Invalidates.push_back(render::MappedBufferRange{this, range});
    }

    void SetDebugName(std::string_view name) noexcept override { Name = name; }

    render::BufferDescriptor GetDesc() const noexcept override { return _desc; }
    render::Device* GetDevice() const noexcept override { return nullptr; }

    uint32_t MapCount{0};
    uint32_t UnmapCount{0};
    vector<render::MappedBufferRange> Flushes;
    vector<render::MappedBufferRange> Invalidates;
    string Name;

private:
    render::BufferDescriptor _desc;
    vector<byte> _storage;
    uint32_t* _destroyedCount;
    bool _valid{true};
};

class FakeShader final : public render::Shader {
public:
    FakeShader(const render::ShaderDescriptor& desc, uint32_t* destroyCount)
        : _stages(desc.Stages), _reflection(desc.Reflection), _destroyCount(destroyCount) {}

    ~FakeShader() noexcept override { Destroy(); }

    bool IsValid() const noexcept override { return _valid; }

    void Destroy() noexcept override {
        if (!_valid) {
            return;
        }
        _valid = false;
        _stages = render::ShaderStage::UNKNOWN;
        _reflection.reset();
        if (_destroyCount != nullptr) {
            ++*_destroyCount;
        }
    }

    render::ShaderStages GetStages() const noexcept override { return _stages; }

    Nullable<const render::ShaderReflectionDesc*> GetReflection() const noexcept override {
        return _reflection.has_value() ? &*_reflection : nullptr;
    }

private:
    render::ShaderStages _stages;
    std::optional<render::ShaderReflectionDesc> _reflection;
    uint32_t* _destroyCount{nullptr};
    bool _valid{true};
};

class FakeDevice final : public render::Device {
public:
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    render::RenderBackend GetBackend() noexcept override { return Backend; }
    render::DeviceDetail GetDetail() const noexcept override { return {}; }

    Nullable<render::CommandQueue*> GetCommandQueue(render::QueueType, uint32_t) noexcept override { return nullptr; }
    Nullable<unique_ptr<render::CommandBuffer>> CreateCommandBuffer(render::CommandQueue*) noexcept override { return nullptr; }
    Nullable<unique_ptr<render::Fence>> CreateFence() noexcept override { return nullptr; }
    Nullable<unique_ptr<render::QueryPool>> CreateQueryPool(const render::QueryPoolDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<render::SwapChain>> CreateSwapChain(const render::SwapChainDescriptor&) noexcept override { return nullptr; }

    Nullable<unique_ptr<render::Buffer>> CreateBuffer(const render::BufferDescriptor& desc) noexcept override {
        BufferDescriptors.emplace_back(desc);
        unique_ptr<render::Buffer> buffer = make_unique<FakeBuffer>(desc, &DestroyedBufferCount);
        return buffer;
    }

    void FlushMappedRanges(std::span<const render::MappedBufferRange> ranges) noexcept override {
        FlushedRanges.assign(ranges.begin(), ranges.end());
    }

    Nullable<unique_ptr<render::Texture>> CreateTexture(const render::TextureDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<render::TextureView>> CreateTextureView(const render::TextureViewDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<render::RenderPass>> CreateRenderPass(const render::RenderPassDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<render::Framebuffer>> CreateFramebuffer(const render::FramebufferDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<render::Shader>> CreateShader(const render::ShaderDescriptor& desc) noexcept override {
        ++ShaderCreateCount;
        LastShaderCategory = desc.Category;
        LastShaderStages = desc.Stages;
        LastShaderSourceSize = desc.Source.size();
        LastShaderHasReflection = desc.Reflection.has_value();
        if (!AllowShaderCreation) {
            return nullptr;
        }
        unique_ptr<render::Shader> shader = make_unique<FakeShader>(desc, &DestroyedShaderCount);
        return shader;
    }
    Nullable<unique_ptr<render::PipelineLayout>> CreatePipelineLayout(const render::PipelineLayoutDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<render::DescriptorPool>> CreateDescriptorPool(const render::DescriptorPoolDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<render::BindingGroup>> CreateBindingGroup(
        render::DescriptorPool*,
        render::PipelineLayout*,
        uint32_t) noexcept override {
        return nullptr;
    }
    Nullable<unique_ptr<render::GraphicsPipelineState>> CreateGraphicsPipelineState(
        const render::GraphicsPipelineStateDescriptor&) noexcept override {
        return nullptr;
    }
    Nullable<unique_ptr<render::ComputePipelineState>> CreateComputePipelineState(
        const render::ComputePipelineStateDescriptor&) noexcept override {
        return nullptr;
    }
    Nullable<unique_ptr<render::AccelerationStructure>> CreateAccelerationStructure(
        const render::AccelerationStructureDescriptor&) noexcept override {
        return nullptr;
    }
    Nullable<unique_ptr<render::AccelerationStructureView>> CreateAccelerationStructureView(
        const render::AccelerationStructureViewDescriptor&) noexcept override {
        return nullptr;
    }
    Nullable<unique_ptr<render::RayTracingPipelineState>> CreateRayTracingPipelineState(
        const render::RayTracingPipelineStateDescriptor&) noexcept override {
        return nullptr;
    }
    Nullable<unique_ptr<render::ShaderBindingTable>> CreateShaderBindingTable(
        const render::ShaderBindingTableDescriptor&) noexcept override {
        return nullptr;
    }
    Nullable<unique_ptr<render::Sampler>> CreateSampler(const render::SamplerDescriptor&) noexcept override { return nullptr; }
    Nullable<unique_ptr<render::BindlessArray>> CreateBindlessArray(const render::BindlessArrayDescriptor&) noexcept override { return nullptr; }

    vector<render::BufferDescriptor> BufferDescriptors;
    vector<render::MappedBufferRange> FlushedRanges;
    render::RenderBackend Backend{render::RenderBackend::Vulkan};
    render::ShaderBlobCategory LastShaderCategory{render::ShaderBlobCategory::DXIL};
    render::ShaderStages LastShaderStages{render::ShaderStage::UNKNOWN};
    size_t LastShaderSourceSize{0};
    uint32_t ShaderCreateCount{0};
    uint32_t DestroyedShaderCount{0};
    uint32_t DestroyedBufferCount{0};
    bool LastShaderHasReflection{false};
    bool AllowShaderCreation{false};
};

DynamicCBufferArena MakeArena(
    FakeDevice& device,
    HostWriteBatch& batch,
    uint64_t pageSize = 1024,
    uint64_t alignment = 1,
    uint64_t maxResetSize = 4096) {
    return DynamicCBufferArena{
        &device,
        &batch,
        DynamicCBufferArena::Descriptor{
            .BasicSize = pageSize,
            .Alignment = alignment,
            .MaxResetSize = maxResetSize,
            .NamePrefix = "dynamic_test"}};
}

}  // namespace

TEST(CacheKeyHashTest, SamplerDescriptorUsesFieldValueSemantics) {
    render::SamplerDescriptor firstDesc{
        .AddressS = render::AddressMode::Repeat,
        .AddressT = render::AddressMode::Mirror,
        .AddressR = render::AddressMode::ClampToEdge,
        .MinFilter = render::FilterMode::Linear,
        .MagFilter = render::FilterMode::Nearest,
        .MipmapFilter = render::FilterMode::Linear,
        .LodMin = 0.0f,
        .LodMax = 8.0f,
        .Compare = render::CompareFunction::LessEqual,
        .AnisotropyClamp = 4};
    render::SamplerDescriptor secondDesc = firstDesc;
    secondDesc.LodMin = -0.0f;

    ASSERT_EQ(firstDesc, secondDesc);
    EXPECT_EQ(
        std::hash<render::SamplerDescriptor>{}(firstDesc),
        std::hash<render::SamplerDescriptor>{}(secondDesc));

    unordered_map<render::SamplerDescriptor, int> cache;
    cache.emplace(firstDesc, 1);
    cache.insert_or_assign(secondDesc, 2);
    EXPECT_EQ(cache.size(), 1u);
    EXPECT_EQ(cache.at(firstDesc), 2);
}

TEST(CacheKeyHashTest, TextureSubViewDescUsesFieldValueSemantics) {
    const TextureSubViewDesc first{
        .Dim = render::TextureDimension::Cube,
        .Format = render::TextureFormat::RGBA8_UNORM,
        .Range = {
            .BaseArrayLayer = 6,
            .ArrayLayerCount = 12,
            .BaseMipLevel = 2,
            .MipLevelCount = 5}};
    const TextureSubViewDesc second = first;

    ASSERT_EQ(first, second);
    EXPECT_EQ(
        std::hash<TextureSubViewDesc>{}(first),
        std::hash<TextureSubViewDesc>{}(second));

    unordered_map<TextureSubViewDesc, int> cache;
    cache.emplace(first, 1);
    cache.insert_or_assign(second, 2);
    EXPECT_EQ(cache.size(), 1u);
    EXPECT_EQ(cache.at(first), 2);
}

TEST(CacheKeyHashTest, ShaderModuleKeyUsesFieldValueSemantics) {
    const Guid shaderId = Guid::NewGuid();
    const ShaderModuleKey first{
        .Shader = shaderId,
        .Defines = {"ALPHA_TEST=1", "USE_NORMAL_MAP=1"},
        .PassIndex = 2,
        .Stage = render::ShaderStage::Pixel};
    const ShaderModuleKey second = first;

    ASSERT_EQ(first, second);
    EXPECT_EQ(std::hash<ShaderModuleKey>{}(first), std::hash<ShaderModuleKey>{}(second));

    unordered_map<ShaderModuleKey, int> cache;
    cache.emplace(first, 1);
    cache.insert_or_assign(second, 2);
    EXPECT_EQ(cache.size(), 1u);
    EXPECT_EQ(cache.at(first), 2);
}

#if defined(RADRAY_ENABLE_DXC)

TEST(ShaderModuleCacheTest, CompilesNormalizesAndCachesReadyShaderAsset) {
    auto dxcOpt = render::CreateDxc();
    if (!dxcOpt.HasValue()) {
        GTEST_SKIP() << "DXC runtime is unavailable";
    }
    shared_ptr<render::Dxc> dxc = dxcOpt.Release();

    const std::filesystem::path shaderRoot = GetExecutableDirectory() / "shaderlib";
    if (!std::filesystem::is_regular_file(shaderRoot / "forward_pipeline/error_pass.hlsl")) {
        GTEST_SKIP() << "deployed shaderlib is unavailable";
    }

    const AssetId shaderId = Guid::NewGuid();
    AssetManager assets;
    ShaderPassDesc pass{};
    pass.Name = "CacheTest";
    pass.SourcePath = "forward_pipeline/error_pass.hlsl";
    pass.SM = render::HlslShaderModel::SM66;
    pass.IsOptimize = false;
    pass.EnableUnbounded = false;
    pass.KeywordGroups = {
        ShaderKeywordGroupDesc{
            .Alternatives = {"", "CACHE_A=1"},
            .Stages = render::ShaderStage::Vertex},
        ShaderKeywordGroupDesc{
            .Alternatives = {"", "CACHE_B=1"},
            .Stages = render::ShaderStage::Vertex}};
    std::get<ShaderGraphicsPassDesc>(pass.Program).VertexEntry = "VSMain";
    StreamingAssetRef<ShaderAsset> shaderAsset = assets.AddReady(
        shaderId,
        make_unique<ShaderAsset>(vector<ShaderPassDesc>{std::move(pass)}));
    ASSERT_TRUE(shaderAsset.IsReady());

    FakeDevice device;
    device.Backend = render::RenderBackend::D3D12;
    device.AllowShaderCreation = true;
    ShaderModuleCache cache{&device, dxc.get(), &assets, shaderRoot.string()};

    const ShaderModuleKey unnormalized{
        .Shader = shaderId,
        .Defines = {"CACHE_B=1", "", "CACHE_A=1", "CACHE_B=1"},
        .PassIndex = 0,
        .Stage = render::ShaderStage::Vertex};
    auto first = cache.GetOrCreate(unnormalized);
    ASSERT_TRUE(first.HasValue());

    const ShaderModuleKey normalized{
        .Shader = shaderId,
        .Defines = {"CACHE_A=1", "CACHE_B=1"},
        .PassIndex = 0,
        .Stage = render::ShaderStage::Vertex};
    auto second = cache.GetOrCreate(normalized);
    ASSERT_TRUE(second.HasValue());

    EXPECT_EQ(first.Get(), second.Get());
    EXPECT_EQ(cache.GetCount(), 1u);
    EXPECT_EQ(device.ShaderCreateCount, 1u);
    EXPECT_EQ(device.LastShaderCategory, render::ShaderBlobCategory::DXIL);
    EXPECT_EQ(device.LastShaderStages, render::ShaderStage::Vertex);
    EXPECT_GT(device.LastShaderSourceSize, 0u);
    EXPECT_TRUE(device.LastShaderHasReflection);

    cache.Clear();
    EXPECT_EQ(cache.GetCount(), 0u);
    EXPECT_EQ(device.DestroyedShaderCount, 1u);

#if defined(RADRAY_ENABLE_SPIRV_CROSS)
    device.Backend = render::RenderBackend::Vulkan;
    ShaderModuleCache spirvCache{&device, dxc.get(), &assets, shaderRoot.string()};
    auto spirv = spirvCache.GetOrCreate(normalized);
    ASSERT_TRUE(spirv.HasValue());
    EXPECT_EQ(spirvCache.GetCount(), 1u);
    EXPECT_EQ(device.ShaderCreateCount, 2u);
    EXPECT_EQ(device.LastShaderCategory, render::ShaderBlobCategory::SPIRV);
    EXPECT_EQ(device.LastShaderStages, render::ShaderStage::Vertex);
    EXPECT_TRUE(device.LastShaderHasReflection);

    spirvCache.Clear();
    EXPECT_EQ(device.DestroyedShaderCount, 2u);
#endif
}

TEST(ShaderModuleCacheTest, ResolvesSourceRelativeToExecutableDirectory) {
    auto dxcOpt = render::CreateDxc();
    if (!dxcOpt.HasValue()) {
        GTEST_SKIP() << "DXC runtime is unavailable";
    }
    shared_ptr<render::Dxc> dxc = dxcOpt.Release();

    const std::filesystem::path executableDirectory = GetExecutableDirectory();
    const std::filesystem::path shaderRoot = executableDirectory / "shaderlib";
    if (!std::filesystem::is_regular_file(shaderRoot / "forward_pipeline/error_pass.hlsl")) {
        GTEST_SKIP() << "deployed shaderlib is unavailable";
    }

    const AssetId shaderId = Guid::NewGuid();
    AssetManager assets;
    ShaderPassDesc pass{};
    pass.Name = "ExecutableRelativePathTest";
    pass.SourcePath = "shaderlib/forward_pipeline/error_pass.hlsl";
    std::get<ShaderGraphicsPassDesc>(pass.Program).VertexEntry = "VSMain";
    StreamingAssetRef<ShaderAsset> shaderAsset = assets.AddReady(
        shaderId,
        make_unique<ShaderAsset>(vector<ShaderPassDesc>{std::move(pass)}));
    ASSERT_TRUE(shaderAsset.IsReady());

    FakeDevice device;
    device.Backend = render::RenderBackend::D3D12;
    device.AllowShaderCreation = true;
    ShaderModuleCache cache{&device, dxc.get(), &assets, shaderRoot.string()};

    auto shader = cache.GetOrCreate(ShaderModuleKey{
        .Shader = shaderId,
        .PassIndex = 0,
        .Stage = render::ShaderStage::Vertex});

    ASSERT_TRUE(shader.HasValue());
    EXPECT_EQ(cache.GetCount(), 1u);
    EXPECT_EQ(device.ShaderCreateCount, 1u);
}

#endif

TEST(MappedUploadPageTest, CommitRecordsOnlyActualPrefix) {
    FakeDevice device;
    HostWriteBatch batch;
    auto buffer = device.CreateBuffer(render::BufferDescriptor{
                                          .Size = 256,
                                          .Memory = render::MemoryType::Upload,
                                          .Usage = render::BufferUse::MapWrite | render::BufferUse::CopySource,
                                          .Hints = render::ResourceHint::PersistentMap})
                      .Unwrap();
    MappedUploadPage page{std::move(buffer), &batch};

    auto reservation = page.Reserve(128, 16, batch);
    ASSERT_TRUE(reservation.IsValid());
    std::memset(reservation.Data(), 0x5a, 37);
    const auto allocation = reservation.Commit(37);

    EXPECT_EQ(allocation.Offset, 0u);
    EXPECT_EQ(allocation.Size, 37u);
    ASSERT_EQ(batch.GetRanges().size(), 1u);
    EXPECT_EQ(batch.GetRanges()[0].Range.Size, 37u);
    EXPECT_EQ(batch.GetStats().PageCount, 1u);
    EXPECT_EQ(batch.GetStats().PageCapacityBytes, 256u);
    EXPECT_EQ(batch.GetStats().CommitCount, 1u);
    EXPECT_EQ(batch.GetStats().CommittedBytes, 37u);
}

TEST(MappedUploadPageTest, ZeroCommitProducesNoRange) {
    FakeDevice device;
    HostWriteBatch batch;
    auto arena = MakeArena(device, batch);

    auto reservation = arena.Reserve(64);
    const auto allocation = reservation.Commit(0);
    EXPECT_TRUE(allocation.IsValid());
    EXPECT_EQ(allocation.Size, 0u);
    EXPECT_TRUE(batch.Empty());
    EXPECT_EQ(batch.GetStats().CommitCount, 1u);
    EXPECT_EQ(batch.GetStats().CommittedBytes, 0u);
}

#ifdef RADRAY_IS_DEBUG
TEST(MappedUploadPageTest, DestroyingUncommittedReservationFails) {
    EXPECT_DEATH(
        {
            FakeDevice device;
            HostWriteBatch batch;
            auto arena = MakeArena(device, batch);
            auto reservation = arena.Reserve(16);
            (void)reservation;
        },
        "");
}

TEST(MappedUploadPageTest, ZeroCommitAfterSealFails) {
    EXPECT_DEATH(
        {
            FakeDevice device;
            HostWriteBatch batch;
            auto arena = MakeArena(device, batch);
            auto reservation = arena.Reserve(16);
            batch.Seal();
            reservation.Commit(0);
        },
        "");
}
#endif

TEST(DynamicCBufferArenaTest, CoalescesAdjacentCommitsAndKeepsSparseWritesSeparate) {
    FakeDevice device;
    HostWriteBatch batch;
    auto arena = MakeArena(device, batch, 1024, 1);

    auto firstReservation = arena.Reserve(32);
    auto secondReservation = arena.Reserve(32);
    const auto first = firstReservation.Commit(32);
    const auto second = secondReservation.Commit(32);
    ASSERT_EQ(first.Target, second.Target);
    ASSERT_EQ(batch.GetRanges().size(), 1u);
    EXPECT_EQ(batch.GetRanges()[0].Range.Offset, 0u);
    EXPECT_EQ(batch.GetRanges()[0].Range.Size, 64u);

    batch.Reset();
    auto sparseArena = MakeArena(device, batch, 1024, 256);
    auto sparseFirstReservation = sparseArena.Reserve(100);
    auto sparseSecondReservation = sparseArena.Reserve(100);
    sparseFirstReservation.Commit(100);
    sparseSecondReservation.Commit(100);
    ASSERT_EQ(batch.GetRanges().size(), 2u);
    EXPECT_EQ(batch.GetRanges()[0].Range.Offset, 0u);
    EXPECT_EQ(batch.GetRanges()[1].Range.Offset, 256u);

    ASSERT_GE(device.BufferDescriptors.size(), 2u);
    EXPECT_TRUE(device.BufferDescriptors[0].Hints.HasFlag(render::ResourceHint::PersistentMap));
    EXPECT_FALSE(device.BufferDescriptors[0].Hints.HasFlag(render::ResourceHint::Dedicated));
}

TEST(DynamicCBufferArenaTest, ReusesRetainedOverflowPagesAcrossResets) {
    FakeDevice device;
    HostWriteBatch batch;
    auto arena = MakeArena(device, batch, 512, 256, 512);

    auto firstReservation = arena.Reserve(100);
    auto secondReservation = arena.Reserve(100);
    auto overflowReservation = arena.Reserve(100);
    const auto first = firstReservation.Commit(100);
    const auto second = secondReservation.Commit(100);
    const auto overflow = overflowReservation.Commit(100);
    ASSERT_EQ(first.Target, second.Target);
    ASSERT_NE(first.Target, overflow.Target);

    batch.Reset();
    arena.Reset();
    auto reusedFirstReservation = arena.Reserve(100);
    auto reusedSecondReservation = arena.Reserve(100);
    auto reusedOverflowReservation = arena.Reserve(100);
    const auto reusedFirst = reusedFirstReservation.Commit(100);
    const auto reusedSecond = reusedSecondReservation.Commit(100);
    const auto reusedOverflow = reusedOverflowReservation.Commit(100);
    EXPECT_EQ(reusedFirst.Target, first.Target);
    EXPECT_EQ(reusedSecond.Target, second.Target);
    EXPECT_EQ(reusedOverflow.Target, overflow.Target);
    EXPECT_EQ(device.BufferDescriptors.size(), 2u);
    EXPECT_EQ(device.DestroyedBufferCount, 0u);
}
