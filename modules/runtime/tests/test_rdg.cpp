#include <gtest/gtest.h>

#include <radray/logger.h>

#include <algorithm>
#include <cstring>
#include <cstdio>

#include <radray/runtime/rdg.h>
#include <radray/runtime/render_graph_pipeline.h>
#include <radray/runtime/scene.h>
#include <radray/runtime/geometry_buffer_pool.h>
#include <radray/runtime/gpu_resource.h>

#include "test_fakes.h"

using namespace radray;
using namespace radray::test;
using namespace radray::render;

namespace {

class ContainerProbe final : public std::pmr::memory_resource {
public:
    explicit ContainerProbe(std::pmr::memory_resource& upstream) noexcept : _upstream(upstream) {}
    size_t LiveBytes{0};

private:
    void* do_allocate(size_t bytes, size_t alignment) override {
        auto* result = _upstream.allocate(bytes, alignment);
        LiveBytes += bytes;
        return result;
    }
    void do_deallocate(void* storage, size_t bytes, size_t alignment) override {
        EXPECT_GE(LiveBytes, bytes);
        LiveBytes -= bytes;
        _upstream.deallocate(storage, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
    std::pmr::memory_resource& _upstream;
};

struct Probe {
    struct RasterScope {
        vector<RenderPassColorAttachment> Colors;
        std::optional<RenderPassDepthStencilAttachment> Depth;
        bool AllowUavWrites;
    };
    vector<ResourceBarrierDescriptor> Barriers;
    vector<size_t> BarrierBatches;
    vector<RasterScope> RasterScopes;
    uint32_t ComputeScopes{0};
    vector<string> DebugGroups;
    uint32_t DebugDepth{0};
    vector<string> Executed;
    vector<BufferDescriptor> BufferAllocations;
    vector<TextureDescriptor> TextureAllocations;
    vector<TextureViewDescriptor> Views;
    uint32_t Maps{0}, Flushes{0};
    uint32_t Copies{0};
    vector<TextureRegion2D> TextureCopies;
    int32_t TextureCopiesUntilFailure{-1};
    uint32_t OpenEncoders{0};
};

class ByteBuffer final : public FakeBuffer {
public:
    ByteBuffer(BufferDescriptor desc, Probe& probe) : FakeBuffer(desc.Size, desc.Usage), Bytes(desc.Size), _probe(probe) { Desc = desc; }
    void* Map(uint64_t offset, uint64_t size) noexcept override {
        ++_probe.Maps;
        return offset <= Bytes.size() && size <= Bytes.size() - offset ? Bytes.data() + offset : nullptr;
    }
    void FlushMappedRange(BufferRange) noexcept override { ++_probe.Flushes; }
    vector<byte> Bytes;

private:
    Probe& _probe;
};

class ProbeTexture final : public Texture {
public:
    explicit ProbeTexture(TextureDescriptor desc) noexcept : Desc(desc) {}
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}
    TextureDescriptor GetDesc() const noexcept override { return Desc; }
    Device* GetDevice() const noexcept override { return nullptr; }
    TextureDescriptor Desc;
};

class ProbeView final : public TextureView {
public:
    explicit ProbeView(TextureViewDescriptor desc) noexcept : Desc(desc) {}
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}
    TextureViewDescriptor GetDesc() const noexcept override { return Desc; }
    TextureViewDescriptor Desc;
};

class ProbeComputeEncoder final : public ComputeCommandEncoder {
public:
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    CommandBuffer* GetCommandBuffer() const noexcept override { return nullptr; }
    void BindShaderParameterSet(uint32_t, ShaderParameterSet*, std::span<const ShaderParameterDynamicOffset>) noexcept override {}
    bool SetPushConstants(uint32_t, uint32_t, std::span<const byte>) noexcept override { return true; }
    void BindComputePipelineState(ComputePipelineState*) noexcept override {}
    void Dispatch(uint32_t, uint32_t, uint32_t) noexcept override {}
    void DispatchIndirect(Buffer*, uint64_t) noexcept override {}
};

class ProbeCommand final : public FakeCommandBuffer {
public:
    explicit ProbeCommand(Probe& probe) noexcept : _probe(probe) {}
    void ResourceBarrier(std::span<const ResourceBarrierDescriptor> barriers) noexcept override {
        EXPECT_EQ(_probe.OpenEncoders, 0);
        _probe.BarrierBatches.push_back(barriers.size());
        _probe.Barriers.insert(_probe.Barriers.end(), barriers.begin(), barriers.end());
    }
    void PushDebugGroup(std::string_view name) noexcept override {
        _probe.DebugGroups.emplace_back(name);
        ++_probe.DebugDepth;
    }
    void PopDebugGroup() noexcept override {
        ASSERT_GT(_probe.DebugDepth, 0u);
        --_probe.DebugDepth;
    }
    Nullable<unique_ptr<GraphicsCommandEncoder>> BeginRenderPass(const RenderPassBeginDescriptor& desc) noexcept override {
        EXPECT_EQ(_probe.OpenEncoders, 0);
        _probe.RasterScopes.push_back({{desc.Colors.begin(), desc.Colors.end()}, desc.DepthStencil, desc.AllowUavWrites});
        ++_probe.OpenEncoders;
        return make_unique<RecordingEncoder>();
    }
    void EndRenderPass(unique_ptr<GraphicsCommandEncoder>) noexcept override { --_probe.OpenEncoders; }
    Nullable<unique_ptr<ComputeCommandEncoder>> BeginComputePass() noexcept override {
        EXPECT_EQ(_probe.OpenEncoders, 0);
        ++_probe.ComputeScopes;
        ++_probe.OpenEncoders;
        return make_unique<ProbeComputeEncoder>();
    }
    void EndComputePass(unique_ptr<ComputeCommandEncoder>) noexcept override { --_probe.OpenEncoders; }
    void CopyBufferToBuffer(Buffer* dst, uint64_t dstOffset, Buffer* src, uint64_t srcOffset, uint64_t size) noexcept override {
        ++_probe.Copies;
        std::memcpy(static_cast<ByteBuffer*>(dst)->Bytes.data() + dstOffset, static_cast<ByteBuffer*>(src)->Bytes.data() + srcOffset, size);
    }
    bool CopyBufferToTextureRegion(const BufferToTextureCopyDescriptor& desc) noexcept override {
        EXPECT_EQ(_probe.OpenEncoders, 0u);
        EXPECT_GT(_probe.DebugDepth, 0u);
        if (_probe.TextureCopiesUntilFailure == 0) return false;
        if (_probe.TextureCopiesUntilFailure > 0) --_probe.TextureCopiesUntilFailure;
        _probe.TextureCopies.push_back(desc.Region.Destination);
        return true;
    }

private:
    Probe& _probe;
};

class ProbeDevice final : public FakeDevice {
public:
    explicit ProbeDevice(Probe& probe) noexcept : _probe(probe) {}
    Nullable<unique_ptr<CommandBuffer>> CreateCommandBuffer(CommandAllocator*) noexcept override { return make_unique<ProbeCommand>(_probe); }
    Nullable<unique_ptr<Buffer>> CreateBuffer(const BufferDescriptor& desc) noexcept override {
        _probe.BufferAllocations.push_back(desc);
        if (FailAllocations) return nullptr;
        return make_unique<ByteBuffer>(desc, _probe);
    }
    Nullable<unique_ptr<Texture>> CreateTexture(const TextureDescriptor& desc) noexcept override {
        _probe.TextureAllocations.push_back(desc);
        if (FailAllocations) return nullptr;
        return make_unique<ProbeTexture>(desc);
    }
    Nullable<unique_ptr<TextureView>> CreateTextureView(const TextureViewDescriptor& desc) noexcept override {
        _probe.Views.push_back(desc);
        return make_unique<ProbeView>(desc);
    }
    bool FailAllocations{false};

private:
    Probe& _probe;
};

TextureDescriptor ColorDescriptor() {
    return {.Dim = TextureDimension::Dim2D, .Width = 32, .Height = 16, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = TextureFormat::RGBA8_UNORM, .Usage = TextureUse::RenderTarget | TextureUse::UnorderedAccess | TextureUse::Resource | TextureUse::CopySource};
}

class RDGExecutionTest : public testing::Test {
protected:
    void SetUp() override {
        auto pipeline = make_unique<CallbackPipeline>();
        pipeline->OnExtract = [&](FrameSlot& slot) {
            Recorded = Prepared = false;
            const auto buffers = Trace.BufferAllocations.size();
            const auto textures = Trace.TextureAllocations.size();
            RDGBuilder graph{slot};
            Setup(graph);
            Compiled = graph.Compile();
            ASSERT_TRUE(Compiled);
            EXPECT_EQ(Trace.BufferAllocations.size(), buffers);
            EXPECT_EQ(Trace.TextureAllocations.size(), textures);
            if (AfterCompile) AfterCompile();
        };
        pipeline->OnPrepare = [&](FrameSlot& slot) {
            if (!Compiled) return;
            Pool.BeginFrame();
            Uploads.Reset();
            Prepared = Compiled->Prepare(slot, Pool, Uploads);
            if (!Prepared) EXPECT_FALSE(ExpectSuccess);
            if (AfterPrepare) AfterPrepare();
            if (Prepared) {
                PreparedCommand = slot.AcquireCommandBuffer();
                ASSERT_TRUE(PreparedCommand);
            }
        };
        pipeline->OnRecord = [&](FrameSlot&) {
            if (!Prepared) return;
            auto command = PreparedCommand;
            command->Begin();
            const auto maps = Trace.Maps, flushes = Trace.Flushes;
            const auto buffers = Trace.BufferAllocations.size(), views = Trace.Views.size();
            Recorded = Compiled->Record(*command);
            EXPECT_EQ(Trace.Maps, maps);
            EXPECT_EQ(Trace.Flushes, flushes);
            EXPECT_EQ(Trace.BufferAllocations.size(), buffers);
            EXPECT_EQ(Trace.Views.size(), views);
            if (AfterRecord) AfterRecord();
            EXPECT_EQ(Recorded, ExpectSuccess);
            command->End();
        };
        pipeline->OnSubmit = [&](FrameSlot& slot) {
            if (Recorded) {
                auto* command = PreparedCommand.Get();
                slot.GetQueue().Submit({.CmdBuffers = std::span{&command, 1}});
                Compiled->CommitSubmittedStates();
            }
        };
        auto scheduler = FrameScheduler::Create(Context, std::move(pipeline), 1);
        ASSERT_TRUE(scheduler);
        Scheduler = scheduler.Release();
    }
    void TearDown() override {
        Scheduler.reset();
        EXPECT_EQ(Trace.OpenEncoders, 0);
        EXPECT_EQ(Trace.DebugDepth, 0);
    }
    void Run() { Scheduler->RunFrame(); }
    Probe Trace;
    shared_ptr<ProbeDevice> Device{make_shared<ProbeDevice>(Trace)};
    DeviceContext Context{Device, &Device->Queue};
    RDGTransientResourcePool Pool{*Device};
    UploadArena Uploads{*Device, 64};
    bool Prepared{false}, Recorded{false};
    Nullable<CommandBuffer*> PreparedCommand{nullptr};
    Scene Source;
    unique_ptr<FrameScheduler> Scheduler;
    std::function<void(RDGBuilder&)> Setup;
    std::function<void()> AfterCompile, AfterPrepare, AfterRecord;
    Nullable<CompiledRDG*> Compiled{nullptr};
    bool ExpectSuccess{true};
};

TEST_F(RDGExecutionTest, MergesRasterAttachmentsFoldsActionsAndHoistsLaterConstants) {
    ByteBuffer ubo0{{.Size = 16, .Memory = MemoryType::Upload, .Usage = BufferUse::MapWrite | BufferUse::CBuffer}, Trace};
    ByteBuffer ubo1{ubo0.Desc, Trace};
    Setup = [&](RDGBuilder& graph) {
        const auto constants0 = graph.ImportBuffer(&ubo0, BufferState::HostWrite, BufferState::HostWrite);
        const auto constants1 = graph.ImportBuffer(&ubo1, BufferState::HostWrite, BufferState::HostWrite);
        const auto color = graph.CreateTexture(ColorDescriptor());
        auto depthDesc = ColorDescriptor();
        depthDesc.Format = TextureFormat::D32_FLOAT_S8_UINT;
        depthDesc.Usage = TextureUse::DepthStencilWrite;
        const auto depth = graph.CreateTexture(depthDesc);
        for (uint32_t i = 0; i < 3; ++i) {
            {
                struct PassData {
                    std::remove_cvref_t<decltype(i)> I;
                    decltype(&Trace) TraceRef;
                };
                auto pass = graph.AddRasterPass(radray::format("raster{}", i), PassData{i, &Trace}, +[](const PassData& data, RDGRasterPassContext& ctx) noexcept {
            const auto& i = data.I; auto& Trace = *data.TraceRef;
                auto& encoder = static_cast<RecordingEncoder&>(ctx.GetEncoder());
                EXPECT_FALSE(encoder.Stencils.empty());
                if (!encoder.Stencils.empty()) EXPECT_EQ(encoder.Stencils.back(), 0);
                encoder.SetStencilReference(7);
                EXPECT_EQ(Trace.BarrierBatches, (vector<size_t>{4}));
                Trace.Executed.emplace_back(radray::format("raster{}", i));
                return true; });

                const auto load = i == 0 ? LoadAction::Clear : LoadAction::Load;
                const auto store = i == 2 ? StoreAction::Discard : StoreAction::Store;
                pass.WriteColor(0, color, load, store, {float(i) * 0.25f, 0.5f, 0.75f, 1.0f})
                    .SetDepthStencil(depth, TextureState::DepthWrite, load, store, load, store, {0.5f + float(i) * 0.1f, uint8_t(i + 3)})
                    .ReadBuffer(i == 1 ? constants1 : constants0, BufferState::CBuffer);
            };
        }
        graph.MarkOutput(color);
    };
    Run();
    EXPECT_EQ(Trace.Executed, (vector<string>{"raster0", "raster1", "raster2"}));
    EXPECT_EQ(Trace.DebugGroups, Trace.Executed);
    ASSERT_EQ(Trace.RasterScopes.size(), 1);
    const auto& scope = Trace.RasterScopes.front();
    EXPECT_EQ(scope.Colors[0].Load, LoadAction::Clear);
    EXPECT_EQ(scope.Colors[0].Store, StoreAction::Discard);
    EXPECT_EQ(scope.Colors[0].Clear.Value, (array<float, 4>{0.0f, 0.5f, 0.75f, 1.0f}));
    ASSERT_TRUE(scope.Depth);
    EXPECT_EQ(scope.Depth->DepthLoad, LoadAction::Clear);
    EXPECT_EQ(scope.Depth->DepthStore, StoreAction::Discard);
    EXPECT_EQ(scope.Depth->StencilLoad, LoadAction::Clear);
    EXPECT_EQ(scope.Depth->StencilStore, StoreAction::Discard);
    EXPECT_EQ(scope.Depth->Clear.Depth, 0.5f);
    EXPECT_EQ(scope.Depth->Clear.Stencil, 3);
    EXPECT_EQ(Trace.BarrierBatches, (vector<size_t>{4, 2}));
}

TEST(RDG, CullsDeadMeshPassesAndKeepsFirstDeclarationOrder) {
    FakeGpu gpu;
    auto scheduler = FrameScheduler::Create(gpu.Context, make_unique<CallbackPipeline>(), 1).Unwrap();
    RDGBuilder graph{scheduler->BeginFrame()};
    const auto live = graph.CreateTexture(ColorDescriptor());
    const auto dead = graph.CreateTexture(ColorDescriptor());
    {
        struct PassData {};
        auto pass = graph.AddRasterPass("dead", PassData{}, +[](const PassData&, RDGRasterPassContext&) noexcept { return true; });
        pass.UseMeshPass({2}).WriteColor(0, dead, LoadAction::Clear, StoreAction::Store);
    };
    {
        struct PassData {};
        auto pass = graph.AddRasterPass("live", PassData{}, +[](const PassData&, RDGRasterPassContext&) noexcept { return true; });
        pass.UseMeshPass({3}).UseMeshPass({1}).UseMeshPass({3}).WriteColor(0, live, LoadAction::Clear, StoreAction::Store);
    };
    graph.MarkOutput(live);

    const auto compiled = graph.Compile();
    ASSERT_TRUE(compiled);
    const auto passes = compiled->GetAliveMeshPasses();
    ASSERT_EQ(passes.size(), 2);
    EXPECT_EQ(passes[0], MeshPassId{3});
    EXPECT_EQ(passes[1], MeshPassId{1});
}

TEST(RDG, RejectsOverlappingDirectUploads) {
    FakeBuffer buffer{16, BufferUse::MapWrite | BufferUse::Vertex};
    buffer.Desc.Memory = MemoryType::Upload;
    FakeGpu gpu;
    auto scheduler = FrameScheduler::Create(gpu.Context, make_unique<CallbackPipeline>(), 1).Unwrap();
    RDGBuilder graph{scheduler->BeginFrame()};
    const auto dst = graph.ImportBuffer(&buffer, BufferState::HostWrite);
    const array<byte, 8> bytes{};
    graph.UploadBuffer(dst, 0, RDGUploadSource::SnapshotBytes(bytes));
    graph.UploadBuffer(dst, 4, RDGUploadSource::SnapshotBytes(bytes));
    graph.MarkOutput(dst);

    EXPECT_FALSE(graph.Compile());
}

TEST(RDG, RejectsAttachmentViewOfAnotherResource) {
    ProbeTexture texture{ColorDescriptor()};
    ProbeTexture other{ColorDescriptor()};
    ProbeView view{{&other, TextureDimension::Dim2D, TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, TextureViewUsage::RenderTarget}};
    FakeGpu gpu;
    auto scheduler = FrameScheduler::Create(gpu.Context, make_unique<CallbackPipeline>(), 1).Unwrap();
    RDGBuilder graph{scheduler->BeginFrame()};
    const auto color = graph.ImportTexture(&texture, TextureState::Undefined);
    {
        struct PassData {};
        auto pass = graph.AddRasterPass("invalid view", PassData{}, +[](const PassData&, RDGRasterPassContext&) noexcept { return true; });
        pass.WriteColor(0, color, LoadAction::Clear, StoreAction::Store, {}, &view).NeverCull();
    };
    EXPECT_FALSE(graph.Compile());
}

TEST(RDG, RejectsIncompatibleAttachmentDimensions) {
    FakeGpu gpu;
    auto scheduler = FrameScheduler::Create(gpu.Context, make_unique<CallbackPipeline>(), 1).Unwrap();
    RDGBuilder graph{scheduler->BeginFrame()};
    const auto a = graph.CreateTexture(ColorDescriptor());
    auto smaller = ColorDescriptor();
    smaller.Width /= 2;
    const auto b = graph.CreateTexture(smaller);
    {
        struct PassData {};
        auto pass = graph.AddRasterPass("invalid extent", PassData{}, +[](const PassData&, RDGRasterPassContext&) noexcept { return true; });
        pass.WriteColor(0, a, LoadAction::Clear, StoreAction::Store).WriteColor(1, b, LoadAction::Clear, StoreAction::Store).NeverCull();
    };
    EXPECT_FALSE(graph.Compile());
}

TEST(RDG, RejectsClearOfReadOnlyDepthAttachment) {
    auto descriptor = ColorDescriptor();
    descriptor.Format = TextureFormat::D32_FLOAT;
    descriptor.Usage = TextureUse::DepthStencilRead;
    ProbeTexture texture{descriptor};
    FakeGpu gpu;
    auto scheduler = FrameScheduler::Create(gpu.Context, make_unique<CallbackPipeline>(), 1).Unwrap();
    RDGBuilder graph{scheduler->BeginFrame()};
    const auto depth = graph.ImportTexture(&texture, TextureState::DepthRead);
    {
        struct PassData {};
        auto pass = graph.AddRasterPass("read-only clear", PassData{}, +[](const PassData&, RDGRasterPassContext&) noexcept { return true; });
        pass.SetDepthStencil(depth, TextureState::DepthRead, LoadAction::Clear, StoreAction::Store).NeverCull();
    };
    EXPECT_FALSE(graph.Compile());
}

TEST_F(RDGExecutionTest, DeadPassesDoNotExecuteAllocateOrUpload) {
    ByteBuffer unused{{.Size = 16, .Memory = MemoryType::Upload, .Usage = BufferUse::MapWrite | BufferUse::CBuffer}, Trace};
    Setup = [&](RDGBuilder& graph) {
        const auto buffer = graph.ImportBuffer(&unused, BufferState::HostWrite, BufferState::HostWrite);
        const array<byte, 16> bytes{};
        graph.UploadBuffer(buffer, 0, RDGUploadSource::SnapshotBytes(bytes));
        const auto color = graph.CreateTexture(ColorDescriptor());
        graph.CreateBuffer({.Size = 128, .Usage = BufferUse::UnorderedAccess});
        {
            struct PassData {
                decltype(&Trace) TraceRef;
            };
            auto pass = graph.AddRasterPass("unused", PassData{&Trace}, +[](const PassData& data, RDGRasterPassContext&) noexcept {
            auto& Trace = *data.TraceRef; Trace.Executed.push_back("dead"); return true; });
            pass.UseMeshPass({5}).ReadBuffer(buffer, BufferState::CBuffer).WriteColor(0, color, LoadAction::Clear, StoreAction::Store);
        };
    };
    Run();
    EXPECT_TRUE(Trace.Executed.empty());
    EXPECT_TRUE(Trace.TextureAllocations.empty());
    EXPECT_TRUE(Trace.BufferAllocations.empty());
    EXPECT_TRUE(Trace.Views.empty());
    EXPECT_TRUE(Trace.Barriers.empty());
    EXPECT_EQ(Trace.Maps, 0);
}

TEST_F(RDGExecutionTest, SegmentedDeviceUploadsRetainCopiedBytesAndFinalStateWithoutDraws) {
    ByteBuffer target{{.Size = 8, .Usage = BufferUse::CopyDestination | BufferUse::Vertex}, Trace};
    Setup = [&](RDGBuilder& graph) {
        const auto buffer = graph.ImportBuffer(&target, BufferState::Undefined, BufferState::Vertex);
        array bytes{byte{1}, byte{2}, byte{3}, byte{4}};
        graph.UploadBuffer(buffer, 0, RDGUploadSource::SnapshotBytes(bytes));
        bytes = {byte{5}, byte{6}, byte{7}, byte{8}};
        graph.UploadBuffer(buffer, 4, RDGUploadSource::SnapshotBytes(bytes));
        bytes.fill(byte{0});
        graph.MarkOutput(buffer);
    };
    Run();
    EXPECT_EQ(target.Bytes, (vector<byte>{byte{1}, byte{2}, byte{3}, byte{4}, byte{5}, byte{6}, byte{7}, byte{8}}));
    EXPECT_EQ(Trace.Copies, 2);
    const auto* epilogue = std::get_if<BarrierBufferDescriptor>(&Trace.Barriers.back());
    ASSERT_NE(epilogue, nullptr);
    EXPECT_EQ(epilogue->Target, &target);
    EXPECT_EQ(epilogue->Before, BufferStates{BufferState::CopyDestination});
    EXPECT_EQ(epilogue->After, BufferStates{BufferState::Vertex});
}

TEST_F(RDGExecutionTest, DisjointDirectUploadsAreChainedBeforeGpuReads) {
    ByteBuffer target{{.Size = 8, .Memory = MemoryType::Upload, .Usage = BufferUse::MapWrite | BufferUse::Resource}, Trace};
    Setup = [&](RDGBuilder& graph) {
        const auto buffer = graph.ImportBuffer(&target, BufferState::HostWrite, BufferState::HostWrite);
        const array bytes{byte{7}, byte{8}, byte{9}, byte{10}};
        graph.UploadBuffer(buffer, 0, RDGUploadSource::SnapshotBytes(bytes));
        graph.UploadBuffer(buffer, 4, RDGUploadSource::SnapshotBytes(bytes));
        {
            struct PassData {
                std::remove_cvref_t<decltype(buffer)> Buffer;
                decltype(&target) TargetRef;
            };
            auto pass = graph.AddComputePass("read", PassData{buffer, &target}, +[](const PassData& data, RDGComputePassContext& ctx) noexcept {
            const auto& buffer = data.Buffer; auto& target = *data.TargetRef;
                                 EXPECT_EQ(ctx.ResolveBuffer(buffer).Buffer.Get(), &target);
                                 EXPECT_EQ(target.Bytes[7], byte{10});
                                 return true; });
            pass.ReadBuffer(buffer).NeverCull();
        };
    };
    Run();
    EXPECT_EQ(Trace.Maps, 2);
    EXPECT_EQ(Trace.Copies, 0);
    EXPECT_TRUE(Trace.BufferAllocations.empty());
    const auto* epilogue = std::get_if<BarrierBufferDescriptor>(&Trace.Barriers.back());
    ASSERT_NE(epilogue, nullptr);
    EXPECT_EQ(epilogue->After, BufferStates{BufferState::HostWrite});
}

TEST(RDG, RejectsDirectUploadAfterGpuReadEvenForDisjointRanges) {
    FakeBuffer target{16, BufferUse::MapWrite | BufferUse::Resource};
    target.Desc.Memory = MemoryType::Upload;
    FakeGpu gpu;
    auto scheduler = FrameScheduler::Create(gpu.Context, make_unique<CallbackPipeline>(), 1).Unwrap();
    RDGBuilder graph{scheduler->BeginFrame()};
    const auto buffer = graph.ImportBuffer(&target, BufferState::HostWrite);
    const array<byte, 4> bytes{};
    graph.UploadBuffer(buffer, 0, RDGUploadSource::SnapshotBytes(bytes));
    {
        struct PassData {};
        auto pass = graph.AddComputePass("read", PassData{}, +[](const PassData&, RDGComputePassContext&) noexcept { return true; });
        pass.ReadBuffer(buffer).NeverCull();
    };
    graph.UploadBuffer(buffer, 4, RDGUploadSource::SnapshotBytes(bytes));
    graph.MarkOutput(buffer);
    EXPECT_FALSE(graph.Compile());
}

TEST_F(RDGExecutionTest, DeviceBufferCanBeReuploadedBetweenGpuReads) {
    ByteBuffer target{{.Size = 4, .Usage = BufferUse::CopyDestination | BufferUse::Resource}, Trace};
    Setup = [&](RDGBuilder& graph) {
        const auto buffer = graph.ImportBuffer(&target, BufferState::Undefined);
        const array bytes{byte{1}, byte{2}, byte{3}, byte{4}};
        graph.UploadBuffer(buffer, 0, RDGUploadSource::SnapshotBytes(bytes));
        {
            struct PassData {};
            auto pass = graph.AddComputePass("first read", PassData{}, +[](const PassData&, RDGComputePassContext&) noexcept { return true; });
            pass.ReadBuffer(buffer).NeverCull();
        };
        graph.UploadBuffer(buffer, 0, RDGUploadSource::SnapshotBytes(bytes));
        {
            struct PassData {};
            auto pass = graph.AddComputePass("second read", PassData{}, +[](const PassData&, RDGComputePassContext&) noexcept { return true; });
            pass.ReadBuffer(buffer).NeverCull();
        };
    };
    Run();
    EXPECT_EQ(Trace.Copies, 2);
}

TEST_F(RDGExecutionTest, InPlaceComputeKeepsUpstreamRasterAndResolvesUavView) {
    Setup = [&](RDGBuilder& graph) {
        const auto color = graph.CreateTexture(ColorDescriptor());
        {
            struct PassData {
                decltype(&Trace) TraceRef;
            };
            auto pass = graph.AddRasterPass("raster", PassData{&Trace}, +[](const PassData& data, RDGRasterPassContext&) noexcept {
            auto& Trace = *data.TraceRef; Trace.Executed.push_back("raster"); return true; });
            pass.WriteColor(0, color, LoadAction::Clear, StoreAction::Store);
        };
        {
            struct PassData {
                std::remove_cvref_t<decltype(color)> Color;
                std::remove_cvref_t<decltype(make_unique<uint32_t>(9))> Owner;
                decltype(&Trace) TraceRef;
            };
            auto pass = graph.AddComputePass("post", PassData{color, make_unique<uint32_t>(9), &Trace}, +[](const PassData& data, RDGComputePassContext& ctx) noexcept {
            const auto& color = data.Color; const auto& owner = data.Owner; auto& Trace = *data.TraceRef;
                                 EXPECT_EQ(*owner, 9);
                                 EXPECT_TRUE(ctx.ResolveView(color, TextureViewUsage::UnorderedAccess));
                                 EXPECT_FALSE(ctx.ResolveView(color, TextureViewUsage::Resource));
                                 Trace.Executed.push_back("compute");
                                 return true; });
            pass.ReadWriteTexture(color);
        };
        {
            struct PassData {
                std::remove_cvref_t<decltype(color)> Color;
                decltype(&Trace) TraceRef;
            };
            auto pass = graph.AddCopyPass("readback", PassData{color, &Trace}, +[](const PassData& data, RDGCopyPassContext& ctx) noexcept {
            const auto& color = data.Color; auto& Trace = *data.TraceRef;
                              EXPECT_EQ(ctx.Resolve(color).GetDesc().Width, 32);
                              Trace.Executed.push_back("copy");
                              return true; });
            pass.ReadTexture(color).NeverCull();
        };
    };
    Run();
    EXPECT_EQ(Trace.Executed, (vector<string>{"raster", "compute", "copy"}));
    ASSERT_EQ(Trace.Barriers.size(), 3);
    EXPECT_EQ(std::get<BarrierTextureDescriptor>(Trace.Barriers[1]).After, TextureStates{TextureState::UnorderedAccess});
    EXPECT_EQ(std::get<BarrierTextureDescriptor>(Trace.Barriers[2]).After, TextureStates{TextureState::CopySource});
}

TEST_F(RDGExecutionTest, OverwritingColorClearCullsPreviousWriterAndBridgesStates) {
    Setup = [&](RDGBuilder& graph) {
        const auto color = graph.CreateTexture(ColorDescriptor());
        {
            struct PassData {
                decltype(&Trace) TraceRef;
            };
            auto pass = graph.AddComputePass("dead writer", PassData{&Trace}, +[](const PassData& data, RDGComputePassContext&) noexcept {
            auto& Trace = *data.TraceRef; Trace.Executed.push_back("dead"); return true; });
            pass.WriteTexture(color);
        };
        {
            struct PassData {
                decltype(&Trace) TraceRef;
            };
            auto pass = graph.AddRasterPass("clear", PassData{&Trace}, +[](const PassData& data, RDGRasterPassContext&) noexcept {
            auto& Trace = *data.TraceRef; Trace.Executed.push_back("clear"); return true; });
            pass.WriteColor(0, color, LoadAction::Clear, StoreAction::Store);
        };
        graph.MarkOutput(color);
    };
    Run();
    EXPECT_EQ(Trace.Executed, (vector<string>{"clear"}));
    ASSERT_EQ(Trace.Barriers.size(), 1);
    const auto& first = std::get<BarrierTextureDescriptor>(Trace.Barriers[0]);
    EXPECT_EQ(first.Before, TextureStates{TextureState::Undefined});
    EXPECT_EQ(first.After, TextureStates{TextureState::RenderTarget});
}

TEST_F(RDGExecutionTest, MergedAttachmentsReuseRealFinalStateOnNextFrame) {
    Setup = [&](RDGBuilder& graph) {
        const auto color = graph.CreateTexture(ColorDescriptor());
        auto depthDesc = ColorDescriptor();
        depthDesc.Format = TextureFormat::D32_FLOAT_S8_UINT;
        depthDesc.Usage = TextureUse::DepthStencilWrite;
        const auto depth = graph.CreateTexture(depthDesc);
        for (uint32_t i = 0; i < 2; ++i) {
            {
                struct PassData {};
                auto pass = graph.AddRasterPass("attachments", PassData{}, +[](const PassData&, RDGRasterPassContext&) noexcept { return true; });

                const auto load = i == 0 ? LoadAction::Clear : LoadAction::Load;
                pass.WriteColor(0, color, load, StoreAction::Store)
                    .SetDepthStencil(depth, TextureState::DepthWrite, load, StoreAction::Store, load, StoreAction::Store);
            };
        }
        graph.MarkOutput(color);
    };
    Run();
    ASSERT_EQ(Trace.TextureAllocations.size(), 2);
    ASSERT_EQ(Trace.Barriers.size(), 2);
    EXPECT_EQ(Trace.RasterScopes.size(), 1);
    Trace.Barriers.clear();
    Run();
    EXPECT_EQ(Trace.TextureAllocations.size(), 2);
    EXPECT_EQ(Trace.Views.size(), 2);
    ASSERT_EQ(Trace.Barriers.size(), 2);
    EXPECT_EQ(std::get<BarrierTextureDescriptor>(Trace.Barriers[0]).Before, TextureStates{TextureState::RenderTarget});
    EXPECT_EQ(std::get<BarrierTextureDescriptor>(Trace.Barriers[1]).Before, TextureStates{TextureState::DepthWrite});
    EXPECT_EQ(std::get<BarrierTextureDescriptor>(Trace.Barriers[0]).After, TextureStates{TextureState::RenderTarget});
    EXPECT_EQ(std::get<BarrierTextureDescriptor>(Trace.Barriers[1]).After, TextureStates{TextureState::DepthWrite});
}

TEST_F(RDGExecutionTest, UavReadWriteReadReadSynchronizesOnlyBeforeAndAfterWrite) {
    ByteBuffer target{{.Size = 16, .Usage = BufferUse::Resource | BufferUse::UnorderedAccess}, Trace};
    ProbeTexture texture{ColorDescriptor()};
    Setup = [&](RDGBuilder& graph) {
        const auto buffer = graph.ImportBuffer(&target, BufferState::ShaderRead);
        const auto color = graph.ImportTexture(&texture, TextureState::ShaderRead);
        for (uint32_t i = 0; i < 4; ++i) {
            {
                struct PassData {};
                auto pass = graph.AddComputePass("uav sequence", PassData{}, +[](const PassData&, RDGComputePassContext&) noexcept { return true; });

                if (i == 1)
                    pass.WriteBuffer(buffer).WriteTexture(color);
                else
                    pass.ReadBuffer(buffer, BufferState::UnorderedAccess).ReadTexture(color, TextureState::UnorderedAccess);
                pass.NeverCull();
            };
        }
    };
    Run();
    ASSERT_EQ(Trace.Barriers.size(), 6);
    EXPECT_TRUE(std::holds_alternative<BarrierBufferDescriptor>(Trace.Barriers[0]));
    EXPECT_TRUE(std::holds_alternative<BarrierTextureDescriptor>(Trace.Barriers[1]));
    for (size_t i = 2; i < Trace.Barriers.size(); ++i) EXPECT_TRUE(std::holds_alternative<BarrierUavDescriptor>(Trace.Barriers[i]));
}

TEST_F(RDGExecutionTest, ImportedOutputsGetEpilogueEvenWithoutPasses) {
    ProbeTexture texture{ColorDescriptor()};
    Setup = [&](RDGBuilder& graph) {
        graph.MarkOutput(graph.ImportTexture(&texture, TextureState::RenderTarget, TextureState::Present));
    };
    Run();
    ASSERT_EQ(Trace.Barriers.size(), 1);
    const auto barrier = std::get<BarrierTextureDescriptor>(Trace.Barriers[0]);
    EXPECT_EQ(barrier.Target, &texture);
    EXPECT_EQ(barrier.After, TextureStates{TextureState::Present});
}

TEST_F(RDGExecutionTest, CallbackFailureClosesEncoderAndStopsLaterPasses) {
    ExpectSuccess = false;
    Setup = [&](RDGBuilder& graph) {
        {
            struct PassData {};
            auto pass = graph.AddComputePass("failure", PassData{}, +[](const PassData&, RDGComputePassContext&) noexcept { return false; });
            pass.NeverCull();
        };
        {
            struct PassData {
                decltype(&Trace) TraceRef;
            };
            auto pass = graph.AddComputePass("later", PassData{&Trace}, +[](const PassData& data, RDGComputePassContext&) noexcept {
            auto& Trace = *data.TraceRef; Trace.Executed.push_back("later"); return true; });
            pass.NeverCull();
        };
    };
    Run();
    EXPECT_TRUE(Trace.Executed.empty());
    EXPECT_EQ(Trace.OpenEncoders, 0);
    EXPECT_EQ(Trace.ComputeScopes, 1);
}

TEST_F(RDGExecutionTest, AllocationFailureReturnsFalseBeforeExecutingPasses) {
    ExpectSuccess = false;
    Device->FailAllocations = true;
    Setup = [&](RDGBuilder& graph) {
        const auto buffer = graph.CreateBuffer({.Size = 4, .Usage = BufferUse::UnorderedAccess});
        {
            struct PassData {
                decltype(&Trace) TraceRef;
            };
            auto pass = graph.AddComputePass("allocation", PassData{&Trace}, +[](const PassData& data, RDGComputePassContext&) noexcept {
            auto& Trace = *data.TraceRef; Trace.Executed.push_back("unexpected"); return true; });
            pass.WriteBuffer(buffer).NeverCull();
        };
    };
    Run();
    EXPECT_TRUE(Trace.Executed.empty());
}

TEST_F(RDGExecutionTest, CombinesRepeatedReadOnlyBufferAccessesAndRestoresBoundary) {
    ByteBuffer input{{.Size = 16, .Usage = BufferUse::Vertex | BufferUse::Resource}, Trace};
    Setup = [&](RDGBuilder& graph) {
        const auto buffer = graph.ImportBuffer(&input, BufferState::Common, BufferState::Common);
        const auto color = graph.CreateTexture(ColorDescriptor());
        {
            struct PassData {};
            auto pass = graph.AddRasterPass("read geometry", PassData{}, +[](const PassData&, RDGRasterPassContext&) noexcept { return true; });

            pass.WriteColor(0, color, LoadAction::Clear, StoreAction::Store).NeverCull();
            pass.ReadBuffer(buffer, BufferState::Vertex).ReadBuffer(buffer, BufferState::ShaderRead);
        };
    };
    Run();
    vector<BarrierBufferDescriptor> barriers;
    for (const auto& barrier : Trace.Barriers) {
        if (const auto* buffer = std::get_if<BarrierBufferDescriptor>(&barrier)) barriers.push_back(*buffer);
    }
    ASSERT_EQ(barriers.size(), 2u);
    EXPECT_EQ(barriers[0].Before, BufferStates{BufferState::Common});
    EXPECT_EQ(barriers[0].After, BufferState::Vertex | BufferState::ShaderRead);
    EXPECT_EQ(barriers[1].Before, barriers[0].After);
    EXPECT_EQ(barriers[1].After, BufferStates{BufferState::Common});
}

TEST_F(RDGExecutionTest, GeometryPoolsShareUploadBoundaryAcrossUploadOnlyAndDrawFrames) {
    const array layouts{VertexBufferLayout{0, 12, VertexStepMode::Vertex}};
    const array attributes{VertexInputAttribute{0, 0, "POSITION", 0, VertexFormat::FLOAT32X3, 0}};
    auto vertices = VertexBufferPool::Create(*Device, {.VertexStride = 12, .PageVertexCapacity = 3, .AdditionalUsage = BufferUse::Resource, .Layouts = layouts, .Attributes = attributes});
    auto indices = IndexBufferPool::Create(*Device, {.PageIndexCapacity = 3});
    ASSERT_TRUE(vertices && indices);
    auto v = vertices->Allocate(3);
    auto i = indices->Allocate(3);
    ASSERT_TRUE(v && i);
    const array<byte, 36> vertexBytes{byte{42}};
    const array<uint32_t, 3> indexBytes{0, 1, 2};
    bool draw = false;
    Setup = [&](RDGBuilder& graph) {
        if (!draw) {
            const auto vb = graph.ImportPersistentOwned(v->GetGpuBuffer());
            const auto ib = graph.ImportPersistentOwned(i->GetGpuBuffer());
            graph.UploadBuffer(vb, v->GetByteOffset(), RDGUploadSource::BorrowedBytes(vertexBytes));
            graph.UploadBuffer(ib, i->GetByteOffset(), RDGUploadSource::BorrowedBytes(std::as_bytes(std::span{indexBytes})));
            graph.MarkOutput(vb);
            graph.MarkOutput(ib);
            return;
        }
        const auto color = graph.CreateTexture(ColorDescriptor());
        {
            struct PassData {};
            auto pass = graph.AddRasterPass("pooled geometry", PassData{}, +[](const PassData&, RDGRasterPassContext&) noexcept { return true; });

            pass.WriteColor(0, color, LoadAction::Clear, StoreAction::Store).NeverCull();
            vertices->AddPassReadAccess(graph, pass, BufferState::Vertex | BufferState::ShaderRead);
            indices->AddPassReadAccess(graph, pass);
        };
    };
    Run();
    EXPECT_EQ(Trace.Copies, 2u);
    draw = true;
    Trace.Barriers.clear();
    Run();
    uint32_t pooledTransitions = 0;
    for (const auto& barrier : Trace.Barriers) {
        if (const auto* buffer = std::get_if<BarrierBufferDescriptor>(&barrier)) {
            if (buffer->Target == v->GetBuffer()) {
                EXPECT_TRUE(buffer->Before == BufferState::Common || buffer->After == BufferState::Common);
                EXPECT_TRUE(buffer->Before == (BufferState::Vertex | BufferState::ShaderRead) || buffer->After == (BufferState::Vertex | BufferState::ShaderRead));
            }
            ++pooledTransitions;
        }
    }
    EXPECT_EQ(pooledTransitions, 4u);
    Scheduler->WaitIdle();
}

TEST(RDG, CombinedBufferStateRequiresEveryUsageAndPassCompatibleBit) {
    for (const auto usage : {BufferUses{BufferUse::Vertex}, BufferUse::Vertex | BufferUse::Resource}) {
        FakeGpu gpu;
        auto scheduler = FrameScheduler::Create(gpu.Context, make_unique<CallbackPipeline>(), 1).Unwrap();
        RDGBuilder graph{scheduler->BeginFrame()};
        FakeBuffer input{16, usage};
        const auto buffer = graph.ImportBuffer(&input, BufferState::Common);
        {
            struct PassData {};
            auto pass = graph.AddComputePass("invalid", PassData{}, +[](const PassData&, RDGComputePassContext&) noexcept { return true; });
            pass.ReadBuffer(buffer, BufferState::Vertex | BufferState::ShaderRead).NeverCull();
        };
        EXPECT_FALSE(graph.Compile());
    }
}

TEST(RDG, ValidatesEveryReadOnlyBufferUsageAndCombinedImportBoundary) {
    const auto combined = BufferState::Vertex | BufferState::ShaderRead;
    for (const auto usage : {BufferUses{BufferUse::Vertex}, BufferUse::Vertex | BufferUse::Resource}) {
        FakeGpu gpu;
        auto scheduler = FrameScheduler::Create(gpu.Context, make_unique<CallbackPipeline>(), 1).Unwrap();
        RDGBuilder graph{scheduler->BeginFrame()};
        FakeBuffer input{16, usage};
        const auto buffer = graph.ImportBuffer(&input, combined, combined);
        {
            struct PassData {};
            auto pass = graph.AddRasterPass("geometry", PassData{}, +[](const PassData&, RDGRasterPassContext&) noexcept { return true; });

            pass.WriteColor(0, graph.CreateTexture(ColorDescriptor()), LoadAction::Clear, StoreAction::Store);
            pass.ReadBuffer(buffer, combined).NeverCull();
        };
        EXPECT_EQ(bool{graph.Compile()}, usage.HasFlag(BufferUse::Resource));
    }
}

TEST(RDG, ComputeCombinesShaderConstantAndIndirectReads) {
    FakeGpu gpu;
    auto scheduler = FrameScheduler::Create(gpu.Context, make_unique<CallbackPipeline>(), 1).Unwrap();
    RDGBuilder graph{scheduler->BeginFrame()};
    FakeBuffer input{16, BufferUse::Resource | BufferUse::CBuffer | BufferUse::Indirect};
    const auto buffer = graph.ImportBuffer(&input, BufferState::Common);
    {
        struct PassData {};
        auto pass = graph.AddComputePass("combined reads", PassData{}, +[](const PassData&, RDGComputePassContext&) noexcept { return true; });
        pass.ReadBuffer(buffer).ReadBuffer(buffer, BufferState::CBuffer | BufferState::Indirect).NeverCull();
    };
    EXPECT_TRUE(graph.Compile());
}

TEST(RDGTransientPool, RetainsCombinedReadOnlyBufferStateAcrossReuse) {
    Probe trace;
    ProbeDevice device{trace};
    RDGTransientResourcePool pool{device};
    const BufferDescriptor desc{.Size = 16, .Usage = BufferUse::Vertex | BufferUse::Resource};
    const auto first = pool.AcquireBuffer(desc);
    ASSERT_TRUE(first.Buffer);
    pool.CommitBufferState(first.Buffer.Get(), BufferState::Vertex | BufferState::ShaderRead);
    pool.BeginFrame();
    const auto reused = pool.AcquireBuffer(desc);
    EXPECT_EQ(reused.Buffer.Get(), first.Buffer.Get());
    EXPECT_EQ(reused.LastState, BufferState::Vertex | BufferState::ShaderRead);
}

TEST(RDG, RejectsMixedReadAndWriteBufferStates) {
    FakeGpu gpu;
    auto scheduler = FrameScheduler::Create(gpu.Context, make_unique<CallbackPipeline>(), 1).Unwrap();
    RDGBuilder graph{scheduler->BeginFrame()};
    FakeBuffer input{16, BufferUse::Vertex | BufferUse::UnorderedAccess};
    const auto buffer = graph.ImportBuffer(&input, BufferState::Common);
    {
        struct PassData {};
        auto pass = graph.AddRasterPass("invalid", PassData{}, +[](const PassData&, RDGRasterPassContext&) noexcept { return true; });

        pass.WriteColor(0, graph.CreateTexture(ColorDescriptor()), LoadAction::Clear, StoreAction::Store);
        pass.ReadBuffer(buffer, BufferState::Vertex | BufferState::UnorderedAccess).NeverCull();
    };
    EXPECT_FALSE(graph.Compile());
}

TEST(RDG, RejectsInvalidResourceHandles) {
    FakeGpu gpu;
    auto scheduler = FrameScheduler::Create(gpu.Context, make_unique<CallbackPipeline>(), 1).Unwrap();
    RDGBuilder graph{scheduler->BeginFrame()};
    {
        struct PassData {};
        auto pass = graph.AddComputePass("invalid", PassData{}, +[](const PassData&, RDGComputePassContext&) noexcept { return true; });
        pass.ReadBuffer({}).NeverCull();
    };
    EXPECT_FALSE(graph.Compile());
}

TEST(RDG, RejectsConflictingWholeResourceStates) {
    FakeGpu gpu;
    auto scheduler = FrameScheduler::Create(gpu.Context, make_unique<CallbackPipeline>(), 1).Unwrap();
    RDGBuilder graph{scheduler->BeginFrame()};
    const auto buffer = graph.CreateBuffer({.Size = 4, .Usage = BufferUse::Resource | BufferUse::UnorderedAccess});
    {
        struct PassData {};
        auto pass = graph.AddComputePass("invalid", PassData{}, +[](const PassData&, RDGComputePassContext&) noexcept { return true; });
        pass.WriteBuffer(buffer).ReadBuffer(buffer).NeverCull();
    };
    EXPECT_FALSE(graph.Compile());
}

TEST(RDG, RejectsUninitializedResourceReads) {
    FakeGpu gpu;
    auto scheduler = FrameScheduler::Create(gpu.Context, make_unique<CallbackPipeline>(), 1).Unwrap();
    RDGBuilder graph{scheduler->BeginFrame()};
    const auto buffer = graph.CreateBuffer({.Size = 4, .Usage = BufferUse::Resource});
    {
        struct PassData {};
        auto pass = graph.AddComputePass("invalid", PassData{}, +[](const PassData&, RDGComputePassContext&) noexcept { return true; });
        pass.ReadBuffer(buffer).NeverCull();
    };
    EXPECT_FALSE(graph.Compile());
}

TEST(RDG, RejectsAttachmentStatesInComputePasses) {
    FakeGpu gpu;
    auto scheduler = FrameScheduler::Create(gpu.Context, make_unique<CallbackPipeline>(), 1).Unwrap();
    RDGBuilder graph{scheduler->BeginFrame()};
    const auto texture = graph.CreateTexture(ColorDescriptor());
    {
        struct PassData {};
        auto pass = graph.AddComputePass("invalid", PassData{}, +[](const PassData&, RDGComputePassContext&) noexcept { return true; });
        pass.WriteTexture(texture, TextureState::RenderTarget).NeverCull();
    };
    EXPECT_FALSE(graph.Compile());
}

TEST(RDG, DuplicateImportsCannotDeclareDifferentPhysicalInitialStates) {
    FakeBuffer target{16, BufferUse::Resource | BufferUse::UnorderedAccess};
    FakeGpu gpu;
    auto scheduler = FrameScheduler::Create(gpu.Context, make_unique<CallbackPipeline>(), 1).Unwrap();
    RDGBuilder graph{scheduler->BeginFrame()};
    const auto a = graph.ImportBuffer(&target, BufferState::ShaderRead);
    const auto b = graph.ImportBuffer(&target, BufferState::UnorderedAccess);
    EXPECT_EQ(a, b);
    EXPECT_FALSE(graph.Compile());
}

TEST(RDGTransientPool, ReusesOnlyCompletedMatchingAllocationsAndKeepsTheirState) {
    Probe trace;
    ProbeDevice device{trace};
    RDGTransientResourcePool pool{device};
    const BufferDescriptor desc{.Size = 16, .Memory = MemoryType::Upload, .Usage = BufferUse::MapWrite | BufferUse::CopySource};
    const auto first = pool.AcquireBuffer(desc);
    const auto sibling = pool.AcquireBuffer(desc);
    ASSERT_TRUE(first.Buffer);
    ASSERT_TRUE(sibling.Buffer);
    EXPECT_NE(first.Buffer.Get(), sibling.Buffer.Get());
    EXPECT_EQ(first.LastState, BufferState::HostWrite);
    pool.CommitBufferState(first.Buffer.Get(), BufferState::CopySource);
    pool.BeginFrame();
    const auto reused = pool.AcquireBuffer(desc);
    EXPECT_EQ(reused.Buffer.Get(), first.Buffer.Get());
    EXPECT_EQ(reused.LastState, BufferState::CopySource);
    auto different = desc;
    different.Hints = ResourceHint::Dedicated;
    const auto other = pool.AcquireBuffer(different);
    ASSERT_TRUE(other.Buffer);
    EXPECT_NE(other.Buffer.Get(), sibling.Buffer.Get());
    EXPECT_EQ(trace.BufferAllocations.size(), 3);
}

TEST_F(RDGExecutionTest, ReadOnlyDepthAttachmentKeepsPriorDepthWriter) {
    Setup = [&](RDGBuilder& graph) {
        auto desc = ColorDescriptor();
        desc.Format = TextureFormat::D32_FLOAT_S8_UINT;
        desc.Usage = TextureUse::DepthStencilRead | TextureUse::DepthStencilWrite;
        const auto depth = graph.CreateTexture(desc);
        const auto color = graph.CreateTexture(ColorDescriptor());
        {
            struct PassData {
                decltype(&Trace) TraceRef;
            };
            auto pass = graph.AddRasterPass("depth", PassData{&Trace}, +[](const PassData& data, RDGRasterPassContext&) noexcept {
            auto& Trace = *data.TraceRef; Trace.Executed.push_back("depth"); return true; });
            pass.SetDepthStencil(depth, TextureState::DepthWrite, LoadAction::Clear, StoreAction::Store, LoadAction::Clear, StoreAction::Store);
        };
        {
            struct PassData {
                decltype(&Trace) TraceRef;
            };
            auto pass = graph.AddRasterPass("color", PassData{&Trace}, +[](const PassData& data, RDGRasterPassContext&) noexcept {
            auto& Trace = *data.TraceRef; Trace.Executed.push_back("color"); return true; });
            pass.SetDepthStencil(depth, TextureState::DepthRead, LoadAction::Load, StoreAction::Store, LoadAction::Load, StoreAction::Store)
                .WriteColor(0, color, LoadAction::Clear, StoreAction::Store);
        };
        graph.MarkOutput(color);
    };
    Run();
    EXPECT_EQ(Trace.Executed, (vector<string>{"depth", "color"}));
    ASSERT_FALSE(Trace.Views.empty());
    EXPECT_EQ(Trace.Views[0].Usage, TextureViewUsage::DepthWrite);
    ASSERT_EQ(Trace.RasterScopes.size(), 2u);
    EXPECT_EQ(Trace.RasterScopes[1].Depth->View->GetDesc().Usage, TextureViewUsage::DepthRead);
}

TEST_F(RDGExecutionTest, ImportedAttachmentUsesExplicitViewAndSynchronizesSameStateEpilogue) {
    ProbeTexture target{ColorDescriptor()};
    ProbeView view{{&target, TextureDimension::Dim2D, TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, TextureViewUsage::RenderTarget}};
    Setup = [&](RDGBuilder& graph) {
        const auto color = graph.ImportTexture(&target, TextureState::Undefined, TextureState::RenderTarget);
        {
            struct PassData {};
            auto pass = graph.AddRasterPass("external", PassData{}, +[](const PassData&, RDGRasterPassContext&) noexcept { return true; });
            pass.WriteColor(0, color, LoadAction::Clear, StoreAction::Store, {}, &view);
        };
        graph.MarkOutput(color);
    };
    Run();
    EXPECT_TRUE(Trace.Views.empty());
    EXPECT_TRUE(Trace.TextureAllocations.empty());
    ASSERT_EQ(Trace.Barriers.size(), 2);
    const auto epilogue = std::get<BarrierTextureDescriptor>(Trace.Barriers.back());
    EXPECT_EQ(epilogue.Before, TextureStates{TextureState::RenderTarget});
    EXPECT_EQ(epilogue.After, TextureStates{TextureState::RenderTarget});
}

TEST_F(RDGExecutionTest, ReordersUploadsAndIndependentPassesButKeepsMeshDeclarationOrder) {
    ByteBuffer constants{{.Size = 4, .Memory = MemoryType::Upload, .Usage = BufferUse::MapWrite | BufferUse::CBuffer}, Trace};
    Setup = [&](RDGBuilder& graph) {
        const auto color = graph.CreateTexture(ColorDescriptor());
        const auto other = graph.CreateTexture(ColorDescriptor());
        const auto buffer = graph.ImportBuffer(&constants, BufferState::HostWrite);
        {
            struct PassData {
                decltype(&Trace) TraceRef;
            };
            auto pass = graph.AddRasterPass("first", PassData{&Trace}, +[](const PassData& data, RDGRasterPassContext&) noexcept {
            auto& Trace = *data.TraceRef; Trace.Executed.push_back("first"); return true; });
            pass.UseMeshPass({3}).WriteColor(0, color, LoadAction::Clear, StoreAction::Store);
        };
        {
            struct PassData {
                decltype(&Trace) TraceRef;
            };
            auto pass = graph.AddComputePass("independent", PassData{&Trace}, +[](const PassData& data, RDGComputePassContext&) noexcept {
            auto& Trace = *data.TraceRef; Trace.Executed.push_back("compute"); return true; });
            pass.NeverCull();
        };
        {
            struct PassData {
                decltype(&Trace) TraceRef;
            };
            auto pass = graph.AddRasterPass("other", PassData{&Trace}, +[](const PassData& data, RDGRasterPassContext&) noexcept {
            auto& Trace = *data.TraceRef; Trace.Executed.push_back("other"); return true; });
            pass.UseMeshPass({1}).WriteColor(0, other, LoadAction::Clear, StoreAction::Store).NeverCull();
        };
        const array bytes{byte{42}, byte{}, byte{}, byte{}};
        graph.UploadBuffer(buffer, 0, RDGUploadSource::SnapshotBytes(bytes));
        {
            struct PassData {
                std::remove_cvref_t<decltype(buffer)> Buffer;
                decltype(&Trace) TraceRef;
            };
            auto pass = graph.AddRasterPass("last", PassData{buffer, &Trace}, +[](const PassData& data, RDGRasterPassContext& ctx) noexcept {
            const auto& buffer = data.Buffer; auto& Trace = *data.TraceRef;
            EXPECT_EQ(static_cast<ByteBuffer&>(*ctx.ResolveBuffer(buffer).Buffer).Bytes[0], byte{42});
            Trace.Executed.push_back("last");
            return true; });
            pass.UseMeshPass({2}).UseMeshPass({3}).WriteColor(0, color, LoadAction::Load, StoreAction::Store).ReadBuffer(buffer, BufferState::CBuffer);
        };
        graph.MarkOutput(color);
    };
    for (uint32_t repeat = 0; repeat < 3; ++repeat) {
        Trace.Executed.clear();
        Trace.RasterScopes.clear();
        Run();
        EXPECT_EQ(Trace.Executed, (vector<string>{"first", "last", "compute", "other"}));
        EXPECT_EQ(Trace.RasterScopes.size(), 2);
        const auto alive = Compiled->GetAliveMeshPasses();
        EXPECT_EQ((vector<MeshPassId>{alive.begin(), alive.end()}), (vector<MeshPassId>{{3}, {1}, {2}}));
    }
}

TEST_F(RDGExecutionTest, LiveReaderRunsBeforePreferredCopyOverwrite) {
    ByteBuffer target{{.Size = 4, .Usage = BufferUse::Resource | BufferUse::CopyDestination}, Trace};
    Setup = [&](RDGBuilder& graph) {
        const auto buffer = graph.ImportBuffer(&target, BufferState::ShaderRead);
        {
            struct PassData {
                decltype(&Trace) TraceRef;
            };
            auto pass = graph.AddComputePass("reader", PassData{&Trace}, +[](const PassData& data, RDGComputePassContext&) noexcept {
            auto& Trace = *data.TraceRef; Trace.Executed.push_back("reader"); return true; });
            pass.ReadBuffer(buffer).NeverCull();
        };
        {
            struct PassData {
                decltype(&Trace) TraceRef;
            };
            auto pass = graph.AddCopyPass("overwrite", PassData{&Trace}, +[](const PassData& data, RDGCopyPassContext&) noexcept {
            auto& Trace = *data.TraceRef; Trace.Executed.push_back("overwrite"); return true; });
            pass.WriteBuffer(buffer);
        };
        graph.MarkOutput(buffer);
    };
    Run();
    EXPECT_EQ(Trace.Executed, (vector<string>{"reader", "overwrite"}));
}

TEST_F(RDGExecutionTest, TextureReaderPrecedesMergeableComputeOverwrite) {
    ProbeTexture target{ColorDescriptor()};
    Setup = [&](RDGBuilder& graph) {
        const auto source = graph.ImportTexture(&target, TextureState::ShaderRead);
        const auto color = graph.CreateTexture(ColorDescriptor());
        {
            struct PassData {
                decltype(&Trace) TraceRef;
            };
            auto pass = graph.AddComputePass("open compute", PassData{&Trace}, +[](const PassData& data, RDGComputePassContext&) noexcept {
            auto& Trace = *data.TraceRef; Trace.Executed.push_back("open"); return true; });
            pass.NeverCull();
        };
        {
            struct PassData {
                decltype(&Trace) TraceRef;
            };
            auto pass = graph.AddRasterPass("reader", PassData{&Trace}, +[](const PassData& data, RDGRasterPassContext&) noexcept {
            auto& Trace = *data.TraceRef; Trace.Executed.push_back("reader"); return true; });
            pass.ReadTexture(source).WriteColor(0, color, LoadAction::Clear, StoreAction::Store).NeverCull();
        };
        {
            struct PassData {
                decltype(&Trace) TraceRef;
            };
            auto pass = graph.AddComputePass("overwrite", PassData{&Trace}, +[](const PassData& data, RDGComputePassContext&) noexcept {
            auto& Trace = *data.TraceRef; Trace.Executed.push_back("overwrite"); return true; });
            pass.WriteTexture(source);
        };
        graph.MarkOutput(source);
    };
    Run();
    EXPECT_EQ(Trace.Executed, (vector<string>{"open", "reader", "overwrite"}));
    EXPECT_EQ(Trace.ComputeScopes, 2);
}

TEST_F(RDGExecutionTest, OrderEdgesDoNotKeepDeadReadersAlive) {
    ProbeTexture target{ColorDescriptor()};
    Setup = [&](RDGBuilder& graph) {
        const auto color = graph.ImportTexture(&target, TextureState::ShaderRead);
        {
            struct PassData {
                decltype(&Trace) TraceRef;
            };
            auto pass = graph.AddComputePass("dead reader", PassData{&Trace}, +[](const PassData& data, RDGComputePassContext&) noexcept {
            auto& Trace = *data.TraceRef; Trace.Executed.push_back("dead"); return true; });
            pass.ReadTexture(color);
        };
        {
            struct PassData {
                decltype(&Trace) TraceRef;
            };
            auto pass = graph.AddRasterPass("clear", PassData{&Trace}, +[](const PassData& data, RDGRasterPassContext&) noexcept {
            auto& Trace = *data.TraceRef; Trace.Executed.push_back("clear"); return true; });
            pass.WriteColor(0, color, LoadAction::Clear, StoreAction::Store);
        };
        graph.MarkOutput(color);
    };
    Run();
    EXPECT_EQ(Trace.Executed, (vector<string>{"clear"}));
}

TEST_F(RDGExecutionTest, OrderEdgeDoesNotHideDataEdgeFromSamePredecessor) {
    ByteBuffer target{{.Size = 4, .Usage = BufferUse::Resource | BufferUse::UnorderedAccess}, Trace};
    Setup = [&](RDGBuilder& graph) {
        const auto x = graph.ImportBuffer(&target, BufferState::ShaderRead);
        const auto y = graph.CreateBuffer(target.Desc);
        {
            struct PassData {
                decltype(&Trace) TraceRef;
            };
            auto pass = graph.AddComputePass("producer", PassData{&Trace}, +[](const PassData& data, RDGComputePassContext&) noexcept {
            auto& Trace = *data.TraceRef; Trace.Executed.push_back("producer"); return true; });
            pass.ReadBuffer(x).WriteBuffer(y);
        };
        {
            struct PassData {
                decltype(&Trace) TraceRef;
            };
            auto pass = graph.AddComputePass("consumer", PassData{&Trace}, +[](const PassData& data, RDGComputePassContext&) noexcept {
            auto& Trace = *data.TraceRef; Trace.Executed.push_back("consumer"); return true; });
            pass.WriteBuffer(x).ReadBuffer(y);
        };
        graph.MarkOutput(x);
    };
    Run();
    EXPECT_EQ(Trace.Executed, (vector<string>{"producer", "consumer"}));
    EXPECT_EQ(Trace.ComputeScopes, 2);
}

TEST_F(RDGExecutionTest, ClearStaysAfterWriterKeptAliveByAnotherOutput) {
    Setup = [&](RDGBuilder& graph) {
        const auto x = graph.CreateTexture(ColorDescriptor());
        const auto y = graph.CreateTexture(ColorDescriptor());
        const auto gate = graph.CreateTexture(ColorDescriptor());
        {
            struct PassData {
                decltype(&Trace) TraceRef;
            };
            auto pass = graph.AddRasterPass("gate", PassData{&Trace}, +[](const PassData& data, RDGRasterPassContext&) noexcept {
            auto& Trace = *data.TraceRef; Trace.Executed.push_back("gate"); return true; });
            pass.WriteColor(0, gate, LoadAction::Clear, StoreAction::Store);
        };
        {
            struct PassData {
                decltype(&Trace) TraceRef;
            };
            auto pass = graph.AddComputePass("old writer", PassData{&Trace}, +[](const PassData& data, RDGComputePassContext&) noexcept {
            auto& Trace = *data.TraceRef; Trace.Executed.push_back("old writer"); return true; });
            pass.ReadTexture(gate).WriteTexture(x).WriteTexture(y);
        };
        {
            struct PassData {
                decltype(&Trace) TraceRef;
            };
            auto pass = graph.AddCopyPass("new writer", PassData{&Trace}, +[](const PassData& data, RDGCopyPassContext&) noexcept {
            auto& Trace = *data.TraceRef; Trace.Executed.push_back("copy"); return true; });
            pass.ReadTexture(gate).NeverCull();
        };
        {
            struct PassData {
                decltype(&Trace) TraceRef;
            };
            auto pass = graph.AddRasterPass("clear", PassData{&Trace}, +[](const PassData& data, RDGRasterPassContext&) noexcept {
            auto& Trace = *data.TraceRef; Trace.Executed.push_back("clear"); return true; });
            pass.WriteColor(0, x, LoadAction::Clear, StoreAction::Store);
        };
        graph.MarkOutput(x);
        graph.MarkOutput(y);
    };
    Run();
    EXPECT_EQ(Trace.Executed, (vector<string>{"gate", "copy", "old writer", "clear"}));
}

TEST_F(RDGExecutionTest, ComputeHoistsIndependentResourcesAndSplitsAtUavDependency) {
    Setup = [&](RDGBuilder& graph) {
        const auto a = graph.CreateBuffer({.Size = 4, .Usage = BufferUse::UnorderedAccess});
        const auto b = graph.CreateTexture(ColorDescriptor());
        {
            struct PassData {
                std::remove_cvref_t<decltype(a)> A;
                decltype(&Trace) TraceRef;
            };
            auto pass = graph.AddComputePass("write a", PassData{a, &Trace}, +[](const PassData& data, RDGComputePassContext& ctx) noexcept {
            const auto& a = data.A; auto& Trace = *data.TraceRef; EXPECT_EQ(ctx.ResolveBuffer(a).Size, 4); EXPECT_EQ(Trace.BarrierBatches, (vector<size_t>{2})); return true; });
            pass.WriteBuffer(a);
        };
        {
            struct PassData {
                std::remove_cvref_t<decltype(b)> B;
            };
            auto pass = graph.AddComputePass("write b", PassData{b}, +[](const PassData& data, RDGComputePassContext& ctx) noexcept {
            const auto& b = data.B; EXPECT_TRUE(ctx.ResolveView(b, TextureViewUsage::UnorderedAccess)); return true; });
            pass.WriteTexture(b);
        };
        {
            struct PassData {};
            auto pass = graph.AddComputePass("update a", PassData{}, +[](const PassData&, RDGComputePassContext&) noexcept { return true; });
            pass.ReadWriteBuffer(a);
        };
        graph.MarkOutput(a);
        graph.MarkOutput(b);
    };
    Run();
    EXPECT_EQ(Trace.ComputeScopes, 2);
    EXPECT_EQ(Trace.BarrierBatches, (vector<size_t>{2, 1}));
    ASSERT_EQ(Trace.Barriers.size(), 3);
    EXPECT_TRUE(std::holds_alternative<BarrierUavDescriptor>(Trace.Barriers.back()));
}

TEST_F(RDGExecutionTest, RasterGroupTracksConflictsAcrossAllEarlierSubpasses) {
    Setup = [&](RDGBuilder& graph) {
        const auto color = graph.CreateTexture(ColorDescriptor());
        const auto uav = graph.CreateBuffer({.Size = 4, .Usage = BufferUse::UnorderedAccess});
        for (uint32_t i = 0; i < 3; ++i) {
            {
                struct PassData {};
                auto pass = graph.AddRasterPass("raster", PassData{}, +[](const PassData&, RDGRasterPassContext&) noexcept { return true; });

                pass.WriteColor(0, color, i == 0 ? LoadAction::Clear : LoadAction::Load, StoreAction::Store);
                if (i != 1) pass.WriteBuffer(uav);
            };
        }
        graph.MarkOutput(color);
    };
    Run();
    ASSERT_EQ(Trace.RasterScopes.size(), 2);
    EXPECT_TRUE(Trace.RasterScopes[0].AllowUavWrites);
    EXPECT_TRUE(Trace.RasterScopes[1].AllowUavWrites);
    EXPECT_EQ(Trace.BarrierBatches, (vector<size_t>{2, 2}));
    EXPECT_TRUE(std::holds_alternative<BarrierUavDescriptor>(Trace.Barriers.back()));
}

TEST_F(RDGExecutionTest, RasterSplitsWhenReadOnlyBufferChangesState) {
    ByteBuffer input{{.Size = 16, .Usage = BufferUse::Vertex | BufferUse::CBuffer}, Trace};
    Setup = [&](RDGBuilder& graph) {
        const auto buffer = graph.ImportBuffer(&input, BufferState::Vertex);
        const auto color = graph.CreateTexture(ColorDescriptor());
        {
            struct PassData {};
            auto pass = graph.AddRasterPass("vertex", PassData{}, +[](const PassData&, RDGRasterPassContext&) noexcept { return true; });
            pass.WriteColor(0, color, LoadAction::Clear, StoreAction::Store).ReadBuffer(buffer, BufferState::Vertex);
        };
        {
            struct PassData {};
            auto pass = graph.AddRasterPass("constant", PassData{}, +[](const PassData&, RDGRasterPassContext&) noexcept { return true; });
            pass.WriteColor(0, color, LoadAction::Load, StoreAction::Store).ReadBuffer(buffer, BufferState::CBuffer);
        };
        graph.MarkOutput(color);
    };
    Run();
    EXPECT_EQ(Trace.RasterScopes.size(), 2);
    ASSERT_EQ(Trace.Barriers.size(), 3);
    const auto& transition = std::get<BarrierBufferDescriptor>(Trace.Barriers.back());
    EXPECT_EQ(transition.Before, BufferStates{BufferState::Vertex});
    EXPECT_EQ(transition.After, BufferStates{BufferState::CBuffer});
}

TEST_F(RDGExecutionTest, ClearAndDiscardSeparateRasterScopes) {
    Setup = [&](RDGBuilder& graph) {
        const auto color = graph.CreateTexture(ColorDescriptor());
        for (uint32_t i = 0; i < 3; ++i) {
            {
                struct PassData {};
                auto pass = graph.AddRasterPass("raster", PassData{}, +[](const PassData&, RDGRasterPassContext&) noexcept { return true; });
                pass.WriteColor(0, color, i == 2 ? LoadAction::Load : LoadAction::Clear, i == 1 ? StoreAction::Discard : StoreAction::Store).NeverCull();
            };
        }
    };
    Run();
    EXPECT_EQ(Trace.RasterScopes.size(), 3);
    ASSERT_EQ(Trace.Barriers.size(), 3);
    EXPECT_EQ(std::get<BarrierTextureDescriptor>(Trace.Barriers[1]).Before, TextureStates{TextureState::RenderTarget});
}

TEST_F(RDGExecutionTest, DepthStateChangeSeparatesOtherwiseMatchingAttachments) {
    Setup = [&](RDGBuilder& graph) {
        auto desc = ColorDescriptor();
        desc.Format = TextureFormat::D32_FLOAT;
        desc.Usage = TextureUse::DepthStencilWrite | TextureUse::DepthStencilRead;
        const auto depth = graph.CreateTexture(desc);
        {
            struct PassData {};
            auto pass = graph.AddRasterPass("write depth", PassData{}, +[](const PassData&, RDGRasterPassContext&) noexcept { return true; });
            pass.SetDepthStencil(depth, TextureState::DepthWrite, LoadAction::Clear, StoreAction::Store);
        };
        {
            struct PassData {};
            auto pass = graph.AddRasterPass("read depth", PassData{}, +[](const PassData&, RDGRasterPassContext&) noexcept { return true; });
            pass.SetDepthStencil(depth, TextureState::DepthRead, LoadAction::Load, StoreAction::Store).NeverCull();
        };
    };
    Run();
    EXPECT_EQ(Trace.RasterScopes.size(), 2);
    ASSERT_EQ(Trace.Barriers.size(), 2);
    EXPECT_EQ(std::get<BarrierTextureDescriptor>(Trace.Barriers.back()).After, TextureStates{TextureState::DepthRead});
}

TEST_F(RDGExecutionTest, RasterCallbackFailureClosesMergedScopeAndSkipsRemainingCallbacks) {
    ExpectSuccess = false;
    Setup = [&](RDGBuilder& graph) {
        const auto color = graph.CreateTexture(ColorDescriptor());
        for (uint32_t i = 0; i < 3; ++i) {
            {
                struct PassData {
                    std::remove_cvref_t<decltype(i)> I;
                    decltype(&Trace) TraceRef;
                };
                auto pass = graph.AddRasterPass("raster", PassData{i, &Trace}, +[](const PassData& data, RDGRasterPassContext&) noexcept {
            const auto& i = data.I; auto& Trace = *data.TraceRef; Trace.Executed.emplace_back(radray::format("{}", i)); return i != 1; });
                pass.WriteColor(0, color, i == 0 ? LoadAction::Clear : LoadAction::Load, StoreAction::Store);
            };
        }
        graph.MarkOutput(color);
    };
    Run();
    EXPECT_EQ(Trace.Executed, (vector<string>{"0", "1"}));
    EXPECT_EQ(Trace.RasterScopes.size(), 1);
}

TEST_F(RDGExecutionTest, DifferentExplicitViewsOfSameTextureSplitRasterScopes) {
    ProbeTexture target{ColorDescriptor()};
    const TextureViewDescriptor desc{&target, TextureDimension::Dim2D, TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, TextureViewUsage::RenderTarget};
    ProbeView first{desc};
    ProbeView second{desc};
    Setup = [&](RDGBuilder& graph) {
        const auto color = graph.ImportTexture(&target, TextureState::Undefined);
        {
            struct PassData {};
            auto pass = graph.AddRasterPass("first view", PassData{}, +[](const PassData&, RDGRasterPassContext&) noexcept { return true; });
            pass.WriteColor(0, color, LoadAction::Clear, StoreAction::Store, {}, &first);
        };
        {
            struct PassData {};
            auto pass = graph.AddRasterPass("second view", PassData{}, +[](const PassData&, RDGRasterPassContext&) noexcept { return true; });
            pass.WriteColor(0, color, LoadAction::Load, StoreAction::Store, {}, &second);
        };
        graph.MarkOutput(color);
    };
    Run();
    ASSERT_EQ(Trace.RasterScopes.size(), 2);
    EXPECT_EQ(Trace.RasterScopes[0].Colors[0].View, &first);
    EXPECT_EQ(Trace.RasterScopes[1].Colors[0].View, &second);
}

TEST_F(RDGExecutionTest, DepthOnlyFormatIgnoresAbsentStencilActionsWhenMerging) {
    Setup = [&](RDGBuilder& graph) {
        auto desc = ColorDescriptor();
        desc.Format = TextureFormat::D32_FLOAT;
        desc.Usage = TextureUse::DepthStencilWrite;
        const auto depth = graph.CreateTexture(desc);
        {
            struct PassData {};
            auto pass = graph.AddRasterPass("clear depth", PassData{}, +[](const PassData&, RDGRasterPassContext&) noexcept { return true; });
            pass.SetDepthStencil(depth, TextureState::DepthWrite, LoadAction::Clear, StoreAction::Store);
        };
        {
            struct PassData {};
            auto pass = graph.AddRasterPass("load depth", PassData{}, +[](const PassData&, RDGRasterPassContext&) noexcept { return true; });
            pass.SetDepthStencil(depth, TextureState::DepthWrite, LoadAction::Load, StoreAction::Store);
        };
        graph.MarkOutput(depth);
    };
    Run();
    EXPECT_EQ(Trace.RasterScopes.size(), 1);
    EXPECT_EQ(Trace.Barriers.size(), 1);
}

TEST_F(RDGExecutionTest, StencilClearSeparatesOtherwiseMatchingDepthScopes) {
    Setup = [&](RDGBuilder& graph) {
        auto desc = ColorDescriptor();
        desc.Format = TextureFormat::D32_FLOAT_S8_UINT;
        desc.Usage = TextureUse::DepthStencilWrite;
        const auto depth = graph.CreateTexture(desc);
        {
            struct PassData {};
            auto pass = graph.AddRasterPass("clear both", PassData{}, +[](const PassData&, RDGRasterPassContext&) noexcept { return true; });
            pass.SetDepthStencil(depth, TextureState::DepthWrite, LoadAction::Clear, StoreAction::Store, LoadAction::Clear, StoreAction::Store);
        };
        {
            struct PassData {};
            auto pass = graph.AddRasterPass("clear stencil", PassData{}, +[](const PassData&, RDGRasterPassContext&) noexcept { return true; });
            pass.SetDepthStencil(depth, TextureState::DepthWrite, LoadAction::Load, StoreAction::Store, LoadAction::Clear, StoreAction::Store);
        };
        graph.MarkOutput(depth);
    };
    Run();
    EXPECT_EQ(Trace.RasterScopes.size(), 2);
    EXPECT_EQ(Trace.Barriers.size(), 2);
}

TEST_F(RDGExecutionTest, LaterRasterUavWriteEnablesWritesForWholeMergedScope) {
    Setup = [&](RDGBuilder& graph) {
        const auto color = graph.CreateTexture(ColorDescriptor());
        const auto uav = graph.CreateTexture(ColorDescriptor());
        {
            struct PassData {};
            auto pass = graph.AddRasterPass("color", PassData{}, +[](const PassData&, RDGRasterPassContext&) noexcept { return true; });
            pass.WriteColor(0, color, LoadAction::Clear, StoreAction::Store);
        };
        {
            struct PassData {
                std::remove_cvref_t<decltype(uav)> Uav;
            };
            auto pass = graph.AddRasterPass("uav", PassData{uav}, +[](const PassData& data, RDGRasterPassContext& ctx) noexcept {
            const auto& uav = data.Uav; return bool(ctx.ResolveView(uav, TextureViewUsage::UnorderedAccess)); });
            pass.WriteColor(0, color, LoadAction::Load, StoreAction::Store).WriteTexture(uav);
        };
        graph.MarkOutput(color);
    };
    Run();
    ASSERT_EQ(Trace.RasterScopes.size(), 1);
    EXPECT_TRUE(Trace.RasterScopes[0].AllowUavWrites);
    EXPECT_EQ(Trace.BarrierBatches, (vector<size_t>{2}));
}

TEST_F(RDGExecutionTest, ComputeSharedReadsMergeButTextureUavWritesSplit) {
    ByteBuffer input{{.Size = 4, .Usage = BufferUse::Resource}, Trace};
    Setup = [&](RDGBuilder& graph) {
        const auto buffer = graph.ImportBuffer(&input, BufferState::ShaderRead);
        const auto uav = graph.CreateTexture(ColorDescriptor());
        {
            struct PassData {};
            auto pass = graph.AddComputePass("reader", PassData{}, +[](const PassData&, RDGComputePassContext&) noexcept { return true; });
            pass.ReadBuffer(buffer).NeverCull();
        };
        {
            struct PassData {};
            auto pass = graph.AddComputePass("writer", PassData{}, +[](const PassData&, RDGComputePassContext&) noexcept { return true; });
            pass.ReadBuffer(buffer).WriteTexture(uav);
        };
        {
            struct PassData {};
            auto pass = graph.AddComputePass("update", PassData{}, +[](const PassData&, RDGComputePassContext&) noexcept { return true; });
            pass.ReadWriteTexture(uav);
        };
        graph.MarkOutput(uav);
    };
    Run();
    EXPECT_EQ(Trace.ComputeScopes, 2);
    EXPECT_EQ(Trace.BarrierBatches, (vector<size_t>{1, 1}));
    ASSERT_EQ(Trace.Barriers.size(), 2);
    EXPECT_TRUE(std::holds_alternative<BarrierUavDescriptor>(Trace.Barriers.back()));
}

class RdgOnlyPipeline final : public RenderGraphFramePipeline {
public:
    explicit RdgOnlyPipeline(DeviceContext& context) : RenderGraphFramePipeline(context, 2) {}
    Nullable<const uint32_t*> Input{nullptr};
    bool DeclareMesh{false};
    vector<uint32_t> Recorded;
    void Wait(FrameSlot&) noexcept override { EXPECT_FALSE(Input); }

private:
    void ExtractFrameData(FrameSlot& slot) noexcept override {
        ASSERT_TRUE(Input);
        _values[slot.GetIndex()] = slot.Emplace<uint32_t>(*Input);
    }
    void SetupGraph(RDGBuilder& graph, FrameSlot& slot) noexcept override {
        const auto value = *_values[slot.GetIndex()];
        if (DeclareMesh) {
            const auto color = graph.CreateTexture(ColorDescriptor());
            {
                struct PassData {};
                auto pass = graph.AddRasterPass("missing extraction", PassData{}, +[](const PassData&, RDGRasterPassContext&) noexcept { return true; });

                pass.UseMeshPass({9}).WriteColor(0, color, LoadAction::Clear, StoreAction::Store);
            };
            graph.MarkOutput(color);
        } else {
            {
                struct PassData {
                    std::remove_cvref_t<decltype(value)> Value;
                    decltype(&Recorded) RecordedRef;
                };
                auto pass = graph.AddCopyPass("custom content", PassData{value, &Recorded}, +[](const PassData& data, RDGCopyPassContext&) noexcept {
                    const auto& value = data.Value;
                    auto& Recorded = *data.RecordedRef;
                    Recorded.push_back(value);
                    return true; });
                pass.NeverCull();
            };
        }
    }
    void SubmitFrame(FrameSlot&, CommandBuffer&) noexcept override {}
    array<Nullable<uint32_t*>, 2> _values{nullptr, nullptr};
};

TEST(RenderGraphFramePipeline, CustomContentNeedsNoSceneOrMeshExtraction) {
    Probe trace;
    auto device = make_shared<ProbeDevice>(trace);
    DeviceContext context{device, &device->Queue};
    auto owner = make_unique<RdgOnlyPipeline>(context);
    auto* pipeline = owner.get();
    auto scheduler = FrameScheduler::Create(context, std::move(owner), 2);
    ASSERT_TRUE(scheduler);
    for (uint32_t value : {17u, 29u, 41u}) {
        scheduler->BeginFrame();
        pipeline->Input = &value;
        scheduler->RunFrame();
        pipeline->Input = nullptr;
    }
    EXPECT_EQ(pipeline->Recorded, (vector<uint32_t>{17, 29, 41}));
    scheduler->WaitIdle();
    scheduler = nullptr;
}

TEST(RenderGraphFramePipeline, AliveMeshPassWithoutExtractionTerminatesWithDiagnostic) {
    EXPECT_DEATH(([] {
                     SetLogCallback([](LogLevel, std::string_view message, void*) {
                         std::fwrite(message.data(), 1, message.size(), stderr);
                         std::fflush(stderr);
                     },
                                    nullptr);
                     Probe trace;
                     auto device = make_shared<ProbeDevice>(trace);
                     DeviceContext context{device, &device->Queue};
                     auto owner = make_unique<RdgOnlyPipeline>(context);
                     uint32_t value = 1;
                     owner->Input = &value;
                     owner->DeclareMesh = true;
                     auto scheduler = FrameScheduler::Create(context, std::move(owner), 2);
                     scheduler->RunFrame();
                 }()),
                 "alive mesh pass 9 requires an ExtractMeshPass implementation");
}

class SceneRdgPipeline final : public RenderGraphFramePipeline {
public:
    SceneRdgPipeline(DeviceContext& context, MeshPassProcessor& processor, Buffer& vertices, Buffer& indices)
        : RenderGraphFramePipeline(context, 2), Registration{{0}, &processor}, Vertices(vertices), Indices(indices) {}
    Nullable<const Scene*> Source{nullptr};
    PrimitiveHandle Candidate;
    MeshPassRegistration Registration;
    vector<uint32_t> Recorded;
    std::span<const MeshPassRegistration> GetMeshPassRegistrations() noexcept override { return {&Registration, 1}; }
    void Wait(FrameSlot&) noexcept override { EXPECT_FALSE(Source); }

private:
    void ExtractFrameData(FrameSlot&) noexcept override {
        if (!Source) RADRAY_ABORT("missing test scene input");
    }
    void SetupGraph(RDGBuilder& graph, FrameSlot&) noexcept override {
        const auto count = Source->GetPrimitiveCount();
        {
            struct PassData {
                std::remove_cvref_t<decltype(count)> Count;
                decltype(&Recorded) RecordedRef;
            };
            auto pass = graph.AddCopyPass("custom RDG content", PassData{count, &Recorded}, +[](const PassData& data, RDGCopyPassContext&) noexcept {
                const auto& count = data.Count;
                auto& Recorded = *data.RecordedRef;
                Recorded.push_back(count);
                return true; });
            pass.NeverCull();
        };
        const auto vertices = graph.ImportBuffer(&Vertices, BufferState::Vertex);
        const auto indices = graph.ImportBuffer(&Indices, BufferState::Index);
        const auto color = graph.CreateTexture(ColorDescriptor());
        {
            struct PassData {};
            auto pass = graph.AddRasterPass("cached Scene content", PassData{}, +[](const PassData&, RDGRasterPassContext& context) noexcept { return SubmitMeshDrawCommands(context.GetEncoder(), context.GetVisibleCommands({0})); });

            pass.UseMeshPass({0}).ReadBuffer(vertices, BufferState::Vertex).ReadBuffer(indices, BufferState::Index).WriteColor(0, color, LoadAction::Clear, StoreAction::Store);
        };
        graph.MarkOutput(color);
    }
    void ExtractMeshPass(MeshPassId pass, FrameSlot&, MeshPassFrameDrawList& list) noexcept override {
        if (!Source->GatherVisiblePrimitives(pass, std::span{&Candidate, 1}, list)) RADRAY_ABORT("cannot gather test scene");
    }
    void SubmitFrame(FrameSlot&, CommandBuffer&) noexcept override {}
    Buffer& Vertices;
    Buffer& Indices;
};

TEST(RenderGraphFramePipeline, ExplicitSceneInputsCoexistWithCustomRdgPassAndMayBeClearedBeforeWait) {
    class Processor final : public MeshPassProcessor {
    public:
        void AddMeshBatch(const MeshBatchContext& context, MeshPassDrawListContext& output) override {
            BuildMeshDrawCommands(context, {.Pipeline = &Pipeline}, output);
        }
        FakePipeline Pipeline;
    } processor;
    Probe trace;
    auto device = make_shared<ProbeDevice>(trace);
    DeviceContext context{device, &device->Queue};
    FakeBuffer vertices{48, BufferUse::Vertex}, indices{12, BufferUse::Index};
    VertexInput input;
    input.Layouts = {{0, 12, VertexStepMode::Vertex}};
    input.Attributes = {{0, 0, "POSITION", 0, VertexFormat::FLOAT32X3, 0}};
    input.Streams = {{&vertices, 0, 48, 0}};
    const auto add = [&](Scene& scene) {
        const auto primitive = scene.AddPrimitive();
        MeshBatch batch;
        batch.VertexInput = &input;
        batch.Material = scene.AddMaterial({});
        batch.Elements.push_back({.IndexBuffer = &indices, .IndexStride = 4, .IndexCount = 3});
        EXPECT_TRUE(scene.AddMeshBatch(primitive, std::move(batch)).IsValid());
        return primitive;
    };
    Scene first, second;
    auto a = add(first), b = add(second);
    second.AddPrimitive();
    auto owner = make_unique<SceneRdgPipeline>(context, processor, vertices, indices);
    auto* pipeline = owner.get();
    auto scheduler = FrameScheduler::Create(context, std::move(owner), 2);
    ASSERT_TRUE(scheduler);
    const auto run = [&](const Scene& scene, PrimitiveHandle candidate) {
        scheduler->BeginFrame();
        pipeline->Source = &scene;
        pipeline->Candidate = candidate;
        scheduler->RunFrame();
        pipeline->Source = nullptr;
    };
    first.SyncCachedMeshDrawCommands(pipeline->GetMeshPassRegistrations());
    run(first, a);
    first.Clear();
    second.SyncCachedMeshDrawCommands(pipeline->GetMeshPassRegistrations());
    run(second, b);
    second.Clear();
    a = add(first);
    first.SyncCachedMeshDrawCommands(pipeline->GetMeshPassRegistrations());
    run(first, a);
    first.Clear();
    scheduler->WaitIdle();
    EXPECT_EQ(pipeline->Recorded, (vector<uint32_t>{1, 2, 1}));
}

class FilteringPipeline final : public RenderGraphFramePipeline {
public:
    explicit FilteringPipeline(DeviceContext& context) : RenderGraphFramePipeline(context, 1) {}
    void Wait(FrameSlot&) noexcept override {}
    vector<string> Events;
    vector<MeshPassId> Extracted;
    std::function<void(RDGBuilder&)> OnSetup;
    bool Reorder{false};

protected:
    void ExtractFrameData(FrameSlot&) noexcept override { Events.push_back("frame"); }
    void SetupGraph(RDGBuilder& graph, FrameSlot& slot) noexcept override {
        Events.push_back("setup");
        if (OnSetup) OnSetup(graph);
        // 被裁剪的列表保持不变，包括其初始顺序。
        slot.GetPassDrawList({7}).VisibleCommands = {{.SortKey = 8}, {.SortKey = 1}};
        const auto live = graph.CreateTexture(ColorDescriptor());
        const auto dead = graph.CreateTexture(ColorDescriptor());
        {
            struct PassData {};
            auto pass = graph.AddRasterPass("dead", PassData{}, +[](const PassData&, RDGRasterPassContext&) noexcept { return false; });
            pass.UseMeshPass({7}).WriteColor(0, dead, LoadAction::Clear, StoreAction::Store);
        };
        {
            struct PassData {
                decltype(&Events) EventsRef;
            };
            auto pass = graph.AddRasterPass("live", PassData{&Events}, +[](const PassData& data, RDGRasterPassContext& ctx) noexcept {
            auto& Events = *data.EventsRef;
            Events.push_back("execute");
            const auto& commands = ctx.GetVisibleCommands({3});
            EXPECT_EQ(commands[0].SortKey, 1);
            EXPECT_EQ(commands[1].SortKey, 8);
            EXPECT_EQ(ctx.GetVisibleCommands({7})[0].SortKey, 8);
            return true; });
            pass.UseMeshPass({3}).UseMeshPass({3}).WriteColor(0, live, LoadAction::Clear, StoreAction::Store);
        };
        if (Reorder) {
            const auto other = graph.CreateTexture(ColorDescriptor());
            {
                struct PassData {
                    decltype(&Events) EventsRef;
                };
                auto pass = graph.AddRasterPass("other", PassData{&Events}, +[](const PassData& data, RDGRasterPassContext&) noexcept {
            auto& Events = *data.EventsRef; Events.push_back("other"); return true; });
                pass.UseMeshPass({1}).WriteColor(0, other, LoadAction::Clear, StoreAction::Store).NeverCull();
            };
            {
                struct PassData {
                    decltype(&Events) EventsRef;
                };
                auto pass = graph.AddRasterPass("tail", PassData{&Events}, +[](const PassData& data, RDGRasterPassContext&) noexcept {
            auto& Events = *data.EventsRef; Events.push_back("tail"); return true; });
                pass.UseMeshPass({2}).WriteColor(0, live, LoadAction::Load, StoreAction::Store);
            };
        }
        graph.MarkOutput(live);
    }
    void ExtractMeshPass(MeshPassId pass, FrameSlot&, MeshPassFrameDrawList& list) noexcept override {
        Events.push_back("mesh");
        Extracted.push_back(pass);
        list.VisibleCommands = {{.SortKey = 8}, {.SortKey = 1}};
    }
    void SubmitFrame(FrameSlot&, CommandBuffer&) noexcept override { Events.push_back("submit"); }
};

TEST(RenderGraphFramePipeline, ExtractsAndSortsOnlyAliveMeshPassesBeforeExecution) {
    Probe trace;
    auto device = make_shared<ProbeDevice>(trace);
    DeviceContext context{device, &device->Queue};
    auto owner = make_unique<FilteringPipeline>(context);
    auto* pipeline = owner.get();
    auto scheduler = FrameScheduler::Create(context, std::move(owner), 1);
    ASSERT_TRUE(scheduler);
    Scene scene;
    scheduler->RunFrame();
    EXPECT_EQ(pipeline->Events, (vector<string>{"frame", "setup", "mesh", "execute", "submit"}));
    EXPECT_EQ(pipeline->Extracted, (vector<MeshPassId>{{3}}));
    EXPECT_EQ(trace.TextureAllocations.size(), 1);
}

TEST(RenderGraphFramePipeline, ExtractionKeepsDeclarationOrderWhenGpuPassesReorder) {
    Probe trace;
    auto device = make_shared<ProbeDevice>(trace);
    DeviceContext context{device, &device->Queue};
    auto owner = make_unique<FilteringPipeline>(context);
    auto* pipeline = owner.get();
    pipeline->Reorder = true;
    auto scheduler = FrameScheduler::Create(context, std::move(owner), 1);
    ASSERT_TRUE(scheduler);
    Scene scene;
    scheduler->RunFrame();
    EXPECT_EQ(pipeline->Extracted, (vector<MeshPassId>{{3}, {1}, {2}}));
    EXPECT_EQ(pipeline->Events, (vector<string>{"frame", "setup", "mesh", "mesh", "mesh", "execute", "tail", "other", "submit"}));
    EXPECT_EQ(trace.RasterScopes.size(), 2);
}

TEST(RenderGraphFramePipeline, SetupAndExecuteBorrowCurrentSceneUntilRecordCompletes) {
    Probe trace;
    auto device = make_shared<ProbeDevice>(trace);
    DeviceContext context{device, &device->Queue};
    Scene scene;
    const auto primitive = scene.AddPrimitive();
    auto owner = make_unique<FilteringPipeline>(context);
    vector<uint32_t> recordedCounts;
    owner->OnSetup = [&](RDGBuilder& graph) {
        ASSERT_TRUE(scene.GetPrimitive(primitive));
        {
            struct PassData {
                decltype(&scene) SceneRef;
                decltype(&recordedCounts) RecordedCountsRef;
            };
            auto pass = graph.AddCopyPass("borrow scene", PassData{&scene, &recordedCounts}, +[](const PassData& data, RDGCopyPassContext&) noexcept {
            auto& scene = *data.SceneRef; auto& recordedCounts = *data.RecordedCountsRef;
            recordedCounts.push_back(scene.GetPrimitiveCount());
            return true; });
            pass.NeverCull();
        };
    };
    auto scheduler = FrameScheduler::Create(context, std::move(owner), 1);
    ASSERT_TRUE(scheduler);
    scheduler->RunFrame();
    scene.AddPrimitive();
    scheduler->RunFrame();
    EXPECT_EQ(recordedCounts, (vector<uint32_t>{1, 2}));
    scene.Clear();
    scheduler->WaitIdle();
}

}  // namespace

TEST_F(RDGExecutionTest, ExternalOwnershipIsRecordedWithoutExecutionGroupsOrStateChanges) {
    FakeBuffer buffer{64, BufferUse::Vertex};
    ProbeTexture texture{ColorDescriptor()};
    Setup = [&](RDGBuilder& graph) {
        graph.MarkOutput(graph.ImportBuffer(&buffer, BufferState::Vertex, BufferState::Vertex, {true, true}));
        graph.MarkOutput(graph.ImportTexture(&texture, TextureState::ShaderRead, TextureState::ShaderRead, {true, true}));
    };
    Run();
    ASSERT_EQ(Trace.Barriers.size(), 4u);
    EXPECT_TRUE(Trace.Executed.empty());
    const auto& textureAcquire = std::get<BarrierTextureDescriptor>(Trace.Barriers[0]);
    const auto& bufferAcquire = std::get<BarrierBufferDescriptor>(Trace.Barriers[1]);
    const auto& textureRelease = std::get<BarrierTextureDescriptor>(Trace.Barriers[2]);
    const auto& bufferRelease = std::get<BarrierBufferDescriptor>(Trace.Barriers[3]);
    EXPECT_EQ(textureAcquire.ExternalTransfer, ExternalOwnershipTransfer::Acquire);
    EXPECT_EQ(bufferAcquire.ExternalTransfer, ExternalOwnershipTransfer::Acquire);
    EXPECT_EQ(textureRelease.ExternalTransfer, ExternalOwnershipTransfer::Release);
    EXPECT_EQ(bufferRelease.ExternalTransfer, ExternalOwnershipTransfer::Release);
    EXPECT_EQ(textureAcquire.Before, textureAcquire.After);
    EXPECT_EQ(bufferAcquire.Before, bufferAcquire.After);
    EXPECT_EQ(Trace.BarrierBatches, (vector<size_t>{2, 2}));
}

TEST_F(RDGExecutionTest, ExternalOwnershipAcquiresOnceBeforeFirstAccessAndReleasesAtGraphEnd) {
    FakeBuffer buffer{64, BufferUse::Vertex | BufferUse::CopySource};
    ProbeTexture texture{ColorDescriptor()};
    Setup = [&](RDGBuilder& graph) {
        const auto b = graph.ImportBuffer(&buffer, BufferState::Vertex, BufferState::Vertex, {true, true});
        const auto t = graph.ImportTexture(&texture, TextureState::ShaderRead, TextureState::ShaderRead, {true, true});
        for (uint32_t i = 0; i < 2; ++i) {
            {
                struct PassData {
                    decltype(&Trace) TraceRef;
                };
                auto pass = graph.AddCopyPass("read", PassData{&Trace}, +[](const PassData& data, RDGCopyPassContext&) noexcept {
            auto& Trace = *data.TraceRef;
                EXPECT_EQ(Trace.Barriers.size(), 2u);
                EXPECT_EQ(std::get<BarrierBufferDescriptor>(Trace.Barriers[0]).ExternalTransfer, ExternalOwnershipTransfer::Acquire);
                EXPECT_EQ(std::get<BarrierTextureDescriptor>(Trace.Barriers[1]).ExternalTransfer, ExternalOwnershipTransfer::Acquire);
                return true; });
                pass.ReadBuffer(b).ReadTexture(t).NeverCull();
            };
        }
    };
    Run();
    ASSERT_EQ(Trace.Barriers.size(), 6u);
    EXPECT_EQ(std::get<BarrierTextureDescriptor>(Trace.Barriers[4]).ExternalTransfer, ExternalOwnershipTransfer::Release);
    EXPECT_EQ(std::get<BarrierBufferDescriptor>(Trace.Barriers[5]).ExternalTransfer, ExternalOwnershipTransfer::Release);
}

TEST_F(RDGExecutionTest, CulledExternalResourcesDoNotTransferOwnership) {
    FakeBuffer buffer{64, BufferUse::Vertex};
    Setup = [&](RDGBuilder& graph) { graph.ImportBuffer(&buffer, BufferState::Vertex, std::nullopt, {true, true}); };
    Run();
    EXPECT_TRUE(Trace.Barriers.empty());
}

TEST_F(RDGExecutionTest, ExternalOwnershipWithoutFinalStateStillTransfersBothDirections) {
    FakeBuffer buffer{64, BufferUse::Vertex};
    Setup = [&](RDGBuilder& graph) { graph.MarkOutput(graph.ImportBuffer(&buffer, BufferState::Vertex, std::nullopt, {true, true})); };
    Run();
    ASSERT_EQ(Trace.Barriers.size(), 2u);
    EXPECT_EQ(std::get<BarrierBufferDescriptor>(Trace.Barriers[0]).ExternalTransfer, ExternalOwnershipTransfer::Acquire);
    EXPECT_EQ(std::get<BarrierBufferDescriptor>(Trace.Barriers[1]).ExternalTransfer, ExternalOwnershipTransfer::Release);
}

TEST(RDG, DuplicateImportsRequireConsistentOwnership) {
    FakeBuffer buffer{64, BufferUse::Vertex};
    FakeGpu gpu;
    auto scheduler = FrameScheduler::Create(gpu.Context, make_unique<CallbackPipeline>(), 1).Unwrap();
    RDGBuilder graph{scheduler->BeginFrame()};
    graph.ImportBuffer(&buffer, BufferState::Vertex, std::nullopt, {true, false});
    graph.ImportBuffer(&buffer, BufferState::Vertex);
    EXPECT_FALSE(graph.Compile());
}

TEST_F(RDGExecutionTest, SameStateGroupReadStillAcquiresExternalOwnership) {
    FakeBuffer buffer{64, BufferUse::CopySource};
    Setup = [&](RDGBuilder& graph) {
        const auto handle = graph.ImportBuffer(&buffer, BufferState::CopySource, std::nullopt, {true, false});
        {
            struct PassData {
                decltype(&Trace) TraceRef;
            };
            auto pass = graph.AddCopyPass("read", PassData{&Trace}, +[](const PassData& data, RDGCopyPassContext&) noexcept {
            auto& Trace = *data.TraceRef;
            EXPECT_EQ(Trace.Barriers.size(), 1u);
            return true; });
            pass.ReadBuffer(handle).NeverCull();
        };
    };
    Run();
    ASSERT_EQ(Trace.Barriers.size(), 1u);
    const auto& barrier = std::get<BarrierBufferDescriptor>(Trace.Barriers[0]);
    EXPECT_EQ(barrier.Before, barrier.After);
    EXPECT_EQ(barrier.ExternalTransfer, ExternalOwnershipTransfer::Acquire);
}

TEST_F(RDGExecutionTest, BorrowConsumesAtPrepareAndRecordUsesFrozenBytes) {
    ByteBuffer target{{.Size = 4, .Usage = BufferUse::CopyDestination}, Trace};
    array<byte, 4> bytes{byte{1}, byte{2}, byte{3}, byte{4}};
    Setup = [&](RDGBuilder& graph) {
        const auto dst = graph.ImportBuffer(&target, BufferState::Common);
        graph.UploadBuffer(dst, 0, RDGUploadSource::BorrowedBytes(bytes));
        graph.MarkOutput(dst);
    };
    AfterCompile = [&] { EXPECT_EQ(Trace.Maps, 0u); bytes[0] = byte{9}; };
    AfterPrepare = [&] { bytes.fill(byte{0}); EXPECT_EQ(Trace.Copies, 0u); };
    Run();
    EXPECT_EQ(target.Bytes, (vector<byte>{byte{9}, byte{2}, byte{3}, byte{4}}));
}

TEST_F(RDGExecutionTest, ScratchReuseAfterPrepareKeepsRecordedUploadsAndBarriers) {
    ByteBuffer target{{.Size = 64, .Usage = BufferUse::CopyDestination}, Trace};
    const array<byte, 4> bytes{byte{1}, byte{2}, byte{3}, byte{4}};
    Setup = [&](RDGBuilder& graph) {
        const auto dst = graph.ImportBuffer(&target, BufferState::Common, BufferState::Common);
        for (uint32_t i = 0; i < 16; ++i) graph.UploadBuffer(dst, i * bytes.size(), RDGUploadSource::BorrowedBytes(bytes));
        graph.MarkOutput(dst);
    };
    AfterPrepare = [&] {
        auto& scratch = Scheduler->GetCurrentSlot()->GetScratchMemoryResource();
        for (size_t size = 16; size <= 4096; size *= 2) {
            std::pmr::vector<std::pmr::vector<byte>> overwrite{&scratch};
            for (uint32_t i = 0; i < 64; ++i) overwrite.emplace_back(size, byte{0xff});
        }
    };
    Run();
    EXPECT_EQ(Trace.Copies, 16u);
    for (size_t i = 0; i < target.Bytes.size(); ++i) EXPECT_EQ(target.Bytes[i], bytes[i % bytes.size()]);
    bool entersCopy = false, exitsCopy = false;
    for (const auto& barrier : Trace.Barriers) {
        const auto* buffer = std::get_if<BarrierBufferDescriptor>(&barrier);
        if (!buffer || buffer->Target != &target) continue;
        entersCopy |= buffer->Before == BufferState::Common && buffer->After == BufferState::CopyDestination;
        exitsCopy |= buffer->Before == BufferState::CopyDestination && buffer->After == BufferState::Common;
    }
    EXPECT_TRUE(entersCopy);
    EXPECT_TRUE(exitsCopy);
}

TEST_F(RDGExecutionTest, OwnedSourceMovesAllocationAndSnapshotsRemainExplicit) {
    ByteBuffer target{{.Size = 8, .Usage = BufferUse::CopyDestination}, Trace};
    vector<byte> owned{byte{1}, byte{2}, byte{3}, byte{4}};
    auto* source = owned.data();
    array<byte, 4> snapshot{byte{5}, byte{6}, byte{7}, byte{8}};
    Setup = [&](RDGBuilder& graph) {
        const auto dst = graph.ImportBuffer(&target, BufferState::Common);
        graph.UploadBuffer(dst, 0, RDGUploadSource::OwnedBytes(std::move(owned)));
        graph.UploadBuffer(dst, 4, RDGUploadSource::SnapshotBytes(snapshot));
        // 移交后的原地址仍指向来源存储；只在消费前验证没有复制。
        source[0] = byte{9};
        snapshot.fill(byte{0});
        graph.MarkOutput(dst);
    };
    Run();
    EXPECT_TRUE(owned.empty());
    EXPECT_EQ(target.Bytes, (vector<byte>{byte{9}, byte{2}, byte{3}, byte{4}, byte{5}, byte{6}, byte{7}, byte{8}}));
}

TEST_F(RDGExecutionTest, DeadBorrowAndGeneratorNeverPrepareOrAllocate) {
    uint32_t layouts = 0, writes = 0;
    struct Generator {
        uint32_t* Layouts;
        uint32_t* Writes;
    };
    array<byte, 64> bytes{};
    Setup = [&](RDGBuilder& graph) {
        graph.CreateBuffer(RDGDynamicBufferDescriptor{1024 * 1024, BufferUse::Vertex},
                           RDGUploadSource::Generated(1024 * 1024, Generator{&layouts, &writes}, +[](const Generator& data, RDGUploadWriteContext&) noexcept { ++*data.Writes; return true; }, +[](Generator& data, RDGUploadPrepareContext&) noexcept { ++*data.Layouts; return uint64_t{16}; }));
        const auto buffer = graph.CreateBuffer(BufferDescriptor{.Size = 64, .Usage = BufferUse::CopyDestination});
        graph.UploadBuffer(buffer, 0, RDGUploadSource::BorrowedBytes(bytes));
    };
    Run();
    EXPECT_EQ(layouts, 0u);
    EXPECT_EQ(writes, 0u);
    EXPECT_EQ(Trace.Maps, 0u);
    EXPECT_EQ(Trace.Copies, 0u);
    EXPECT_TRUE(Trace.BufferAllocations.empty());
}

TEST_F(RDGExecutionTest, ActualDynamicSizeReservesPaddingAndSharesPhysicalReadState) {
    Device->Capabilities.Detail.CBufferAlignment = 256;
    Device->Capabilities.Limits.CBufferOffsetAlignment = 256;
    RDGBufferSlice constants, vertices;
    uint32_t layouts = 0, writes = 0;
    struct Generator {
        uint32_t* Layouts;
        uint32_t* Writes;
    };
    Setup = [&](RDGBuilder& graph) {
        const auto a = graph.CreateBuffer(RDGDynamicBufferDescriptor{1024 * 1024, BufferUse::CBuffer},
                                          RDGUploadSource::Generated(1024 * 1024, Generator{&layouts, &writes}, +[](const Generator& data, RDGUploadWriteContext& write) noexcept {
                    ++*data.Writes;
                    EXPECT_EQ(write.Bytes.size(), 80u);
                    std::fill(write.Bytes.begin(), write.Bytes.end(), byte{42});
                    return true; }, +[](Generator& data, RDGUploadPrepareContext&) noexcept { ++*data.Layouts; return uint64_t{80}; }));
        array<byte, 16> bytes{};
        const auto b = graph.CreateBuffer(RDGDynamicBufferDescriptor{16, BufferUse::Vertex}, RDGUploadSource::SnapshotBytes(bytes));
        const auto color = graph.CreateTexture(ColorDescriptor());
        struct Data {
            RDGBufferHandle A, B;
            RDGBufferSlice* Constants;
            RDGBufferSlice* Vertices;
        };
        graph.AddRasterPass("slices", Data{a, b, &constants, &vertices}, +[](const Data& data, RDGRasterPassContext& ctx) noexcept {
                EXPECT_EQ(ctx.ResolveBuffer(data.A).Size, 80u);
                return true; }, +[](Data& data, RDGPrepareContext& ctx) noexcept {
                *data.Constants = ctx.ResolveBuffer(data.A);
                *data.Vertices = ctx.ResolveBuffer(data.B);
                return true; }).ReadBuffer(a, BufferState::CBuffer).ReadBuffer(b, BufferState::Vertex).WriteColor(0, color, LoadAction::Clear, StoreAction::Store).NeverCull();
    };
    AfterCompile = [&] { EXPECT_EQ(layouts, 0u); EXPECT_EQ(writes, 0u); };
    Run();
    EXPECT_EQ(layouts, 1u);
    EXPECT_EQ(writes, 1u);
    EXPECT_EQ(constants.Size, 80u);
    EXPECT_EQ(Trace.Copies, 0u);
    for (const auto& allocation : Trace.BufferAllocations) EXPECT_LT(allocation.Size, 1024u * 1024u);
    if (constants.Buffer == vertices.Buffer) EXPECT_GE(vertices.Offset, constants.Offset + 256);
}

TEST_F(RDGExecutionTest, ZeroDynamicSliceKeepsRasterClearWithoutAllocatingBuffer) {
    Setup = [&](RDGBuilder& graph) {
        const auto instances = graph.CreateBuffer(RDGDynamicBufferDescriptor{4096, BufferUse::Vertex},
                                                  RDGUploadSource::Generated(4096, uint32_t{0}, +[](const uint32_t&, RDGUploadWriteContext&) noexcept { ADD_FAILURE(); return false; }, +[](uint32_t&, RDGUploadPrepareContext&) noexcept { return uint64_t{0}; }));
        const auto color = graph.CreateTexture(ColorDescriptor());
        graph.AddRasterPass("empty", instances, +[](const RDGBufferHandle& handle, RDGRasterPassContext& ctx) noexcept {
            EXPECT_EQ(ctx.ResolveBuffer(handle).Size, 0u);
            EXPECT_FALSE(ctx.ResolveBuffer(handle).Buffer);
            return true; }).ReadBuffer(instances, BufferState::Vertex).WriteColor(0, color, LoadAction::Clear, StoreAction::Store);
        graph.MarkOutput(color);
    };
    Run();
    EXPECT_TRUE(Trace.BufferAllocations.empty());
    EXPECT_EQ(Trace.Maps, 0u);
    EXPECT_EQ(Trace.Flushes, 0u);
    ASSERT_EQ(Trace.RasterScopes.size(), 1u);
    EXPECT_EQ(Trace.RasterScopes[0].Colors[0].Load, LoadAction::Clear);
}

TEST_F(RDGExecutionTest, UploadBatchesKeepAllMembersAndIntermediateConsumerOrdering) {
    ByteBuffer target{{.Size = 8, .Usage = BufferUse::CopyDestination | BufferUse::CopySource}, Trace};
    ByteBuffer observed{{.Size = 8, .Usage = BufferUse::CopyDestination}, Trace};
    Setup = [&](RDGBuilder& graph) {
        const auto dst = graph.ImportBuffer(&target, BufferState::Common);
        const auto out = graph.ImportBuffer(&observed, BufferState::Common);
        const array<byte, 4> a{byte{1}, byte{2}, byte{3}, byte{4}}, b{byte{5}, byte{6}, byte{7}, byte{8}}, c{byte{9}};
        graph.UploadBuffer(dst, 0, RDGUploadSource::SnapshotBytes(a));
        graph.UploadBuffer(dst, 4, RDGUploadSource::SnapshotBytes(b));
        struct Data {
            RDGBufferHandle In, Out;
        };
        graph.AddCopyPass("observe", Data{dst, out}, +[](const Data& data, RDGCopyPassContext& ctx) noexcept {
            const auto in = ctx.ResolveBuffer(data.In), out = ctx.ResolveBuffer(data.Out);
            ctx.GetCommandBuffer().CopyBufferToBuffer(out.Buffer.Get(), out.Offset, in.Buffer.Get(), in.Offset, in.Size);
            return true; }).ReadBuffer(dst).WriteBuffer(out).NeverCull();
        graph.UploadBuffer(dst, 0, RDGUploadSource::SnapshotBytes(c));
        graph.MarkOutput(dst);
    };
    Run();
    EXPECT_EQ(observed.Bytes, (vector<byte>{byte{1}, byte{2}, byte{3}, byte{4}, byte{5}, byte{6}, byte{7}, byte{8}}));
    EXPECT_EQ(target.Bytes[0], byte{9});
    EXPECT_EQ(Trace.Copies, 4u);
    EXPECT_EQ(Trace.DebugGroups, (vector<string>{"RDG UploadBatch", "observe", "RDG UploadBatch"}));
}

TEST_F(RDGExecutionTest, SmallBufferUploadsShareOneGpuScopeAndKeepOverlappingWrites) {
    ByteBuffer target{{.Size = 2048, .Usage = BufferUse::CopyDestination}, Trace};
    Setup = [&](RDGBuilder& graph) {
        const auto dst = graph.ImportBuffer(&target, BufferState::Common);
        for (uint32_t i = 0; i < 512; ++i) {
            const array bytes{static_cast<byte>(i & 255), byte{2}, byte{3}, byte{4}};
            graph.UploadBuffer(dst, i * 4, RDGUploadSource::SnapshotBytes(bytes));
        }
        const array patch{byte{91}, byte{92}};
        graph.UploadBuffer(dst, 1, RDGUploadSource::SnapshotBytes(patch));
        graph.MarkOutput(dst);
    };
    Run();
    EXPECT_EQ(Trace.Copies, 513u);
    EXPECT_EQ(Trace.DebugGroups, (vector<string>{"RDG UploadBatch"}));
    EXPECT_EQ(target.Bytes[1], byte{91});
    EXPECT_EQ(target.Bytes[2], byte{92});
    for (uint32_t i = 1; i < 512; ++i) {
        EXPECT_EQ(target.Bytes[i * 4], static_cast<byte>(i & 255));
        EXPECT_EQ(target.Bytes[i * 4 + 3], byte{4});
    }
}

TEST_F(RDGExecutionTest, SmallTextureUploadsShareOneGpuScope) {
    auto desc = ColorDescriptor();
    desc.Usage |= TextureUse::CopyDestination;
    ProbeTexture target{desc};
    Setup = [&](RDGBuilder& graph) {
        const auto dst = graph.ImportTexture(&target, TextureState::ShaderRead);
        const array<byte, 4> pixel{byte{19}};
        for (uint32_t i = 0; i < 512; ++i) {
            graph.UploadTexture(dst, {.X = i % 32, .Y = i / 32, .Width = 1, .Height = 1}, RDGTextureUploadSource::SnapshotBytes(pixel, 4));
        }
        graph.MarkOutput(dst);
    };
    Run();
    ASSERT_EQ(Trace.TextureCopies.size(), 512u);
    EXPECT_EQ(Trace.DebugGroups, (vector<string>{"RDG UploadBatch"}));
    for (uint32_t i = 0; i < 512; ++i) {
        EXPECT_EQ(Trace.TextureCopies[i].X, i % 32);
        EXPECT_EQ(Trace.TextureCopies[i].Y, i / 32);
    }
}

TEST_F(RDGExecutionTest, EmptyUploadGroupBalancesGpuScopeWithoutCopies) {
    ByteBuffer target{{.Size = 4, .Usage = BufferUse::CopyDestination}, Trace};
    Setup = [&](RDGBuilder& graph) {
        const auto dst = graph.ImportBuffer(&target, BufferState::Common);
        graph.UploadBuffer(dst, 0, RDGUploadSource::BorrowedBytes({}));
        graph.UploadBuffer(dst, 0, RDGUploadSource::Generated(4, uint32_t{0}, +[](const uint32_t&, RDGUploadWriteContext&) noexcept { ADD_FAILURE(); return false; }, +[](uint32_t&, RDGUploadPrepareContext&) noexcept { return uint64_t{0}; }));
        graph.MarkOutput(dst);
    };
    Run();
    EXPECT_EQ(Trace.Copies, 0u);
    EXPECT_EQ(Trace.DebugGroups, (vector<string>{"RDG UploadBatch"}));
    EXPECT_EQ(Trace.DebugDepth, 0u);
}

TEST_F(RDGExecutionTest, FailedTextureUploadClosesBatchAndSkipsRemainingWork) {
    auto desc = ColorDescriptor();
    desc.Usage |= TextureUse::CopyDestination;
    ProbeTexture target{desc};
    Trace.TextureCopiesUntilFailure = 1;
    Setup = [&](RDGBuilder& graph) {
        const auto dst = graph.ImportTexture(&target, TextureState::ShaderRead);
        const array<byte, 4> pixel{byte{19}};
        for (uint32_t i = 0; i < 3; ++i)
            graph.UploadTexture(dst, {.X = i, .Width = 1, .Height = 1}, RDGTextureUploadSource::SnapshotBytes(pixel, 4));
        graph.AddCopyPass("unreached", dst, +[](const RDGTextureHandle&, RDGCopyPassContext&) noexcept { ADD_FAILURE(); return true; }).ReadTexture(dst).NeverCull();
        graph.MarkOutput(dst);
    };
    ExpectSuccess = false;
    AfterRecord = [&] {
        EXPECT_EQ(Trace.DebugDepth, 0u);
        EXPECT_FALSE(Compiled->Record(*PreparedCommand));
        EXPECT_EQ(Trace.DebugDepth, 0u);
        PreparedCommand->PushDebugGroup("after failure");
        PreparedCommand->PopDebugGroup();
    };
    Run();
    EXPECT_EQ(Trace.TextureCopies.size(), 1u);
    EXPECT_EQ(Trace.DebugGroups, (vector<string>{"RDG UploadBatch", "after failure"}));
}

TEST_F(RDGExecutionTest, OwnedImportRetainsTargetImmediatelyAndOnlySubmissionPublishesState) {
    Device->EnableTextures = true;
    Device->Capabilities.Limits.MaxTexture2DDimension = 4096;
    Device->Capabilities.Limits.MaxTextureArrayLayers = 2048;
    auto owner = GpuTexture::Create(*Device, {.Width = 2, .Height = 2, .Format = TextureFormat::RGBA8_UNORM}).Unwrap();
    auto* resource = owner.Get();
    Setup = [&](RDGBuilder& graph) {
        const auto texture = graph.ImportPersistentOwned(owner);
        EXPECT_EQ(texture, graph.ImportPersistentOwned(owner));
        EXPECT_EQ(owner->UseCount(), 2u);
        owner.Reset();
        // 普通 CopyPass 同样关联持久提交状态；纹理不通过上传专用回调发布。
        graph.AddCopyPass("initialize", uint32_t{0}, +[](const uint32_t&, RDGCopyPassContext&) noexcept { return true; }).WriteTexture(texture);
        graph.MarkOutput(texture);
    };
    AfterPrepare = [&] { EXPECT_EQ(resource->GetSubmittedState(), TextureState::Undefined); };
    AfterRecord = [&] { EXPECT_EQ(resource->GetSubmittedState(), TextureState::Undefined); };
    Run();
    EXPECT_EQ(resource->GetSubmittedState(), TextureState::ShaderRead);
}

TEST_F(RDGExecutionTest, PublishedExternalWriteStateSynchronizesFirstReadAndRestoresBoundary) {
    auto buffer = GpuBuffer::Create(*Device, {.Size = 16, .Usage = BufferUse::UnorderedAccess}).Unwrap();
    EXPECT_EQ(buffer->GetSubmittedState(), BufferStates{BufferState::Common});
    auto& slot = Scheduler->BeginFrame();
    auto* command = slot.AcquireCommandBuffer().Unwrap();
    command->Begin();
    const ResourceBarrierDescriptor barrier = BarrierBufferDescriptor{buffer->GetBuffer(), BufferState::Common, BufferState::UnorderedAccess};
    command->ResourceBarrier(std::span{&barrier, 1});
    command->End();
    Device->Queue.Submit({.CmdBuffers = std::span{&command, 1}});
    buffer->CommitSubmittedState(BufferState::UnorderedAccess);
    Trace.Barriers.clear();

    Setup = [&](RDGBuilder& graph) {
        const auto input = graph.ImportPersistentOwned(buffer);
        EXPECT_EQ(input, graph.ImportPersistentBorrowed(*buffer));
        graph.AddComputePass("read external UAV", input, +[](const RDGBufferHandle&, RDGComputePassContext&) noexcept { return true; })
            .ReadBuffer(input, BufferState::UnorderedAccess).NeverCull();
    };
    AfterPrepare = AfterRecord = [&] { EXPECT_EQ(buffer->GetSubmittedState(), BufferStates{BufferState::UnorderedAccess}); };
    Run();

    ASSERT_EQ(Trace.Barriers.size(), 2u);
    ASSERT_TRUE(std::holds_alternative<BarrierUavDescriptor>(Trace.Barriers[0]));
    EXPECT_EQ(std::get<BarrierUavDescriptor>(Trace.Barriers[0]).Target, buffer->GetBuffer());
    const auto& end = std::get<BarrierBufferDescriptor>(Trace.Barriers[1]);
    EXPECT_EQ(end.Before, BufferStates{BufferState::UnorderedAccess});
    EXPECT_EQ(end.After, BufferStates{BufferState::Common});
    EXPECT_EQ(buffer->GetSubmittedState(), BufferStates{BufferState::Common});
}

TEST_F(RDGExecutionTest, GeneratorSizeFailurePreventsRecordAndStatePublication) {
    auto owner = GpuBuffer::Create(*Device, {.Size = 16}).Unwrap();
    Setup = [&](RDGBuilder& graph) {
        const auto dst = graph.ImportPersistentOwned(owner);
        graph.UploadBuffer(dst, 0, RDGUploadSource::Generated(16, uint32_t{0}, +[](const uint32_t&, RDGUploadWriteContext&) noexcept { ADD_FAILURE(); return false; }, +[](uint32_t&, RDGUploadPrepareContext&) noexcept { return uint64_t{17}; }));
        graph.MarkOutput(dst);
    };
    ExpectSuccess = false;
    Run();
    EXPECT_EQ(Trace.Copies, 0u);
    EXPECT_EQ(owner->GetSubmittedState(), BufferStates{BufferState::Common});
}

TEST_F(RDGExecutionTest, FailedRecordingDoesNotPublishPersistentTextureState) {
    Device->EnableTextures = true;
    Device->Capabilities.Limits.MaxTexture2DDimension = 4096;
    Device->Capabilities.Limits.MaxTextureArrayLayers = 2048;
    auto texture = GpuTexture::Create(*Device, {.Width = 2, .Height = 2, .Format = TextureFormat::RGBA8_UNORM}).Unwrap();
    Setup = [&](RDGBuilder& graph) {
        const auto image = graph.ImportPersistentOwned(texture);
        graph.AddCopyPass("failed writer", image, +[](const RDGTextureHandle&, RDGCopyPassContext&) noexcept { return false; }).WriteTexture(image);
        graph.MarkOutput(image);
    };
    ExpectSuccess = false;
    Run();
    EXPECT_TRUE(Prepared);
    EXPECT_FALSE(Recorded);
    EXPECT_EQ(texture->GetSubmittedState(), TextureState::Undefined);
}

TEST(RDGPersistentState, AbandonedRecordingKeepsCreationStateUntilLaterSubmission) {
    Probe trace;
    auto device = make_shared<ProbeDevice>(trace);
    device->EnableTextures = true;
    device->Capabilities.Limits.MaxTexture2DDimension = 4096;
    device->Capabilities.Limits.MaxTextureArrayLayers = 2048;
    DeviceContext context{device, &device->Queue};
    RDGTransientResourcePool pool{*device};
    UploadArena uploads{*device, 64};
    auto texture = GpuTexture::Create(*device, {.Width = 2, .Height = 2, .Format = TextureFormat::RGBA8_UNORM}).Unwrap();
    auto scheduler = FrameScheduler::Create(context, make_unique<CallbackPipeline>(), 1).Unwrap();
    for (bool submit : {false, true}) {
        auto& slot = scheduler->BeginFrame();
        pool.BeginFrame();
        uploads.Reset();
        RDGBuilder graph{slot};
        const auto image = graph.ImportPersistentOwned(texture);
        graph.AddCopyPass("initialize", image, +[](const RDGTextureHandle&, RDGCopyPassContext&) noexcept { return true; }).WriteTexture(image);
        graph.MarkOutput(image);
        auto compiled = graph.Compile();
        ASSERT_TRUE(compiled);
        ASSERT_TRUE(compiled->Prepare(slot, pool, uploads));
        EXPECT_EQ(texture->GetSubmittedState(), TextureState::Undefined);
        auto* command = slot.AcquireCommandBuffer().Unwrap();
        command->Begin();
        ASSERT_TRUE(compiled->Record(*command));
        command->End();
        EXPECT_EQ(texture->GetSubmittedState(), TextureState::Undefined);
        if (submit) {
            device->Queue.Submit({.CmdBuffers = std::span{&command, 1}});
            compiled->CommitSubmittedStates();
        }
        EXPECT_EQ(texture->GetSubmittedState(), submit ? TextureState::ShaderRead : TextureState::Undefined);
        scheduler->WaitIdle();
    }
}

TEST(RDGTransientPool, OnlySubmittedGraphsPublishPoolAndPageStates) {
    Probe trace;
    auto device = make_shared<ProbeDevice>(trace);
    DeviceContext context{device, &device->Queue};
    RDGTransientResourcePool pool{*device};
    UploadArena uploads{*device, 64};
    auto scheduler = FrameScheduler::Create(context, make_unique<CallbackPipeline>(), 1).Unwrap();
    const BufferDescriptor desc{.Size = 4, .Usage = BufferUse::CopyDestination};
    const array<byte, 4> bytes{byte{42}};
    for (bool submit : {false, true}) {
        auto& slot = scheduler->BeginFrame();
        pool.BeginFrame();
        uploads.Reset();
        RDGBuilder graph{slot};
        const auto dst = graph.CreateBuffer(desc);
        graph.UploadBuffer(dst, 0, RDGUploadSource::BorrowedBytes(bytes));
        graph.MarkOutput(dst);
        auto compiled = graph.Compile();
        ASSERT_TRUE(compiled);
        ASSERT_TRUE(compiled->Prepare(slot, pool, uploads));
        auto command = slot.AcquireCommandBuffer().Unwrap();
        command->Begin();
        ASSERT_TRUE(compiled->Record(*command));
        command->End();
        if (submit) {
            device->Queue.Submit({.CmdBuffers = std::span{&command, 1}});
            compiled->CommitSubmittedStates();
        }
        scheduler->WaitIdle();
        pool.BeginFrame();
        EXPECT_EQ(pool.AcquireBuffer(desc).LastState, submit ? BufferState::CopyDestination : BufferState::Undefined);
        vector<ResourceBarrierDescriptor> barriers;
        uploads.AppendReadBarriers(barriers);
        ASSERT_FALSE(barriers.empty());
        EXPECT_EQ(std::get<BarrierBufferDescriptor>(barriers[0]).Before, submit ? BufferState::CopySource : BufferState::HostWrite);
    }
}

TEST_F(RDGExecutionTest, FailedWriterSkipsPassPrepareAndRecord) {
    Setup = [&](RDGBuilder& graph) {
        const auto buffer = graph.CreateBuffer(RDGDynamicBufferDescriptor{16, BufferUse::CopySource},
                                               RDGUploadSource::Generated(16, uint32_t{0}, +[](const uint32_t&, RDGUploadWriteContext&) noexcept { return false; }));
        graph.AddCopyPass("must not run", uint32_t{0}, +[](const uint32_t&, RDGCopyPassContext&) noexcept { ADD_FAILURE(); return true; }, +[](uint32_t&, RDGPrepareContext&) noexcept { ADD_FAILURE(); return true; }).ReadBuffer(buffer).NeverCull();
    };
    ExpectSuccess = false;
    Run();
    EXPECT_FALSE(Prepared);
    EXPECT_FALSE(Recorded);
    EXPECT_EQ(Trace.Copies, 0u);
}

TEST(RDG, EmptyUploadDoesNotInitializeFixedBuffer) {
    FakeGpu gpu;
    auto scheduler = FrameScheduler::Create(gpu.Context, make_unique<CallbackPipeline>(), 1).Unwrap();
    RDGBuilder graph{scheduler->BeginFrame()};
    const auto target = graph.CreateBuffer(BufferDescriptor{.Size = 16, .Usage = BufferUse::CopyDestination | BufferUse::CopySource});
    graph.UploadBuffer(target, 0, RDGUploadSource::BorrowedBytes({}));
    graph.AddCopyPass("read", uint32_t{0}, +[](const uint32_t&, RDGCopyPassContext&) noexcept { return true; }).ReadBuffer(target).NeverCull();
    EXPECT_FALSE(graph.Compile());
}

TEST_F(RDGExecutionTest, AllViewsExistBeforePassBindingPreparation) {
    Setup = [&](RDGBuilder& graph) {
        for (uint32_t i = 0; i < 2; ++i) {
            const auto color = graph.CreateTexture(ColorDescriptor());
            graph.AddRasterPass("prepared views", &Trace, +[](Probe* const&, RDGRasterPassContext&) noexcept { return true; }, +[](Probe*& trace, RDGPrepareContext&) noexcept {
                    EXPECT_EQ(trace->Views.size(), 2u);
                    return true; }).WriteColor(0, color, LoadAction::Clear, StoreAction::Store).NeverCull();
        }
    };
    Run();
}

TEST(RDG, FrameResetReleasesOwnersAfterAbandonedFailedAndCulledGraphs) {
    Probe trace;
    ProbeDevice device{trace};
    auto gpuOwner = GpuBuffer::Create(device, {.Size = 4}).Unwrap();
    HeapMemoryResource heap;
    ContainerProbe upstream{heap};
    FakeGpu gpu;
    auto scheduler = FrameScheduler::Create(gpu.Context, make_unique<CallbackPipeline>(), 1, &upstream).Unwrap();
    for (uint32_t mode = 0; mode < 3; ++mode) {
        auto passOwner = make_shared<uint32_t>(1);
        auto sourceOwner = make_shared<uint32_t>(2);
        weak_ptr<uint32_t> passLifetime = passOwner, sourceLifetime = sourceOwner;
        {
            RDGBuilder graph{scheduler->BeginFrame()};
            graph.AddCopyPass("culled owner", std::move(passOwner), +[](const shared_ptr<uint32_t>&, RDGCopyPassContext&) noexcept { return true; });
            const auto target = graph.ImportPersistentOwned(gpuOwner);
            graph.UploadBuffer(target, 0, RDGUploadSource::Generated(4, std::move(sourceOwner), +[](const shared_ptr<uint32_t>&, RDGUploadWriteContext&) noexcept { ADD_FAILURE(); return false; }));
            if (mode == 1) {
                graph.CreateBuffer(BufferDescriptor{.Size = 0});
                EXPECT_FALSE(graph.Compile());
            } else if (mode == 2) {
                EXPECT_TRUE(graph.Compile());
            }
        }
        EXPECT_FALSE(passLifetime.expired());
        EXPECT_FALSE(sourceLifetime.expired());
        EXPECT_EQ(gpuOwner->UseCount(), 2u);
        EXPECT_GT(upstream.LiveBytes, 0u);
        scheduler->WaitIdle();
        EXPECT_TRUE(passLifetime.expired());
        EXPECT_TRUE(sourceLifetime.expired());
        EXPECT_EQ(gpuOwner->UseCount(), 1u);
    }
    RDGBuilder graph{scheduler->BeginFrame()};
    EXPECT_TRUE(graph.Compile());
    scheduler.reset();
    EXPECT_EQ(upstream.LiveBytes, 0u);
}

TEST_F(RDGExecutionTest, ReusedSlotDoesNotRetainPreviousGraphWork) {
    const array<byte, 4> bytes{byte{42}};
    uint32_t expectedCopies = 0;
    for (uint32_t count : {1024u, 3u, 0u, 4096u, 1u}) {
        Setup = [&](RDGBuilder& graph) {
            graph.AddCopyPass("frame marker", &Trace, +[](Probe* const& trace, RDGCopyPassContext&) noexcept {
                trace->Executed.emplace_back("marker");
                return true;
            }).NeverCull();
            for (uint32_t i = 0; i < count; ++i) {
                const auto target = graph.CreateBuffer(BufferDescriptor{.Size = 4, .Usage = BufferUse::CopyDestination});
                auto source = i % 2 ? RDGUploadSource::SnapshotBytes(bytes) : RDGUploadSource::BorrowedBytes(bytes);
                graph.UploadBuffer(target, 0, std::move(source));
                graph.MarkOutput(target);
            }
        };
        Run();
        expectedCopies += count;
        EXPECT_EQ(Trace.Copies, expectedCopies);
    }
    EXPECT_EQ(Trace.Executed, (vector<string>{"marker", "marker", "marker", "marker", "marker"}));
}

TEST_F(RDGExecutionTest, GeneratedContextMovesAndIsDestroyedAfterPrepareExactlyOnce) {
    class Context {
    public:
        explicit Context(uint32_t& destroyed) noexcept : Destroyed(destroyed) {}
        ~Context() noexcept { ++Destroyed; }
        uint32_t& Destroyed;
        uint32_t Value{42};
    };
    uint32_t destroyed = 0;
    ByteBuffer target{{.Size = sizeof(uint32_t), .Usage = BufferUse::CopyDestination}, Trace};
    auto context = make_unique<Context>(destroyed);
    auto source = RDGUploadSource::Generated(sizeof(uint32_t), std::move(context),
                                            +[](const unique_ptr<Context>& value, RDGUploadWriteContext& output) noexcept {
                                                std::memcpy(output.Bytes.data(), &value->Value, sizeof(value->Value));
                                                return true;
                                            },
                                            +[](unique_ptr<Context>& value, RDGUploadPrepareContext&) noexcept {
                                                ++value->Value;
                                                return uint64_t{sizeof(value->Value)};
                                            });
    EXPECT_FALSE(context);
    RDGUploadSource moved;
    moved = std::move(source);
    Setup = [&](RDGBuilder& graph) {
        const auto destination = graph.ImportBuffer(&target, BufferState::Common);
        graph.UploadBuffer(destination, 0, std::move(moved));
        graph.MarkOutput(destination);
    };
    AfterCompile = [&] { EXPECT_EQ(destroyed, 0u); };
    AfterPrepare = [&] { EXPECT_EQ(destroyed, 1u); };
    Run();
    uint32_t actual = 0;
    std::memcpy(&actual, target.Bytes.data(), sizeof(actual));
    EXPECT_EQ(actual, 43u);
    Scheduler->WaitIdle();
    source.ReleaseSource();
    moved.ReleaseSource();
    EXPECT_EQ(destroyed, 1u);
}
