#include <radray/runtime/environment.h>
#include <radray/runtime/renderer.h>
#include <radray/runtime/pipeline_cache.h>
#include <radray/file.h>
#include "gpu_test_fixture.h"
#include <radray/logger.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <iostream>
#include <mutex>
#include <gtest/gtest.h>

namespace radray::test {
using namespace radray::render;
namespace {
void Check(bool condition, std::string_view message) { EXPECT_TRUE(condition) << message; }
vector<byte> Bytecode(const char* name, const char* entry, bool vk) {
    auto path = std::filesystem::path(RADRAY_RUNTIME_TEST_SHADER_DIR) / (string(name) + "." + entry + (vk ? ".spirv.bin" : ".dxil.bin"));
    auto bytes = ReadBinaryFile(path);
    EXPECT_TRUE(bytes.has_value()) << path;
    return bytes.value_or(vector<byte>{});
}
struct Processor final : MeshPassProcessor {
    bool Validate(const MeshBatch&, const MaterialDescriptor&) const override { return true; }
};
struct TestFrame {
    shared_ptr<Buffer> readback;
    uint32_t width{16}, height{16};
    bool initialized{}, extract{}, failBuild{}, failCompile{}, failRecord{}, failResolve{}, failRaster{}, failCopy{};
};
class TestPipeline final : public IRenderPipeline {
public:
    static unique_ptr<TestPipeline> Create(Device& d, bool compute, bool intermediate) {
        Resources resources;
        const bool vk = d.GetBackend() == RenderBackend::Vulkan;
        const auto blob = vk ? ShaderBlobCategory::SPIRV : ShaderBlobCategory::DXIL;
        if (compute) {
            resources.cs = d.CreateShader({vk ? Bytecode("graph_compute", "CSMain", true) : Bytecode("graph_compute", "CSMain", false), blob, ShaderStage::Compute}).Release();
        } else {
            resources.vs = d.CreateShader({vk ? Bytecode("graph_sample", "VSMain", true) : Bytecode("graph_sample", "VSMain", false), blob, ShaderStage::Vertex}).Release();
            resources.ps = d.CreateShader({vk ? Bytecode("graph_sample", "PSMain", true) : Bytecode("graph_sample", "PSMain", false), blob, ShaderStage::Pixel}).Release();
        }
        const BindingDescriptor binding{0, compute ? BindingKind::RWStructuredBuffer : BindingKind::Texture, 1, compute ? ShaderStage::Compute : ShaderStage::Pixel,
                                        D3D12BindingDescriptor{compute ? BindingRegisterClass::UAV : BindingRegisterClass::SRV, 0, 0}, VulkanBindingDescriptor{0, 0}};
        resources.group = d.CreateBindingGroupLayout({{&binding, 1}}).Release();
        Check(resources.group != nullptr, "Create test binding layout");
        const PipelineLayoutGroupDescriptor group{0, resources.group.get()};
        resources.layout = d.CreatePipelineLayout({{&group, 1}, {}}).Release();
        Check(resources.layout != nullptr, "Create test pipeline layout");
        if (compute) {
            Check(resources.cs != nullptr, "Create compute shader");
            resources.compute = d.CreateComputePipelineState({resources.layout.get(), {resources.cs.get(), "CSMain"}}).Release();
            Check(resources.compute != nullptr, "Create compute pipeline");
        } else {
            Check(resources.vs && resources.ps, "Create raster shaders");
            auto primitive = PrimitiveState::Default();
            primitive.Cull = CullMode::None;
            primitive.UnclippedDepth = false;
            const auto color = ColorTargetState::Default(TextureFormat::RGBA8_UNORM);
            GraphicsPipelineStateDescriptor desc{.PipelineLayout = resources.layout.get(), .VS = ShaderEntry{resources.vs.get(), "VSMain"}, .PS = ShaderEntry{resources.ps.get(), "PSMain"}, .Primitive = primitive, .MultiSample = MultiSampleState::Default(), .ColorTargets = {&color, 1}};
            resources.cache = make_unique<GraphicsPipelineCache>(d);
            resources.raster = resources.cache->Get(desc, GetAttachmentSignature(desc)).Get();
            EXPECT_TRUE(resources.raster);
            Check(resources.raster == resources.cache->Get(desc, GetAttachmentSignature(desc)).Get() && resources.cache->Size() == 1, "PSO cache did not reuse complete key");
            desc.Primitive.Cull = CullMode::Front;
            Check(resources.cache->Get(desc, GetAttachmentSignature(desc)).Get() != resources.raster && resources.cache->Size() == 2, "PSO fixed state omitted from cache key");
            auto signature = GetAttachmentSignature(desc);
            signature.Samples = 4;
            Check(!resources.cache->Get(desc, signature), "PSO attachment signature mismatch accepted");
        }
        return unique_ptr<TestPipeline>(new TestPipeline(std::move(resources), compute, intermediate));
    }
    const MeshPassProcessor& Processor() const override { return _processor; }
    bool BuildGraph(GraphBuilder& graph, FrameContext& frame, PreparedMeshPass&, Nullable<const void*> data) override {
        const auto f = *static_cast<const TestFrame*>(data.Get());
        if (f.failBuild) {
            RADRAY_ERR_LOG("injected build failure");
            return false;
        }
        const auto readback = graph.ImportBuffer("Readback", f.readback.get(), {ResourceState::Buffer(f.initialized ? BufferState::HostRead : BufferState::Undefined), ResourceState::Buffer(BufferState::HostRead), f.initialized, false, false, f.readback});
        if (_compute) {
            const auto work = graph.CreateBuffer("Compute working set", {64, MemoryType::Device, BufferUse::UnorderedAccess | BufferUse::CopyDestination | BufferUse::CopySource});
            array<uint32_t, 16> values;
            for (uint32_t i = 0; i < values.size(); ++i) values[i] = i + 1;
            auto upload = frame.Upload("Test data", std::as_bytes(std::span{values}), BufferUse::CopySource);
            if (!upload) return false;
            auto src = graph.ImportBuffer("Upload", upload, {ResourceState::Buffer(BufferState::CopySource), ResourceState::Buffer(BufferState::CopySource)});
            if (!f.failCompile) graph.AddCopyPass("Upload", {{src, Access::CopySource}, {work, Access::CopyDestination}}, [src, work](PassContext& c) {
                c.Copy()->CopyBufferToBuffer(c.Buffer(work).Get(), 0, c.Buffer(src).Get(), 0, 64);
                return true;
            });
            for (uint32_t i = 0; i < 2; ++i) graph.AddComputePass("Compute " + std::to_string(i), {{work, Access::StorageReadWrite}}, [this, work, f](PassContext& c) {
                if (f.failRecord) {
                    RADRAY_ERR_LOG("injected recording failure");
                    return false;
                }
                if (f.failResolve) {
                    Check(!c.Buffer({GraphBuilder().Id(), work.index}), "Foreign resource resolved");
                    return true;
                }
                if (f.failRaster) {
                    Check(!c.Raster(), "Compute pass exposed a raster encoder");
                    return true;
                }
                if (f.failCopy) {
                    Check(!c.Copy(), "Compute pass exposed copy commands");
                    return true;
                }
                auto parameters = c.Device().CreateShaderParameterSet({_resources.group.get()}).Release();
                if (!parameters || !parameters->Set(0, 0, ShaderBufferBinding{c.Buffer(work).Get(), {0, 64}, 4}) || !parameters->FlushWrites()) {
                    RADRAY_ERR_LOG("Test compute binding failed");
                    return false;
                }
                c.Compute()->BindComputePipelineState(_resources.compute.get());
                c.Compute()->BindShaderParameterSet(0, parameters.get());
                c.Compute()->Dispatch(2, 1, 1);
                c.KeepAlive(shared_ptr<ShaderParameterSet>(std::move(parameters)));
                return true;
            });
            graph.AddCopyPass("Readback", {{work, Access::CopySource}, {readback, Access::CopyDestination}}, [work, readback](PassContext& c) {
                c.Copy()->CopyBufferToBuffer(c.Buffer(readback).Get(), 0, c.Buffer(work).Get(), 0, 64);
                return true;
            });
            if (f.extract) graph.Extract(work);
        } else {
            TextureDescriptor desc{TextureDimension::Dim2D, f.width, f.height, 1, 1, 1, TextureFormat::RGBA8_UNORM, MemoryType::Device, TextureUse::RenderTarget | TextureUse::Resource | TextureUse::CopySource};
            const auto source = graph.CreateTexture("Color source", desc);
            ColorAttachment attachment{source};
            attachment.clear.Value = {.25f, .5f, .75f, 1.f};
            graph.AddRasterPass("Clear", {}, {{attachment}, {}, f.width, f.height}, {});
            auto result = source;
            if (_intermediate) {
                result = graph.CreateTexture("Programmable intermediate", desc);
                graph.AddRasterPass("Sample", {{source, Access::Sample, ShaderStage::Pixel}}, {{{result}}, {}, f.width, f.height}, [this, source](PassContext& c) {
                    auto view = c.CreateView(source, TextureViewUsage::Resource);
                    auto parameters = c.Device().CreateShaderParameterSet({_resources.group.get()}).Release();
                    if (!parameters || !parameters->Set(0, 0, view.Get()) || !parameters->FlushWrites()) {
                        RADRAY_ERR_LOG("Test sampled binding failed");
                        return false;
                    }
                    c.Raster()->BindGraphicsPipelineState(_resources.raster);
                    c.Raster()->BindShaderParameterSet(0, parameters.get());
                    c.Raster()->Draw(3, 1, 0, 0);
                    c.KeepAlive(shared_ptr<ShaderParameterSet>(std::move(parameters)));
                    return true;
                });
            }
            graph.AddCopyPass("Readback", {{result, Access::CopySource}, {readback, Access::CopyDestination}}, [result, readback](PassContext& c) {
                c.Copy()->CopyTextureToBuffer(c.Buffer(readback).Get(), 0, c.Texture(result).Get(), {0, 1, 0, 1});
                return true;
            });
        }
        return true;
    }

private:
    struct Resources {
        radray::unique_ptr<Shader> vs, ps, cs;
        radray::unique_ptr<BindingGroupLayout> group;
        radray::unique_ptr<PipelineLayout> layout;
        unique_ptr<GraphicsPipelineCache> cache;
        GraphicsPipelineState* raster{};
        radray::unique_ptr<ComputePipelineState> compute;
    };
    TestPipeline(Resources resources, bool compute, bool intermediate) : _resources(std::move(resources)), _compute(compute), _intermediate(intermediate) {}
    Resources _resources;
    struct Processor _processor;
    bool _compute, _intermediate;
};
shared_ptr<Buffer> Readback(Device& d, uint64_t size) {
    auto b = CreateBuffer(d, size, MemoryType::ReadBack, BufferUse::CopyDestination | BufferUse::MapRead).Release();
    Check(b != nullptr, "Create readback");
    return shared_ptr<Buffer>(std::move(b));
}
void ReadCompute(Buffer& buffer) {
    auto* data = static_cast<uint32_t*>(buffer.Map(0, 64));
    Check(data, "Map compute readback");
    buffer.InvalidateMappedRange({0, 64});
    bool equal = true;
    for (uint32_t i = 0; i < 16; ++i) equal = equal && data[i] == (i + 1) * 9 + 28;
    buffer.Unmap();
    Check(equal, "copy-compute-compute-copy readback mismatch");
}
void ImportChecks(Device& d) {
    auto buffer = CreateBuffer(d, 64, MemoryType::Device, BufferUse::CopyDestination | BufferUse::CopySource).Release();
    const auto initial = ResourceState::Buffer(BufferState::Undefined), final = ResourceState::Buffer(BufferState::CopySource);
    GraphBuilder g;
    auto a = g.ImportBuffer("external", buffer.get(), {initial, final, false});
    auto b = g.ImportBuffer("same", buffer.get(), {initial, final, false});
    Check(a == b, "Imported object not deduplicated");
    g.AddCopyPass("write", {{a, Access::CopyDestination}}, {});
    CompiledGraph c;
    EXPECT_TRUE(g.Compile(c));
    Check(c.plan.size() == 1 && c.epilogue.size() == 1 && c.finalStates[a.index].buffer == BufferState::CopySource, "External write root/final state missing");
    g.ImportBuffer("conflict", buffer.get(), {initial, ResourceState::Buffer(BufferState::Common), false});
    Check(!g.Compile(c), "Conflicting external import accepted");
    GraphBuilder boundary;
    auto s = ResourceState::Buffer(BufferState::Common);
    boundary.ImportBuffer("unused shared", buffer.get(), {s, s, true, true, true});
    EXPECT_TRUE(boundary.Compile(c));
    Check(c.plan.empty() && c.epilogue.size() == 2, "Empty graph lost acquire/release boundary");
    weak_ptr<int> firstOwner, secondOwner;
    {
        auto first = make_shared<int>(1), second = make_shared<int>(2);
        firstOwner = first;
        secondOwner = second;
        GraphBuilder owners;
        auto imported = owners.ImportBuffer("borrowed", buffer.get(), {s, s});
        owners.ImportBuffer("first owner", buffer.get(), {s, s, true, false, false, first});
        owners.ImportBuffer("second owner", buffer.get(), {s, s, true, false, false, second});
        owners.Extract(imported);
        EXPECT_TRUE(owners.Compile(c));
    }
    Check(!firstOwner.expired() && !secondOwner.expired(), "Duplicate import lost a resource owner");
    c = {};
    Check(firstOwner.expired() && secondOwner.expired(), "Released graph retained imported owners");
}
void PoolChecks(DeviceContext& device) {
    auto& d = device.GetDevice();
    ResourceStore pool(d);
    GraphResource resource{"pool buffer", ResourceKind::Buffer};
    resource.buffer = {64, MemoryType::Device, BufferUse::CopyDestination};
    shared_ptr<Fence> fence(d.CreateFence().Release());
    Check(fence != nullptr, "Create pool completion");
    auto first = pool.Acquire(resource).Release();
    auto* original = first->buffer.get();
    first->completion = fence;
    first->value = 1;
    first.reset();
    auto second = pool.Acquire(resource).Release();
    Check(second->buffer.get() != original, "Pool reused allocation before GPU completion");
    second.reset();
    Fence* signals[]{fence.get()};
    uint64_t values[]{1};
    device.GetQueue().Submit({.SignalFences = signals, .SignalValues = values});
    Check(fence->WaitFor(1, 5000), "Pool completion signal failed");
    auto recycled = pool.Acquire(resource).Release();
    Check(recycled->buffer.get() == original, "Completed pool allocation not reused");
    recycled.reset();
    resource.buffer.Size = 128;
    auto different = pool.Acquire(resource).Release();
    Check(different->buffer.get() != original, "Pool ignored complete buffer descriptor");
    GraphBuilder graph;
    graph.CreateBuffer("culled", {256, MemoryType::Device, BufferUse::CopyDestination});
    CompiledGraph compiled;
    EXPECT_TRUE(graph.Compile(compiled));
    const auto count = pool.PooledCount();
    unique_ptr<GraphExecution> execution;
    execution = GraphExecution::Create(d, pool, std::move(compiled)).Release();
    EXPECT_TRUE(execution != nullptr);
    Check(pool.PooledCount() == count, "Unused resource received GPU allocation");
}
void ComputeChecks(DeviceContext& d) {
    unique_ptr<Renderer> renderer;
    renderer = Renderer::Create(d, TestPipeline::Create(d.GetDevice(), true, false)).Release();
    EXPECT_TRUE(renderer != nullptr);
    Scene scene;
    DrawRange range{0, 3};
    MeshUpdate mesh{1, nullptr, 0, &range, 3, 0, 0, 1};
    MaterialDescriptor material{1, 77};
    PrimitiveUpdate primitive{1, 1, 1};
    VisibleItem visible{1, 0, 0, 0, 1};
    array<byte, 8> instance{};
    SceneUpdate changes;
    changes.epoch = 1;
    changes.meshes = &mesh;
    changes.mesh_count = 1;
    changes.materials = &material;
    changes.material_count = 1;
    changes.primitives = &primitive;
    changes.primitive_count = 1;
    ViewSubmission view{{&visible, 1}, instance, 8};
    TestFrame f;
    f.readback = Readback(d.GetDevice(), 64);
    f.extract = true;
    for (uint32_t failure = 0; failure < 6; ++failure) {
        f.failBuild = failure == 0;
        f.failCompile = failure == 1;
        f.failRecord = failure == 2;
        f.failResolve = failure == 3;
        f.failRaster = failure == 4;
        f.failCopy = failure == 5;
        Check(!renderer->Render(scene, changes, view, 0, &f, {}), "Injected failure submitted");
        Check(renderer->Submitted() == 0 && scene.CommandBuilds() == 0, "Failure published scene or completion");
    }
    f.failBuild = f.failCompile = f.failRecord = f.failResolve = f.failRaster = f.failCopy = false;
    EXPECT_TRUE(renderer->Render(scene, changes, view, 0, &f, {}));
    Check(scene.CommandBuilds() == 1, "Successful frame not published");
    auto retained = renderer->Extractions()[0];
    Check(retained.value == 1 && retained.completion.get() == &renderer->Completion(), "Extraction lacks completion ownership");
    Check(retained.completion->WaitFor(retained.value, 5000), "Compute completion");
    ReadCompute(*f.readback);
    Check(renderer->LastGraph().find("UAV") != string::npos, "GPU graph omitted UAV barrier");
    changes.mesh_count = changes.material_count = changes.primitive_count = 0;
    f.initialized = true;
    for (uint32_t i = 0; i < 4; ++i) {
        EXPECT_TRUE(renderer->Render(scene, changes, view, 0, &f, {}));
        Check(renderer->Extractions()[0].buffer != retained.buffer, "Extraction allocation was recycled while externally owned");
    }
    EXPECT_TRUE(renderer->Finish());
    ReadCompute(*f.readback);
    const auto allocations = renderer->Resources().PooledCount();
    retained = {};
    EXPECT_TRUE(renderer->Render(scene, changes, view, 0, &f, {}));
    EXPECT_TRUE(renderer->Finish());
    Check(renderer->Resources().PooledCount() == allocations, "Completed exact-description allocation not reused");
    Check(scene.CommandBuilds() == 1, "View-only frames rebuilt stable mesh commands");
    weak_ptr<Buffer> retired = f.readback;
    f.readback.reset();
    Check(retired.expired(), "Finished frame retained imported resource owner");
}
void RasterChecks(DeviceContext& d, bool intermediate) {
    unique_ptr<Renderer> renderer;
    renderer = Renderer::Create(d, TestPipeline::Create(d.GetDevice(), false, intermediate)).Release();
    EXPECT_TRUE(renderer != nullptr);
    Scene scene;
    SceneUpdate changes;
    changes.epoch = 1;
    ViewSubmission view{{}, {}, 1};
    for (uint32_t width : {16u, 24u, 16u}) {
        TestFrame f;
        f.width = width;
        const auto align = d.GetDevice().GetDetail().TextureDataPitchAlignment;
        const uint64_t pitch = (width * 4 + align - 1) / align * align, bytes = pitch * f.height;
        f.readback = Readback(d.GetDevice(), bytes);
        EXPECT_TRUE(renderer->Render(scene, changes, view, 0, &f, {}));
        EXPECT_TRUE(renderer->Finish());
        auto* data = static_cast<uint8_t*>(f.readback->Map(0, bytes));
        Check(data, "Map raster readback");
        f.readback->InvalidateMappedRange({0, bytes});
        bool correct = true;
        for (uint32_t y = 0; y < f.height; ++y)
            for (uint32_t x = 0; x < width; ++x) {
                const auto* pixel = data + y * pitch + x * 4;
                correct = correct && std::abs(int(pixel[0]) - 64) <= 1 && std::abs(int(pixel[1]) - 128) <= 1 && std::abs(int(pixel[2]) - 191) <= 1 && pixel[3] == 255;
            }
        f.readback->Unmap();
        Check(correct, "Raster-to-sampled-texture readback mismatch");
        Check((renderer->LastGraph().find("Programmable intermediate") != string::npos) == intermediate, "Pipeline substitution did not alter graph");
    }
}
}  // namespace
class RuntimeGpu : public testing::TestWithParam<RenderBackend> {
protected:
    std::atomic<uint32_t> Errors{};
    std::mutex LogMutex;
    vector<string> ExpectedErrors;
    unique_ptr<Environment> Env;
    unique_ptr<radray::DeviceContext> Runtime;
    static void Log(LogLevel level, std::string_view message, void* data) {
        if (level >= LogLevel::Err) {
            auto& fixture = *static_cast<RuntimeGpu*>(data);
            std::lock_guard<std::mutex> lock(fixture.LogMutex);
            auto expected = std::find(fixture.ExpectedErrors.begin(), fixture.ExpectedErrors.end(), message);
            if (level == LogLevel::Err && expected != fixture.ExpectedErrors.end()) {
                fixture.ExpectedErrors.erase(expected);
                return;
            }
            ++fixture.Errors;
            ADD_FAILURE() << message;
        }
    }
    void ExpectErrors(vector<string> messages) {
        std::lock_guard<std::mutex> lock(LogMutex);
        ExpectedErrors = std::move(messages);
    }
    void SetUp() override {
        {
            render::test::DeviceContext probe;
            if (!render::test::TryCreateDevice(GetParam(), probe, true, true)) {
                if (render::test::SetupMustFail(probe.Status, render::test::RequiredBackend(GetParam()))) FAIL() << probe.Reason;
                GTEST_SKIP() << probe.Reason;
            }
            probe.Reset();
        }
        SetLogCallback(Log, this);
        Env = Environment::Create(GetParam(), true).Release();
        ASSERT_NE(Env, nullptr);
        Runtime = radray::DeviceContext::Create(*Env).Release();
        ASSERT_NE(Runtime, nullptr);
    }
    void TearDown() override {
        Runtime.reset();
        Env.reset();
        ClearLogCallback();
        EXPECT_EQ(Errors.load(), 0u);
        std::lock_guard<std::mutex> lock(LogMutex);
        EXPECT_TRUE(ExpectedErrors.empty()) << "An expected error was not logged";
    }
};
TEST_P(RuntimeGpu, ImportsAndOwners) {
    ExpectErrors({"Conflicting duplicate buffer import"});
    ImportChecks(Runtime->GetDevice());
}
TEST_P(RuntimeGpu, PoolCompletion) { PoolChecks(*Runtime); }
TEST_P(RuntimeGpu, ComputeAndTransactions) {
    ExpectErrors({"injected build failure", "Uninitialized read: Compute 0 / Compute working set", "injected recording failure",
                  "Pass attempted to resolve an undeclared resource: Compute 0", "Not a raster pass: Compute 0", "Not a copy pass: Compute 0"});
    ComputeChecks(*Runtime);
}
TEST_P(RuntimeGpu, RasterAndResize) {
    ExpectErrors({"Pipeline attachment signature mismatch"});
    RasterChecks(*Runtime, false);
}
TEST_P(RuntimeGpu, ProgrammableIntermediate) {
    ExpectErrors({"Pipeline attachment signature mismatch"});
    RasterChecks(*Runtime, true);
}
INSTANTIATE_TEST_SUITE_P(Backends, RuntimeGpu, testing::Values(RenderBackend::D3D12, RenderBackend::Vulkan));

}  // namespace radray::test
