#include "runtime_test_support.h"
#include "gpu_runtime_test_support.h"

#include <gtest/gtest.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_system.h>

namespace radray::test {
namespace {

class RuntimeServicesApp final : public Application {
public:
    bool ReadBack{false};

protected:
    void OnInit() override {
        EXPECT_FALSE(GetSceneManager());
        EXPECT_FALSE(GetWorldManager());
        ASSERT_TRUE(GetGpuSystem());
        ASSERT_TRUE(GetRenderSystem());
        auto* device = GetGpuSystem()->GetDevice();
        _program = GetRenderSystem()->GetOrCreateShaderProgram({.SourceName = "modules/runtime/tests/data/runtime_services.hlsl"});
        ASSERT_TRUE(_program);
        auto stage = _program->GetStage(shader::ShaderStage::Compute);
        ASSERT_TRUE(stage);
        auto pipeline = device->CreateComputePipelineState({.PipelineLayout = _program->GetPipelineLayout(), .CS = *stage});
        ASSERT_TRUE(pipeline);
        _pipeline = pipeline.Release();
        auto output = device->CreateBuffer({.Size = sizeof(uint32_t), .Memory = render::MemoryType::Device, .Usage = render::BufferUse::UnorderedAccess | render::BufferUse::CopySource});
        ASSERT_TRUE(output);
        _output = output.Release();
        auto readback = device->CreateBuffer({.Size = sizeof(uint32_t), .Memory = render::MemoryType::ReadBack, .Usage = render::BufferUse::CopyDestination | render::BufferUse::MapRead});
        ASSERT_TRUE(readback);
        _readback = readback.Release();
    }

    void OnUpdate(const AppUpdateContext&) override {
        if (ReadBack || ++_updates == 16) RequestExit();
    }

    void OnRender(AppFrameContext& frame) override {
        if (frame.FrameSerial() != 1 || !_pipeline || !_output || !_readback) return;
        auto* layout = _program->GetPipelineLayout();
        auto parameters = frame.GetGpuFrameResources().AllocateParameters(layout, 0);
        ASSERT_TRUE(parameters);
        ASSERT_TRUE(parameters->Set(layout->FindBinding("Output"), 0, render::ShaderBufferBinding{_output.get(), {0, sizeof(uint32_t)}, sizeof(uint32_t)}));
        ASSERT_TRUE(parameters->FlushWrites());
        auto& allocator = frame.GetCmdAllocator();
        auto* command = allocator.Allocate();
        const render::ResourceBarrierDescriptor begin[]{
            render::BarrierBufferDescriptor{_output.get(), render::BufferState::Undefined, render::BufferState::UnorderedAccess},
            render::BarrierBufferDescriptor{_readback.get(), render::BufferState::Undefined, render::BufferState::CopyDestination}};
        command->ResourceBarrier(begin);
        auto encoder = command->BeginComputePass();
        ASSERT_TRUE(encoder);
        encoder->BindComputePipelineState(_pipeline.get());
        encoder->BindShaderParameterSet(0, parameters.Get());
        encoder->Dispatch(1, 1, 1);
        command->EndComputePass(encoder.Release());
        const render::ResourceBarrierDescriptor copy = render::BarrierBufferDescriptor{_output.get(), render::BufferState::UnorderedAccess, render::BufferState::CopySource};
        command->ResourceBarrier(std::span{&copy, 1});
        command->CopyBufferToBuffer(_readback.get(), 0, _output.get(), 0, sizeof(uint32_t));
        const render::ResourceBarrierDescriptor end = render::BarrierBufferDescriptor{_readback.get(), render::BufferState::CopyDestination, render::BufferState::HostRead};
        command->ResourceBarrier(std::span{&end, 1});
        allocator.Return(std::span{&command, 1});
        frame.RegisterClosedCommandBuffers({.CmdBuffers = std::span{&command, 1}});
    }

    void OnRenderFrameComplete(const FlightCompletion& completion) override {
        if (completion.FrameSerial != 1 || !_readback) return;
        ASSERT_TRUE(completion.GpuWorkCompleted);
        ScopedBufferMap map{_readback.get(), {0, sizeof(uint32_t)}};
        ASSERT_TRUE(map);
        _readback->InvalidateMappedRange({0, sizeof(uint32_t)});
        EXPECT_EQ(*map.DataAs<uint32_t>(), 0xc0de1234u);
        ReadBack = true;
    }

    void OnShutdown() override {
        _pipeline.reset();
        _readback.reset();
        _output.reset();
        _program = nullptr;
    }

private:
    Nullable<ShaderProgram*> _program{nullptr};
    unique_ptr<render::ComputePipelineState> _pipeline;
    unique_ptr<render::Buffer> _output, _readback;
    uint32_t _updates{0};
};

void CheckRuntimeServices(render::RenderBackend backend, bool threaded) {
    RuntimeLogCapture logs;
    RuntimeServicesApp app;
    const auto run = RunApplication(app, {
                                             .FlightDataCount = threaded ? 3u : 1u,
                                             .Window = std::nullopt,
                                             .Gpu = GpuOptions{.Backend = backend, .EnableValidation = true, .EnableSynchronizationValidation = true, .Multithreaded = threaded, .EnableFrameProfiler = false},
                                             .Render = RenderOptions{.ShaderSourceRoot = RADRAY_PROJECT_DIR, .ShaderIncludePaths = {RADRAY_SHADERLIB_DIR}},
                                             .Scene = std::nullopt,
                                             .World = std::nullopt,
                                             .Asset = std::nullopt,
                                         });
    if (CanSkipRuntimeStartup(backend, run.Startup)) GTEST_SKIP() << run.Startup.Reason;
    ASSERT_EQ(run.Startup.Status, RuntimeStartupStatus::Started) << run.Startup.Reason;
    EXPECT_EQ(run.ExitCode, 0);
    EXPECT_TRUE(app.ReadBack);
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
}

TEST(RuntimeServices, D3D12WithoutSceneOrWorld) { CheckRuntimeServices(render::RenderBackend::D3D12, false); }
TEST(RuntimeServices, VulkanWithoutSceneOrWorld) { CheckRuntimeServices(render::RenderBackend::Vulkan, false); }
TEST(RuntimeServices, D3D12ThreadedWithoutSceneOrWorld) { CheckRuntimeServices(render::RenderBackend::D3D12, true); }
TEST(RuntimeServices, VulkanThreadedWithoutSceneOrWorld) { CheckRuntimeServices(render::RenderBackend::Vulkan, true); }

}  // namespace
}  // namespace radray::test
