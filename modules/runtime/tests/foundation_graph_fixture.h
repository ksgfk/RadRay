#pragma once

#include "gpu_test_fixture.h"
#include "render_graph_test_driver.h"
#include "foundation_shader_fixture.h"
#include <radray/runtime/render_framework/render_graph_runtime.h>
#include <radray/runtime/render_framework/viewport.h>
#include <radray/utility.h>

namespace radray::test {

struct EmptyGraphPass {};

class FoundationGraphGpuTest : public testing::TestWithParam<render::RenderBackend> {
protected:
    void SetUp() override {
        if (!render::test::TryCreateDevice(GetParam(), Context, true)) GTEST_SKIP() << Context.Reason;
        Registry = make_unique<render::RenderPassRegistry>(Context.Device.get());
        Resources = make_unique<RenderGraphFrameResources>(*Context.Device, *Registry);
        Resources->BeginFlight(1, Writes);
    }
    void TearDown() override {
        if (Context.Queue) Context.Queue->Wait();
        Resources.reset();
        Registry.reset();
        Context.Reset();
        EXPECT_EQ(Context.ValidationErrors.load(), 0u);
    }
    RenderGraph MakeGraph(std::string_view name = "foundation") { return {*Context.Device, *Resources, *Registry, name}; }
    bool Run(RenderGraph& graph) {
        auto command = Context.Device->CreateCommandBuffer(Context.Queue);
        if (!command) return false;
        command->Begin();
        const auto result = RenderGraphTestDriver::Execute(graph, *command);
        Writes.Flush(*Context.Device);
        command->End();
        if (result.CommandsRecorded) {
            auto* raw = command.Get();
            Context.Queue->Submit({.CmdBuffers = std::span{&raw, 1}});
            Context.Queue->Wait();
        }
        return result.Success;
    }
    void HostRead(RenderGraph& graph, RgBufferHandle buffer) {
        graph.AddComputePass<EmptyGraphPass>("host visibility", [=](EmptyGraphPass&, RenderGraphComputeBuilder& builder) {
            builder.ReadBuffer(buffer, RgBufferAccess::HostRead);
            builder.SetSideEffect(); }, +[](const EmptyGraphPass&, RenderGraphComputeContext&) {});
    }
    vector<byte> Read(render::Buffer& buffer) {
        vector<byte> bytes(buffer.GetDesc().Size);
        auto* mapped = buffer.Map(0, bytes.size());
        if (!mapped) return {};
        buffer.InvalidateMappedRange({0, bytes.size()});
        std::memcpy(bytes.data(), mapped, bytes.size());
        buffer.Unmap();
        return bytes;
    }
    render::test::DeviceContext Context;
    HostWriteBatch Writes;
    unique_ptr<render::RenderPassRegistry> Registry;
    unique_ptr<RenderGraphFrameResources> Resources;
};

}  // namespace radray::test
