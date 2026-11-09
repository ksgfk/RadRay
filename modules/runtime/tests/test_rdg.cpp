#include <gtest/gtest.h>

#include <radray/logger.h>

#include <algorithm>
#include <cstring>

#include <radray/runtime/rdg.h>
#include <radray/runtime/render_graph_pipeline.h>
#include <radray/runtime/geometry_buffer_pool.h>
#include <radray/runtime/upload_buffer.h>

#include "test_fakes.h"

using namespace radray;
using namespace radray::test;
using namespace radray::render;

namespace {

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
    uint32_t Maps{0};
    uint32_t Copies{0};
    uint32_t OpenEncoders{0};
};

class ByteBuffer final : public FakeBuffer {
public:
    ByteBuffer(BufferDescriptor desc, Probe& probe) : FakeBuffer(desc.Size, desc.Usage), Bytes(desc.Size), _probe(probe) { Desc = desc; }
    void* Map(uint64_t offset, uint64_t size) noexcept override {
        ++_probe.Maps;
        return offset <= Bytes.size() && size <= Bytes.size() - offset ? Bytes.data() + offset : nullptr;
    }
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
    void PopDebugGroup() noexcept override { --_probe.DebugDepth; }
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
        pipeline->OnExtract = [&](const Scene&, FrameSlot& slot) {
            const auto buffers = Trace.BufferAllocations.size();
            const auto textures = Trace.TextureAllocations.size();
            RDGBuilder graph{slot.GetArena()};
            Setup(graph);
            Compiled = graph.Compile();
            ASSERT_TRUE(Compiled);
            EXPECT_EQ(Trace.BufferAllocations.size(), buffers);
            EXPECT_EQ(Trace.TextureAllocations.size(), textures);
        };
        pipeline->OnRecord = [&](const Scene&, FrameSlot& slot) {
            if (!Compiled) return;
            Pool.BeginFrame();
            auto command = slot.AcquireCommandBuffer();
            ASSERT_TRUE(command);
            command->Begin();
            EXPECT_EQ(Compiled->Execute(slot, Pool, *command), ExpectSuccess);
            command->End();
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
    void Run() { Scheduler->RunFrame(Source); }
    Probe Trace;
    shared_ptr<ProbeDevice> Device{make_shared<ProbeDevice>(Trace)};
    DeviceContext Context{Device, &Device->Queue};
    RDGTransientResourcePool Pool{*Device};
    Scene Source;
    unique_ptr<FrameScheduler> Scheduler;
    std::function<void(RDGBuilder&)> Setup;
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
            graph.AddRasterPass(radray::format("raster{}", i), [&, i](RDGRasterPassBuilder& pass) {
                const auto load = i == 0 ? LoadAction::Clear : LoadAction::Load;
                const auto store = i == 2 ? StoreAction::Discard : StoreAction::Store;
                pass.WriteColor(0, color, load, store, {float(i) * 0.25f, 0.5f, 0.75f, 1.0f})
                    .SetDepthStencil(depth, TextureState::DepthWrite, load, store, load, store, {0.5f + float(i) * 0.1f, uint8_t(i + 3)})
                    .ReadBuffer(i == 1 ? constants1 : constants0, BufferState::CBuffer); }, [&, i](RDGRasterPassContext& ctx) {
                auto& encoder = static_cast<RecordingEncoder&>(ctx.GetEncoder());
                EXPECT_FALSE(encoder.Stencils.empty());
                if (!encoder.Stencils.empty()) EXPECT_EQ(encoder.Stencils.back(), 0);
                encoder.SetStencilReference(7);
                EXPECT_EQ(Trace.BarrierBatches, (vector<size_t>{4}));
                Trace.Executed.emplace_back(radray::format("raster{}", i));
                return true; });
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
    ObjectArena arena;
    RDGBuilder graph{arena};
    const auto live = graph.CreateTexture(ColorDescriptor());
    const auto dead = graph.CreateTexture(ColorDescriptor());
    graph.AddRasterPass("dead", [&](RDGRasterPassBuilder& pass) { pass.UseMeshPass({2}).WriteColor(0, dead, LoadAction::Clear, StoreAction::Store); }, [](RDGRasterPassContext&) { return true; });
    graph.AddRasterPass("live", [&](RDGRasterPassBuilder& pass) { pass.UseMeshPass({3}).UseMeshPass({1}).UseMeshPass({3}).WriteColor(0, live, LoadAction::Clear, StoreAction::Store); }, [](RDGRasterPassContext&) { return true; });
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
    ObjectArena arena;
    RDGBuilder graph{arena};
    const auto dst = graph.ImportBuffer(&buffer, BufferState::HostWrite);
    const array<byte, 8> bytes{};
    graph.UploadBuffer(dst, 0, bytes);
    graph.UploadBuffer(dst, 4, bytes);
    graph.MarkOutput(dst);

    EXPECT_FALSE(graph.Compile());
}

TEST(RDG, RejectsAttachmentViewOfAnotherResource) {
    ProbeTexture texture{ColorDescriptor()};
    ProbeTexture other{ColorDescriptor()};
    ProbeView view{{&other, TextureDimension::Dim2D, TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, TextureViewUsage::RenderTarget}};
    ObjectArena arena;
    RDGBuilder graph{arena};
    const auto color = graph.ImportTexture(&texture, TextureState::Undefined);
    graph.AddRasterPass("invalid view", [&](RDGRasterPassBuilder& pass) { pass.WriteColor(0, color, LoadAction::Clear, StoreAction::Store, {}, &view).NeverCull(); }, [](RDGRasterPassContext&) { return true; });
    EXPECT_FALSE(graph.Compile());
}

TEST(RDG, RejectsIncompatibleAttachmentDimensions) {
    ObjectArena arena;
    RDGBuilder graph{arena};
    const auto a = graph.CreateTexture(ColorDescriptor());
    auto smaller = ColorDescriptor();
    smaller.Width /= 2;
    const auto b = graph.CreateTexture(smaller);
    graph.AddRasterPass("invalid extent", [&](RDGRasterPassBuilder& pass) { pass.WriteColor(0, a, LoadAction::Clear, StoreAction::Store).WriteColor(1, b, LoadAction::Clear, StoreAction::Store).NeverCull(); }, [](RDGRasterPassContext&) { return true; });
    EXPECT_FALSE(graph.Compile());
}

TEST(RDG, RejectsClearOfReadOnlyDepthAttachment) {
    auto descriptor = ColorDescriptor();
    descriptor.Format = TextureFormat::D32_FLOAT;
    descriptor.Usage = TextureUse::DepthStencilRead;
    ProbeTexture texture{descriptor};
    ObjectArena arena;
    RDGBuilder graph{arena};
    const auto depth = graph.ImportTexture(&texture, TextureState::DepthRead);
    graph.AddRasterPass("read-only clear", [&](RDGRasterPassBuilder& pass) { pass.SetDepthStencil(depth, TextureState::DepthRead, LoadAction::Clear, StoreAction::Store).NeverCull(); }, [](RDGRasterPassContext&) { return true; });
    EXPECT_FALSE(graph.Compile());
}

TEST_F(RDGExecutionTest, DeadPassesDoNotExecuteAllocateOrUpload) {
    ByteBuffer unused{{.Size = 16, .Memory = MemoryType::Upload, .Usage = BufferUse::MapWrite | BufferUse::CBuffer}, Trace};
    Setup = [&](RDGBuilder& graph) {
        const auto buffer = graph.ImportBuffer(&unused, BufferState::HostWrite, BufferState::HostWrite);
        const array<byte, 16> bytes{};
        graph.UploadBuffer(buffer, 0, bytes);
        const auto color = graph.CreateTexture(ColorDescriptor());
        graph.CreateBuffer({.Size = 128, .Usage = BufferUse::UnorderedAccess});
        graph.AddRasterPass("unused", [&](RDGRasterPassBuilder& pass) { pass.UseMeshPass({5}).ReadBuffer(buffer, BufferState::CBuffer).WriteColor(0, color, LoadAction::Clear, StoreAction::Store); }, [&](RDGRasterPassContext&) { Trace.Executed.push_back("dead"); return true; });
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
        graph.UploadBuffer(buffer, 0, bytes);
        bytes = {byte{5}, byte{6}, byte{7}, byte{8}};
        graph.UploadBuffer(buffer, 4, bytes);
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
        graph.UploadBuffer(buffer, 0, bytes);
        graph.UploadBuffer(buffer, 4, bytes);
        graph.AddComputePass("read", [&](RDGComputePassBuilder& pass) { pass.ReadBuffer(buffer).NeverCull(); }, [&, buffer](RDGComputePassContext& ctx) {
                                 EXPECT_EQ(&ctx.Resolve(buffer), &target);
                                 EXPECT_EQ(target.Bytes[7], byte{10});
                                 return true; });
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
    ObjectArena arena;
    RDGBuilder graph{arena};
    const auto buffer = graph.ImportBuffer(&target, BufferState::HostWrite);
    const array<byte, 4> bytes{};
    graph.UploadBuffer(buffer, 0, bytes);
    graph.AddComputePass("read", [&](RDGComputePassBuilder& pass) { pass.ReadBuffer(buffer).NeverCull(); }, [](RDGComputePassContext&) { return true; });
    graph.UploadBuffer(buffer, 4, bytes);
    graph.MarkOutput(buffer);
    EXPECT_FALSE(graph.Compile());
}

TEST_F(RDGExecutionTest, DeviceBufferCanBeReuploadedBetweenGpuReads) {
    ByteBuffer target{{.Size = 4, .Usage = BufferUse::CopyDestination | BufferUse::Resource}, Trace};
    Setup = [&](RDGBuilder& graph) {
        const auto buffer = graph.ImportBuffer(&target, BufferState::Undefined);
        const array bytes{byte{1}, byte{2}, byte{3}, byte{4}};
        graph.UploadBuffer(buffer, 0, bytes);
        graph.AddComputePass("first read", [&](RDGComputePassBuilder& pass) { pass.ReadBuffer(buffer).NeverCull(); }, [](RDGComputePassContext&) { return true; });
        graph.UploadBuffer(buffer, 0, bytes);
        graph.AddComputePass("second read", [&](RDGComputePassBuilder& pass) { pass.ReadBuffer(buffer).NeverCull(); }, [](RDGComputePassContext&) { return true; });
    };
    Run();
    EXPECT_EQ(Trace.Copies, 2);
}

TEST_F(RDGExecutionTest, InPlaceComputeKeepsUpstreamRasterAndResolvesUavView) {
    Setup = [&](RDGBuilder& graph) {
        const auto color = graph.CreateTexture(ColorDescriptor());
        graph.AddRasterPass("raster", [&](RDGRasterPassBuilder& pass) { pass.WriteColor(0, color, LoadAction::Clear, StoreAction::Store); }, [&](RDGRasterPassContext&) { Trace.Executed.push_back("raster"); return true; });
        graph.AddComputePass("post", [&](RDGComputePassBuilder& pass) { pass.ReadWriteTexture(color); }, [&, color, owner = make_unique<uint32_t>(9)](RDGComputePassContext& ctx) {
                                 EXPECT_EQ(*owner, 9);
                                 EXPECT_TRUE(ctx.ResolveView(color, TextureViewUsage::UnorderedAccess));
                                 EXPECT_FALSE(ctx.ResolveView(color, TextureViewUsage::Resource));
                                 Trace.Executed.push_back("compute");
                                 return true; });
        graph.AddCopyPass("readback", [&](RDGCopyPassBuilder& pass) { pass.ReadTexture(color).NeverCull(); }, [&, color](RDGCopyPassContext& ctx) {
                              EXPECT_EQ(ctx.Resolve(color).GetDesc().Width, 32);
                              Trace.Executed.push_back("copy");
                              return true; });
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
        graph.AddComputePass("dead writer", [&](RDGComputePassBuilder& pass) { pass.WriteTexture(color); }, [&](RDGComputePassContext&) { Trace.Executed.push_back("dead"); return true; });
        graph.AddRasterPass("clear", [&](RDGRasterPassBuilder& pass) { pass.WriteColor(0, color, LoadAction::Clear, StoreAction::Store); }, [&](RDGRasterPassContext&) { Trace.Executed.push_back("clear"); return true; });
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
            graph.AddRasterPass("attachments", [&, i](RDGRasterPassBuilder& pass) {
                const auto load = i == 0 ? LoadAction::Clear : LoadAction::Load;
                pass.WriteColor(0, color, load, StoreAction::Store)
                    .SetDepthStencil(depth, TextureState::DepthWrite, load, StoreAction::Store, load, StoreAction::Store); }, [](RDGRasterPassContext&) { return true; });
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
            graph.AddComputePass("uav sequence", [&, i](RDGComputePassBuilder& pass) {
                if (i == 1) pass.WriteBuffer(buffer).WriteTexture(color);
                else pass.ReadBuffer(buffer, BufferState::UnorderedAccess).ReadTexture(color, TextureState::UnorderedAccess);
                pass.NeverCull(); }, [](RDGComputePassContext&) { return true; });
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
        graph.AddComputePass("failure", [](RDGComputePassBuilder& pass) { pass.NeverCull(); }, [](RDGComputePassContext&) { return false; });
        graph.AddComputePass("later", [](RDGComputePassBuilder& pass) { pass.NeverCull(); }, [&](RDGComputePassContext&) { Trace.Executed.push_back("later"); return true; });
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
        graph.AddComputePass("allocation", [&](RDGComputePassBuilder& pass) { pass.WriteBuffer(buffer).NeverCull(); }, [&](RDGComputePassContext&) { Trace.Executed.push_back("unexpected"); return true; });
    };
    Run();
    EXPECT_TRUE(Trace.Executed.empty());
}

TEST_F(RDGExecutionTest, CombinesRepeatedReadOnlyBufferAccessesAndRestoresBoundary) {
    ByteBuffer input{{.Size = 16, .Usage = BufferUse::Vertex | BufferUse::Resource}, Trace};
    Setup = [&](RDGBuilder& graph) {
        const auto buffer = graph.ImportBuffer(&input, BufferState::Common, BufferState::Common);
        const auto color = graph.CreateTexture(ColorDescriptor());
        graph.AddRasterPass("read geometry", [&](RDGRasterPassBuilder& pass) {
            pass.WriteColor(0, color, LoadAction::Clear, StoreAction::Store).NeverCull();
            pass.ReadBuffer(buffer, BufferState::Vertex).ReadBuffer(buffer, BufferState::ShaderRead); }, [](RDGRasterPassContext&) { return true; });
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

TEST_F(RDGExecutionTest, PagedUploadKeepsEarlierPointersWritableAfterGrowthAndReusesPages) {
    ByteBuffer target{{.Size = 64, .Usage = BufferUse::CopyDestination}, Trace};
    auto upload = BatchedBufferUpload::Create(*Device, 16);
    ASSERT_TRUE(upload);
    auto first = upload->Acquire(&target, 0, 16);
    auto second = upload->Acquire(&target, 32, 32);
    ASSERT_TRUE(first && second);
    EXPECT_EQ(upload->GetPageCount(), 2u);
    std::memset(first.Get(), 0x35, 16);
    std::memset(second.Get(), 0x79, 32);
    const auto maps = Trace.Maps;
    Setup = [&](RDGBuilder& graph) { EXPECT_TRUE(upload->CommitToGraph(graph)); };
    Run();
    EXPECT_EQ(Trace.Maps, maps);
    EXPECT_EQ(Trace.Copies, 2u);
    EXPECT_TRUE(std::all_of(target.Bytes.begin(), target.Bytes.begin() + 16, [](byte value) { return value == byte{0x35}; }));
    EXPECT_TRUE(std::all_of(target.Bytes.begin() + 32, target.Bytes.end(), [](byte value) { return value == byte{0x79}; }));
    const auto& final = std::get<BarrierBufferDescriptor>(Trace.Barriers.back());
    EXPECT_EQ(final.Target, &target);
    EXPECT_EQ(final.After, BufferStates{BufferState::Common});
    Scheduler->BeginFrame();
    upload->Reset();
    EXPECT_TRUE(upload->Acquire(&target, 0, 16));
    EXPECT_EQ(Trace.Maps, maps);
    Scheduler->WaitIdle();
}

TEST_F(RDGExecutionTest, GeometryPoolsShareUploadBoundaryAcrossUploadOnlyAndDrawFrames) {
    const array layouts{VertexBufferLayout{0, 12, VertexStepMode::Vertex}};
    const array attributes{VertexInputAttribute{0, 0, "POSITION", 0, VertexFormat::FLOAT32X3, 0}};
    auto vertices = VertexBufferPool::Create(*Device, {.VertexStride = 12, .PageVertexCapacity = 3, .AdditionalUsage = BufferUse::Resource, .Layouts = layouts, .Attributes = attributes});
    auto indices = IndexBufferPool::Create(*Device, {.PageIndexCapacity = 3});
    auto upload = BatchedBufferUpload::Create(*Device, 16);
    ASSERT_TRUE(vertices && indices && upload);
    auto v = vertices->Allocate(3);
    auto i = indices->Allocate(3);
    ASSERT_TRUE(v && i);
    const array<byte, 36> vertexBytes{byte{42}};
    const array<uint32_t, 3> indexBytes{0, 1, 2};
    ASSERT_TRUE(upload->Stage(v->GetBuffer(), v->GetByteOffset(), vertexBytes.data(), sizeof(vertexBytes)));
    ASSERT_TRUE(upload->Stage(i->GetBuffer(), i->GetByteOffset(), indexBytes.data(), sizeof(indexBytes)));
    bool draw = false;
    Setup = [&](RDGBuilder& graph) {
        if (!draw) {
            EXPECT_TRUE(upload->CommitToGraph(graph));
            return;
        }
        const auto color = graph.CreateTexture(ColorDescriptor());
        graph.AddRasterPass("pooled geometry", [&](RDGRasterPassBuilder& pass) {
            pass.WriteColor(0, color, LoadAction::Clear, StoreAction::Store).NeverCull();
            vertices->AddPassReadAccess(graph, pass, BufferState::Vertex | BufferState::ShaderRead);
            indices->AddPassReadAccess(graph, pass); }, [](RDGRasterPassContext&) { return true; });
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
        ObjectArena arena;
        RDGBuilder graph{arena};
        FakeBuffer input{16, usage};
        const auto buffer = graph.ImportBuffer(&input, BufferState::Common);
        graph.AddComputePass("invalid", [&](RDGComputePassBuilder& pass) { pass.ReadBuffer(buffer, BufferState::Vertex | BufferState::ShaderRead).NeverCull(); }, [](RDGComputePassContext&) { return true; });
        EXPECT_FALSE(graph.Compile());
    }
}

TEST(RDG, ValidatesEveryReadOnlyBufferUsageAndCombinedImportBoundary) {
    const auto combined = BufferState::Vertex | BufferState::ShaderRead;
    for (const auto usage : {BufferUses{BufferUse::Vertex}, BufferUse::Vertex | BufferUse::Resource}) {
        ObjectArena arena;
        RDGBuilder graph{arena};
        FakeBuffer input{16, usage};
        const auto buffer = graph.ImportBuffer(&input, combined, combined);
        graph.AddRasterPass("geometry", [&](RDGRasterPassBuilder& pass) {
            pass.WriteColor(0, graph.CreateTexture(ColorDescriptor()), LoadAction::Clear, StoreAction::Store);
            pass.ReadBuffer(buffer, combined).NeverCull(); }, [](RDGRasterPassContext&) { return true; });
        EXPECT_EQ(bool{graph.Compile()}, usage.HasFlag(BufferUse::Resource));
    }
}

TEST(RDG, ComputeCombinesShaderConstantAndIndirectReads) {
    ObjectArena arena;
    RDGBuilder graph{arena};
    FakeBuffer input{16, BufferUse::Resource | BufferUse::CBuffer | BufferUse::Indirect};
    const auto buffer = graph.ImportBuffer(&input, BufferState::Common);
    graph.AddComputePass("combined reads", [&](RDGComputePassBuilder& pass) { pass.ReadBuffer(buffer).ReadBuffer(buffer, BufferState::CBuffer | BufferState::Indirect).NeverCull(); }, [](RDGComputePassContext&) { return true; });
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
    ObjectArena arena;
    RDGBuilder graph{arena};
    FakeBuffer input{16, BufferUse::Vertex | BufferUse::UnorderedAccess};
    const auto buffer = graph.ImportBuffer(&input, BufferState::Common);
    graph.AddRasterPass("invalid", [&](RDGRasterPassBuilder& pass) {
        pass.WriteColor(0, graph.CreateTexture(ColorDescriptor()), LoadAction::Clear, StoreAction::Store);
        pass.ReadBuffer(buffer, BufferState::Vertex | BufferState::UnorderedAccess).NeverCull(); }, [](RDGRasterPassContext&) { return true; });
    EXPECT_FALSE(graph.Compile());
}

TEST(RDG, RejectsInvalidResourceHandles) {
    ObjectArena arena;
    RDGBuilder graph{arena};
    graph.AddComputePass("invalid", [](RDGComputePassBuilder& pass) { pass.ReadBuffer({}).NeverCull(); }, [](RDGComputePassContext&) { return true; });
    EXPECT_FALSE(graph.Compile());
}

TEST(RDG, RejectsConflictingWholeResourceStates) {
    ObjectArena arena;
    RDGBuilder graph{arena};
    const auto buffer = graph.CreateBuffer({.Size = 4, .Usage = BufferUse::Resource | BufferUse::UnorderedAccess});
    graph.AddComputePass("invalid", [&](RDGComputePassBuilder& pass) { pass.WriteBuffer(buffer).ReadBuffer(buffer).NeverCull(); }, [](RDGComputePassContext&) { return true; });
    EXPECT_FALSE(graph.Compile());
}

TEST(RDG, RejectsUninitializedResourceReads) {
    ObjectArena arena;
    RDGBuilder graph{arena};
    const auto buffer = graph.CreateBuffer({.Size = 4, .Usage = BufferUse::Resource});
    graph.AddComputePass("invalid", [&](RDGComputePassBuilder& pass) { pass.ReadBuffer(buffer).NeverCull(); }, [](RDGComputePassContext&) { return true; });
    EXPECT_FALSE(graph.Compile());
}

TEST(RDG, RejectsAttachmentStatesInComputePasses) {
    ObjectArena arena;
    RDGBuilder graph{arena};
    const auto texture = graph.CreateTexture(ColorDescriptor());
    graph.AddComputePass("invalid", [&](RDGComputePassBuilder& pass) { pass.WriteTexture(texture, TextureState::RenderTarget).NeverCull(); }, [](RDGComputePassContext&) { return true; });
    EXPECT_FALSE(graph.Compile());
}

TEST(RDG, DuplicateImportsCannotDeclareDifferentPhysicalInitialStates) {
    FakeBuffer target{16, BufferUse::Resource | BufferUse::UnorderedAccess};
    ObjectArena arena;
    RDGBuilder graph{arena};
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
        graph.AddRasterPass("depth", [&](RDGRasterPassBuilder& pass) { pass.SetDepthStencil(depth, TextureState::DepthWrite, LoadAction::Clear, StoreAction::Store, LoadAction::Clear, StoreAction::Store); }, [&](RDGRasterPassContext&) { Trace.Executed.push_back("depth"); return true; });
        graph.AddRasterPass("color", [&](RDGRasterPassBuilder& pass) { pass.SetDepthStencil(depth, TextureState::DepthRead, LoadAction::Load, StoreAction::Store, LoadAction::Load, StoreAction::Store)
                                                                           .WriteColor(0, color, LoadAction::Clear, StoreAction::Store); }, [&](RDGRasterPassContext&) { Trace.Executed.push_back("color"); return true; });
        graph.MarkOutput(color);
    };
    Run();
    EXPECT_EQ(Trace.Executed, (vector<string>{"depth", "color"}));
    ASSERT_FALSE(Trace.Views.empty());
    EXPECT_EQ(Trace.Views[0].Usage, TextureViewUsage::DepthWrite);
    EXPECT_EQ(Trace.Views.back().Usage, TextureViewUsage::DepthRead);
}

TEST_F(RDGExecutionTest, ImportedAttachmentUsesExplicitViewAndSynchronizesSameStateEpilogue) {
    ProbeTexture target{ColorDescriptor()};
    ProbeView view{{&target, TextureDimension::Dim2D, TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, TextureViewUsage::RenderTarget}};
    Setup = [&](RDGBuilder& graph) {
        const auto color = graph.ImportTexture(&target, TextureState::Undefined, TextureState::RenderTarget);
        graph.AddRasterPass("external", [&](RDGRasterPassBuilder& pass) { pass.WriteColor(0, color, LoadAction::Clear, StoreAction::Store, {}, &view); }, [](RDGRasterPassContext&) { return true; });
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
        graph.AddRasterPass("first", [&](RDGRasterPassBuilder& pass) { pass.UseMeshPass({3}).WriteColor(0, color, LoadAction::Clear, StoreAction::Store); }, [&](RDGRasterPassContext&) { Trace.Executed.push_back("first"); return true; });
        graph.AddComputePass("independent", [](RDGComputePassBuilder& pass) { pass.NeverCull(); }, [&](RDGComputePassContext&) { Trace.Executed.push_back("compute"); return true; });
        graph.AddRasterPass("other", [&](RDGRasterPassBuilder& pass) { pass.UseMeshPass({1}).WriteColor(0, other, LoadAction::Clear, StoreAction::Store).NeverCull(); }, [&](RDGRasterPassContext&) { Trace.Executed.push_back("other"); return true; });
        const array bytes{byte{42}, byte{}, byte{}, byte{}};
        graph.UploadBuffer(buffer, 0, bytes);
        graph.AddRasterPass("last", [&](RDGRasterPassBuilder& pass) { pass.UseMeshPass({2}).UseMeshPass({3}).WriteColor(0, color, LoadAction::Load, StoreAction::Store).ReadBuffer(buffer, BufferState::CBuffer); }, [&, buffer](RDGRasterPassContext& ctx) {
            EXPECT_EQ(static_cast<ByteBuffer&>(ctx.Resolve(buffer)).Bytes[0], byte{42});
            Trace.Executed.push_back("last");
            return true; });
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
        graph.AddComputePass("reader", [&](RDGComputePassBuilder& pass) { pass.ReadBuffer(buffer).NeverCull(); }, [&](RDGComputePassContext&) { Trace.Executed.push_back("reader"); return true; });
        graph.AddCopyPass("overwrite", [&](RDGCopyPassBuilder& pass) { pass.WriteBuffer(buffer); }, [&](RDGCopyPassContext&) { Trace.Executed.push_back("overwrite"); return true; });
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
        graph.AddComputePass("open compute", [](RDGComputePassBuilder& pass) { pass.NeverCull(); }, [&](RDGComputePassContext&) { Trace.Executed.push_back("open"); return true; });
        graph.AddRasterPass("reader", [&](RDGRasterPassBuilder& pass) { pass.ReadTexture(source).WriteColor(0, color, LoadAction::Clear, StoreAction::Store).NeverCull(); }, [&](RDGRasterPassContext&) { Trace.Executed.push_back("reader"); return true; });
        graph.AddComputePass("overwrite", [&](RDGComputePassBuilder& pass) { pass.WriteTexture(source); }, [&](RDGComputePassContext&) { Trace.Executed.push_back("overwrite"); return true; });
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
        graph.AddComputePass("dead reader", [&](RDGComputePassBuilder& pass) { pass.ReadTexture(color); }, [&](RDGComputePassContext&) { Trace.Executed.push_back("dead"); return true; });
        graph.AddRasterPass("clear", [&](RDGRasterPassBuilder& pass) { pass.WriteColor(0, color, LoadAction::Clear, StoreAction::Store); }, [&](RDGRasterPassContext&) { Trace.Executed.push_back("clear"); return true; });
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
        graph.AddComputePass("producer", [&](RDGComputePassBuilder& pass) { pass.ReadBuffer(x).WriteBuffer(y); }, [&](RDGComputePassContext&) { Trace.Executed.push_back("producer"); return true; });
        graph.AddComputePass("consumer", [&](RDGComputePassBuilder& pass) { pass.WriteBuffer(x).ReadBuffer(y); }, [&](RDGComputePassContext&) { Trace.Executed.push_back("consumer"); return true; });
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
        graph.AddRasterPass("gate", [&](RDGRasterPassBuilder& pass) { pass.WriteColor(0, gate, LoadAction::Clear, StoreAction::Store); }, [&](RDGRasterPassContext&) { Trace.Executed.push_back("gate"); return true; });
        graph.AddComputePass("old writer", [&](RDGComputePassBuilder& pass) { pass.ReadTexture(gate).WriteTexture(x).WriteTexture(y); }, [&](RDGComputePassContext&) { Trace.Executed.push_back("old writer"); return true; });
        graph.AddCopyPass("new writer", [&](RDGCopyPassBuilder& pass) { pass.ReadTexture(gate).NeverCull(); }, [&](RDGCopyPassContext&) { Trace.Executed.push_back("copy"); return true; });
        graph.AddRasterPass("clear", [&](RDGRasterPassBuilder& pass) { pass.WriteColor(0, x, LoadAction::Clear, StoreAction::Store); }, [&](RDGRasterPassContext&) { Trace.Executed.push_back("clear"); return true; });
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
        graph.AddComputePass("write a", [&](RDGComputePassBuilder& pass) { pass.WriteBuffer(a); }, [&, a](RDGComputePassContext& ctx) { EXPECT_EQ(ctx.Resolve(a).GetDesc().Size, 4); EXPECT_EQ(Trace.BarrierBatches, (vector<size_t>{2})); return true; });
        graph.AddComputePass("write b", [&](RDGComputePassBuilder& pass) { pass.WriteTexture(b); }, [&, b](RDGComputePassContext& ctx) { EXPECT_TRUE(ctx.ResolveView(b, TextureViewUsage::UnorderedAccess)); return true; });
        graph.AddComputePass("update a", [&](RDGComputePassBuilder& pass) { pass.ReadWriteBuffer(a); }, [&](RDGComputePassContext&) { return true; });
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
            graph.AddRasterPass("raster", [&, i](RDGRasterPassBuilder& pass) {
                pass.WriteColor(0, color, i == 0 ? LoadAction::Clear : LoadAction::Load, StoreAction::Store);
                if (i != 1) pass.WriteBuffer(uav); }, [](RDGRasterPassContext&) { return true; });
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
        graph.AddRasterPass("vertex", [&](RDGRasterPassBuilder& pass) { pass.WriteColor(0, color, LoadAction::Clear, StoreAction::Store).ReadBuffer(buffer, BufferState::Vertex); }, [](RDGRasterPassContext&) { return true; });
        graph.AddRasterPass("constant", [&](RDGRasterPassBuilder& pass) { pass.WriteColor(0, color, LoadAction::Load, StoreAction::Store).ReadBuffer(buffer, BufferState::CBuffer); }, [](RDGRasterPassContext&) { return true; });
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
            graph.AddRasterPass("raster", [&, i](RDGRasterPassBuilder& pass) { pass.WriteColor(0, color, i == 2 ? LoadAction::Load : LoadAction::Clear, i == 1 ? StoreAction::Discard : StoreAction::Store).NeverCull(); }, [](RDGRasterPassContext&) { return true; });
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
        graph.AddRasterPass("write depth", [&](RDGRasterPassBuilder& pass) { pass.SetDepthStencil(depth, TextureState::DepthWrite, LoadAction::Clear, StoreAction::Store); }, [](RDGRasterPassContext&) { return true; });
        graph.AddRasterPass("read depth", [&](RDGRasterPassBuilder& pass) { pass.SetDepthStencil(depth, TextureState::DepthRead, LoadAction::Load, StoreAction::Store).NeverCull(); }, [](RDGRasterPassContext&) { return true; });
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
            graph.AddRasterPass("raster", [&, i](RDGRasterPassBuilder& pass) { pass.WriteColor(0, color, i == 0 ? LoadAction::Clear : LoadAction::Load, StoreAction::Store); }, [&, i](RDGRasterPassContext&) { Trace.Executed.emplace_back(radray::format("{}", i)); return i != 1; });
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
        graph.AddRasterPass("first view", [&](RDGRasterPassBuilder& pass) { pass.WriteColor(0, color, LoadAction::Clear, StoreAction::Store, {}, &first); }, [](RDGRasterPassContext&) { return true; });
        graph.AddRasterPass("second view", [&](RDGRasterPassBuilder& pass) { pass.WriteColor(0, color, LoadAction::Load, StoreAction::Store, {}, &second); }, [](RDGRasterPassContext&) { return true; });
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
        graph.AddRasterPass("clear depth", [&](RDGRasterPassBuilder& pass) { pass.SetDepthStencil(depth, TextureState::DepthWrite, LoadAction::Clear, StoreAction::Store); }, [](RDGRasterPassContext&) { return true; });
        graph.AddRasterPass("load depth", [&](RDGRasterPassBuilder& pass) { pass.SetDepthStencil(depth, TextureState::DepthWrite, LoadAction::Load, StoreAction::Store); }, [](RDGRasterPassContext&) { return true; });
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
        graph.AddRasterPass("clear both", [&](RDGRasterPassBuilder& pass) { pass.SetDepthStencil(depth, TextureState::DepthWrite, LoadAction::Clear, StoreAction::Store, LoadAction::Clear, StoreAction::Store); }, [](RDGRasterPassContext&) { return true; });
        graph.AddRasterPass("clear stencil", [&](RDGRasterPassBuilder& pass) { pass.SetDepthStencil(depth, TextureState::DepthWrite, LoadAction::Load, StoreAction::Store, LoadAction::Clear, StoreAction::Store); }, [](RDGRasterPassContext&) { return true; });
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
        graph.AddRasterPass("color", [&](RDGRasterPassBuilder& pass) { pass.WriteColor(0, color, LoadAction::Clear, StoreAction::Store); }, [](RDGRasterPassContext&) { return true; });
        graph.AddRasterPass("uav", [&](RDGRasterPassBuilder& pass) { pass.WriteColor(0, color, LoadAction::Load, StoreAction::Store).WriteTexture(uav); }, [uav](RDGRasterPassContext& ctx) { return bool(ctx.ResolveView(uav, TextureViewUsage::UnorderedAccess)); });
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
        graph.AddComputePass("reader", [&](RDGComputePassBuilder& pass) { pass.ReadBuffer(buffer).NeverCull(); }, [](RDGComputePassContext&) { return true; });
        graph.AddComputePass("writer", [&](RDGComputePassBuilder& pass) { pass.ReadBuffer(buffer).WriteTexture(uav); }, [](RDGComputePassContext&) { return true; });
        graph.AddComputePass("update", [&](RDGComputePassBuilder& pass) { pass.ReadWriteTexture(uav); }, [](RDGComputePassContext&) { return true; });
        graph.MarkOutput(uav);
    };
    Run();
    EXPECT_EQ(Trace.ComputeScopes, 2);
    EXPECT_EQ(Trace.BarrierBatches, (vector<size_t>{1, 1}));
    ASSERT_EQ(Trace.Barriers.size(), 2);
    EXPECT_TRUE(std::holds_alternative<BarrierUavDescriptor>(Trace.Barriers.back()));
}

class FilteringPipeline final : public RenderGraphFramePipeline {
public:
    explicit FilteringPipeline(DeviceContext& context) : RenderGraphFramePipeline(context, 1) {}
    void Wait(FrameSlot&) noexcept override {}
    vector<string> Events;
    vector<MeshPassId> Extracted;
    std::function<void(RDGBuilder&, const Scene&)> OnSetup;
    bool Reorder{false};

protected:
    void ExtractFrameData(const Scene&, FrameSlot&) noexcept override { Events.push_back("frame"); }
    void SetupGraph(RDGBuilder& graph, const Scene& source, FrameSlot& slot) noexcept override {
        Events.push_back("setup");
        if (OnSetup) OnSetup(graph, source);
        // 被裁剪的列表保持不变，包括其初始顺序。
        slot.GetPassDrawList({7}).VisibleCommands = {{.SortKey = 8}, {.SortKey = 1}};
        const auto live = graph.CreateTexture(ColorDescriptor());
        const auto dead = graph.CreateTexture(ColorDescriptor());
        graph.AddRasterPass("dead", [&](RDGRasterPassBuilder& pass) { pass.UseMeshPass({7}).WriteColor(0, dead, LoadAction::Clear, StoreAction::Store); }, [](RDGRasterPassContext&) { return false; });
        graph.AddRasterPass("live", [&](RDGRasterPassBuilder& pass) { pass.UseMeshPass({3}).UseMeshPass({3}).WriteColor(0, live, LoadAction::Clear, StoreAction::Store); }, [this](RDGRasterPassContext& ctx) {
            Events.push_back("execute");
            const auto& commands = ctx.GetSlot().GetPassDrawList({3}).VisibleCommands;
            EXPECT_EQ(commands[0].SortKey, 1);
            EXPECT_EQ(commands[1].SortKey, 8);
            EXPECT_EQ(ctx.GetSlot().GetPassDrawList({7}).VisibleCommands[0].SortKey, 8);
            return true; });
        if (Reorder) {
            const auto other = graph.CreateTexture(ColorDescriptor());
            graph.AddRasterPass("other", [&](RDGRasterPassBuilder& pass) { pass.UseMeshPass({1}).WriteColor(0, other, LoadAction::Clear, StoreAction::Store).NeverCull(); }, [this](RDGRasterPassContext&) { Events.push_back("other"); return true; });
            graph.AddRasterPass("tail", [&](RDGRasterPassBuilder& pass) { pass.UseMeshPass({2}).WriteColor(0, live, LoadAction::Load, StoreAction::Store); }, [this](RDGRasterPassContext&) { Events.push_back("tail"); return true; });
        }
        graph.MarkOutput(live);
    }
    void ExtractMeshPass(MeshPassId pass, const Scene&, FrameSlot&, MeshPassFrameDrawList& list) noexcept override {
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
    scheduler->RunFrame(scene);
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
    scheduler->RunFrame(scene);
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
    owner->OnSetup = [&](RDGBuilder& graph, const Scene& source) {
        EXPECT_EQ(&source, &scene);
        ASSERT_TRUE(source.GetPrimitive(primitive));
        graph.AddCopyPass("borrow scene", [](RDGCopyPassBuilder& pass) { pass.NeverCull(); }, [&source, &recordedCounts](RDGCopyPassContext&) {
            recordedCounts.push_back(source.GetPrimitiveCount());
            return true; });
    };
    auto scheduler = FrameScheduler::Create(context, std::move(owner), 1);
    ASSERT_TRUE(scheduler);
    scheduler->RunFrame(scene);
    scene.AddPrimitive();
    scheduler->RunFrame(scene);
    EXPECT_EQ(recordedCounts, (vector<uint32_t>{1, 2}));
    scene.Clear();
    scheduler->WaitIdle();
}

}  // 匿名命名空间

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
            graph.AddCopyPass("read", [&](RDGCopyPassBuilder& pass) { pass.ReadBuffer(b).ReadTexture(t).NeverCull(); }, [&](RDGCopyPassContext&) {
                EXPECT_EQ(Trace.Barriers.size(), 2u);
                EXPECT_EQ(std::get<BarrierBufferDescriptor>(Trace.Barriers[0]).ExternalTransfer, ExternalOwnershipTransfer::Acquire);
                EXPECT_EQ(std::get<BarrierTextureDescriptor>(Trace.Barriers[1]).ExternalTransfer, ExternalOwnershipTransfer::Acquire);
                return true; });
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
    ObjectArena arena;
    RDGBuilder graph{arena};
    graph.ImportBuffer(&buffer, BufferState::Vertex, std::nullopt, {true, false});
    graph.ImportBuffer(&buffer, BufferState::Vertex);
    EXPECT_FALSE(graph.Compile());
}

TEST_F(RDGExecutionTest, SameStateGroupReadStillAcquiresExternalOwnership) {
    FakeBuffer buffer{64, BufferUse::CopySource};
    Setup = [&](RDGBuilder& graph) {
        const auto handle = graph.ImportBuffer(&buffer, BufferState::CopySource, std::nullopt, {true, false});
        graph.AddCopyPass("read", [&](RDGCopyPassBuilder& pass) { pass.ReadBuffer(handle).NeverCull(); }, [&](RDGCopyPassContext&) {
            EXPECT_EQ(Trace.Barriers.size(), 1u);
            return true; });
    };
    Run();
    ASSERT_EQ(Trace.Barriers.size(), 1u);
    const auto& barrier = std::get<BarrierBufferDescriptor>(Trace.Barriers[0]);
    EXPECT_EQ(barrier.Before, barrier.After);
    EXPECT_EQ(barrier.ExternalTransfer, ExternalOwnershipTransfer::Acquire);
}
