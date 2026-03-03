#include <gtest/gtest.h>

#include <algorithm>
#include <string_view>

#include <radray/render/pipeline_layout.h>
#include <radray/render/render_graph.h>

using namespace radray::render;

namespace {

RGTextureDescriptor MakeTextureDesc(std::string_view name, uint32_t mipLevels = 1) {
    RGTextureDescriptor desc{};
    desc.Width = 1;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = mipLevels;
    desc.SampleCount = 1;
    desc.Format = TextureFormat::RGBA8_UNORM;
    desc.Name = name;
    return desc;
}

PipelineLayout MakePipelineLayout(std::span<const ShaderParameter> parameters) {
    D3D12LayoutPlanner planner{};
    return PipelineLayout{parameters, planner};
}

bool HasDependencyEdge(const CompiledGraph& compiled, uint32_t from, uint32_t to) {
    return std::find(
               compiled.DependencyEdges.begin(),
               compiled.DependencyEdges.end(),
               std::pair<uint32_t, uint32_t>{from, to}) != compiled.DependencyEdges.end();
}

const RGSubresourceLifetime* FindTextureLifetime(
    const CompiledGraph& compiled,
    uint32_t resourceIndex,
    const RGTextureRange& range) {
    const auto it = std::find_if(
        compiled.SubresourceLifetimes.begin(),
        compiled.SubresourceLifetimes.end(),
        [resourceIndex, range](const RGSubresourceLifetime& lifetime) {
            return lifetime.Handle.Index == resourceIndex && std::holds_alternative<RGTextureRange>(lifetime.Range) && std::get<RGTextureRange>(lifetime.Range) == range;
        });
    if (it == compiled.SubresourceLifetimes.end()) {
        return nullptr;
    }
    return &(*it);
}

const RGSubresourceLifetime* FindBufferLifetime(
    const CompiledGraph& compiled,
    uint32_t resourceIndex,
    const RGBufferRange& range) {
    const auto it = std::find_if(
        compiled.SubresourceLifetimes.begin(),
        compiled.SubresourceLifetimes.end(),
        [resourceIndex, range](const RGSubresourceLifetime& lifetime) {
            return lifetime.Handle.Index == resourceIndex && std::holds_alternative<RGBufferRange>(lifetime.Range) && std::get<RGBufferRange>(lifetime.Range) == range;
        });
    if (it == compiled.SubresourceLifetimes.end()) {
        return nullptr;
    }
    return &(*it);
}

uint32_t FindSortedPassPosition(const CompiledGraph& compiled, uint32_t passIndex) {
    const auto it = std::find(compiled.SortedPasses.begin(), compiled.SortedPasses.end(), passIndex);
    if (it == compiled.SortedPasses.end()) {
        return RGSubresourceLifetime::InvalidPassIndex;
    }
    return static_cast<uint32_t>(std::distance(compiled.SortedPasses.begin(), it));
}

const RGBarrier* FindTextureBarrier(
    const radray::vector<RGBarrier>& barriers,
    uint32_t resourceIndex,
    const RGTextureRange& range,
    RGAccessMode stateBefore,
    RGAccessMode stateAfter) {
    const auto it = std::find_if(
        barriers.begin(),
        barriers.end(),
        [resourceIndex, range, stateBefore, stateAfter](const RGBarrier& barrier) {
            return barrier.Handle.Index == resourceIndex && barrier.StateBefore == stateBefore && barrier.StateAfter == stateAfter && std::holds_alternative<RGTextureRange>(barrier.Range) && std::get<RGTextureRange>(barrier.Range) == range;
        });
    if (it == barriers.end()) {
        return nullptr;
    }
    return &(*it);
}

const RGBarrier* FindBufferBarrier(
    const radray::vector<RGBarrier>& barriers,
    uint32_t resourceIndex,
    const RGBufferRange& range,
    RGAccessMode stateBefore,
    RGAccessMode stateAfter) {
    const auto it = std::find_if(
        barriers.begin(),
        barriers.end(),
        [resourceIndex, range, stateBefore, stateAfter](const RGBarrier& barrier) {
            return barrier.Handle.Index == resourceIndex && barrier.StateBefore == stateBefore && barrier.StateAfter == stateAfter && std::holds_alternative<RGBufferRange>(barrier.Range) && std::get<RGBufferRange>(barrier.Range) == range;
        });
    if (it == barriers.end()) {
        return nullptr;
    }
    return &(*it);
}

}  // namespace

TEST(RenderGraphPhase2, dce_culls_disconnected_passes) {
    RGGraphBuilder graph{};

    const auto src = graph.CreateTexture(MakeTextureDesc("src"));
    const auto out = graph.CreateTexture(MakeTextureDesc("out"));
    const auto unused = graph.CreateTexture(MakeTextureDesc("unused"));

    auto passProduce = graph.AddPass("produce_src");
    passProduce.WriteTexture(src);

    auto passCompose = graph.AddPass("compose_out");
    passCompose.ReadTexture(src);
    passCompose.WriteTexture(out);
    graph.MarkOutput(out);

    auto passUnused = graph.AddPass("unused_pass");
    passUnused.WriteTexture(unused);

    const auto compiled = graph.Compile();
    ASSERT_TRUE(compiled.Success);
    ASSERT_EQ(compiled.SortedPasses.size(), 2u);
    ASSERT_EQ(compiled.CulledPasses.size(), 1u);

    const auto& passes = graph.GetPasses();
    EXPECT_EQ(passes[compiled.SortedPasses[0]].Name, "produce_src");
    EXPECT_EQ(passes[compiled.SortedPasses[1]].Name, "compose_out");
    EXPECT_EQ(passes[compiled.CulledPasses[0]].Name, "unused_pass");
}

TEST(RenderGraphPhase2, persistent_resource_survives_dce) {
    RGGraphBuilder graph{};

    RGBufferDescriptor historyDesc{};
    historyDesc.Size = 1024;
    historyDesc.Flags = RGResourceFlag::Persistent;
    historyDesc.Name = "history";
    const auto history = graph.CreateBuffer(historyDesc);

    auto passHistory = graph.AddPass("history_update");
    passHistory.WriteBuffer(history);

    const auto compiled = graph.Compile();
    ASSERT_TRUE(compiled.Success);
    EXPECT_EQ(compiled.SortedPasses.size(), 1u);
    EXPECT_TRUE(compiled.CulledPasses.empty());
}

TEST(RenderGraphPhase4, pass_local_persistent_resource_survives_dce) {
    RGGraphBuilder graph{};

    RGTextureDescriptor historyDesc = MakeTextureDesc("history_local");
    historyDesc.Flags = RGResourceFlag::Persistent;

    auto pass = graph.AddPass("history_update");
    const auto history = pass.CreateLocalTexture(historyDesc);
    pass.WriteTexture(history);

    const auto& resource = graph.GetResources()[history.Index];
    EXPECT_TRUE(resource.Flags.HasFlag(RGResourceFlag::PassLocal));
    EXPECT_TRUE(resource.Flags.HasFlag(RGResourceFlag::Persistent));
    EXPECT_EQ(resource.ScopePassId, 0u);

    const auto compiled = graph.Compile();
    ASSERT_TRUE(compiled.Success);
    EXPECT_EQ(compiled.SortedPasses.size(), 1u);
    EXPECT_TRUE(compiled.CulledPasses.empty());
}

TEST(RenderGraphPhase4, pass_local_resource_cannot_be_accessed_by_other_pass) {
    RGGraphBuilder graph{};

    auto ownerPass = graph.AddPass("owner");
    const auto localTex = ownerPass.CreateLocalTexture(MakeTextureDesc("local_tex"));
    ownerPass.WriteTexture(localTex);

    const auto out = graph.CreateTexture(MakeTextureDesc("out"));
    auto otherPass = graph.AddPass("other");
    otherPass.ReadTexture(localTex);
    otherPass.WriteTexture(out);
    graph.MarkOutput(out);

    const auto compiled = graph.Compile();
    ASSERT_TRUE(compiled.Success);
    ASSERT_EQ(compiled.SortedPasses.size(), 1u);
    EXPECT_EQ(compiled.SortedPasses[0], 1u);
    EXPECT_FALSE(HasDependencyEdge(compiled, 0u, 1u));
}

TEST(RenderGraphPhase4, reflection_parameter_binding_auto_infers_dependencies) {
    RGGraphBuilder graph{};

    const auto src = graph.CreateTexture(MakeTextureDesc("src"));
    const auto out = graph.CreateTexture(MakeTextureDesc("out"));

    auto passProduce = graph.AddPass("produce_src");
    passProduce.WriteTexture(src);

    const radray::vector<ShaderParameter> params{
        ShaderParameter{
            .Name = "u_InputTex",
            .Type = ResourceBindType::Texture,
            .Register = 0,
            .Space = 0,
            .ArrayLength = 1,
            .TypeSizeInBytes = 0,
            .Stages = ShaderStage::Compute},
        ShaderParameter{
            .Name = "u_OutputTex",
            .Type = ResourceBindType::RWTexture,
            .Register = 1,
            .Space = 0,
            .ArrayLength = 1,
            .TypeSizeInBytes = 0,
            .Stages = ShaderStage::Compute}};
    auto layout = MakePipelineLayout(params);

    auto passShade = graph.AddPass("shade");
    passShade.SetPipelineLayout(&layout);
    passShade.SetResource("u_InputTex", src);
    passShade.SetResource("u_OutputTex", out);
    graph.MarkOutput(out);

    const auto compiled = graph.Compile();
    ASSERT_TRUE(compiled.Success);
    EXPECT_EQ(compiled.SortedPasses.size(), 2u);
    EXPECT_TRUE(HasDependencyEdge(compiled, 0u, 1u));
}

TEST(RenderGraphPhase4, reflection_parameter_binding_supports_mip_slice_overload) {
    RGGraphBuilder graph{};

    const auto tex = graph.CreateTexture(MakeTextureDesc("tex", 2));
    const auto out0 = graph.CreateTexture(MakeTextureDesc("out0"));
    const auto out1 = graph.CreateTexture(MakeTextureDesc("out1"));

    const RGTextureRange mip0{
        .BaseMipLevel = 0,
        .MipLevelCount = 1,
        .BaseArrayLayer = 0,
        .ArrayLayerCount = 1};

    auto passWriteMip0 = graph.AddPass("write_mip0");
    passWriteMip0.WriteTexture(tex, mip0);

    const radray::vector<ShaderParameter> params{
        ShaderParameter{
            .Name = "u_InputTex",
            .Type = ResourceBindType::Texture,
            .Register = 0,
            .Space = 0,
            .ArrayLength = 1,
            .TypeSizeInBytes = 0,
            .Stages = ShaderStage::Compute}};
    auto layout = MakePipelineLayout(params);

    auto passReadMip0 = graph.AddPass("read_mip0");
    passReadMip0.SetPipelineLayout(&layout);
    passReadMip0.SetResource("u_InputTex", tex, 0, 0);
    passReadMip0.WriteTexture(out0);
    graph.MarkOutput(out0);

    auto passReadMip1 = graph.AddPass("read_mip1");
    passReadMip1.SetPipelineLayout(&layout);
    passReadMip1.SetResource("u_InputTex", tex, 1, 0);
    passReadMip1.WriteTexture(out1);
    graph.MarkOutput(out1);

    const auto compiled = graph.Compile();
    ASSERT_TRUE(compiled.Success);
    EXPECT_TRUE(HasDependencyEdge(compiled, 0u, 1u));
    EXPECT_FALSE(HasDependencyEdge(compiled, 0u, 2u));
}

TEST(RenderGraphPhase4, reflection_and_explicit_edges_can_mix) {
    RGGraphBuilder graph{};

    const auto src = graph.CreateTexture(MakeTextureDesc("src"));
    const auto out = graph.CreateTexture(MakeTextureDesc("out"));

    auto passProduce = graph.AddPass("produce_src");
    passProduce.WriteTexture(src);

    const radray::vector<ShaderParameter> params{
        ShaderParameter{
            .Name = "u_AutoRead",
            .Type = ResourceBindType::Texture,
            .Register = 0,
            .Space = 0,
            .ArrayLength = 1,
            .TypeSizeInBytes = 0,
            .Stages = ShaderStage::Compute}};
    auto layout = MakePipelineLayout(params);

    auto passMix = graph.AddPass("mix");
    passMix.SetPipelineLayout(&layout);
    passMix.SetResource("u_AutoRead", src);
    passMix.WriteTexture(out);
    graph.MarkOutput(out);

    const auto compiled = graph.Compile();
    ASSERT_TRUE(compiled.Success);
    EXPECT_EQ(compiled.SortedPasses.size(), 2u);
    EXPECT_TRUE(HasDependencyEdge(compiled, 0u, 1u));
}

TEST(RenderGraphPhase2, topo_sort_orders_hazards) {
    RGGraphBuilder graph{};

    const auto a = graph.CreateTexture(MakeTextureDesc("a"));
    const auto b = graph.CreateTexture(MakeTextureDesc("b"));

    auto passA = graph.AddPass("pass_a");
    passA.ReadTexture(b);
    passA.WriteTexture(a);

    auto passB = graph.AddPass("pass_b");
    passB.ReadTexture(a);
    passB.WriteTexture(b);

    graph.MarkOutput(a);
    graph.MarkOutput(b);

    const auto compiled = graph.Compile();
    EXPECT_TRUE(compiled.Success);
    EXPECT_TRUE(HasDependencyEdge(compiled, 0, 1));
}

TEST(RenderGraphPhase2, lifetime_interval_matches_usage) {
    RGGraphBuilder graph{};

    const auto intermediate = graph.CreateTexture(MakeTextureDesc("intermediate"));
    const auto finalOut = graph.CreateTexture(MakeTextureDesc("final_out"));

    auto passA = graph.AddPass("produce_intermediate");
    passA.WriteTexture(intermediate);

    auto passB = graph.AddPass("consume_intermediate");
    passB.ReadTexture(intermediate);
    passB.WriteTexture(finalOut);
    graph.MarkOutput(finalOut);

    const auto compiled = graph.Compile();
    ASSERT_TRUE(compiled.Success);
    ASSERT_EQ(compiled.SortedPasses.size(), 2u);

    const RGTextureRange fullRange{
        .BaseMipLevel = 0,
        .MipLevelCount = 1,
        .BaseArrayLayer = 0,
        .ArrayLayerCount = 1};
    const auto* ltIntermediate = FindTextureLifetime(compiled, intermediate.Index, fullRange);
    const auto* ltFinalOut = FindTextureLifetime(compiled, finalOut.Index, fullRange);

    ASSERT_NE(ltIntermediate, nullptr);
    ASSERT_NE(ltFinalOut, nullptr);
    EXPECT_EQ(ltIntermediate->FirstPass, 0u);
    EXPECT_EQ(ltIntermediate->LastPass, 1u);
    EXPECT_EQ(ltFinalOut->FirstPass, 1u);
    EXPECT_EQ(ltFinalOut->LastPass, 1u);
}

TEST(RenderGraphPhase2, subresource_dependency_only_on_overlap) {
    RGGraphBuilder graph{};

    const auto tex = graph.CreateTexture(MakeTextureDesc("tex", 2));
    const auto out0 = graph.CreateTexture(MakeTextureDesc("out0"));
    const auto out1 = graph.CreateTexture(MakeTextureDesc("out1"));

    const RGTextureRange mip0{
        .BaseMipLevel = 0,
        .MipLevelCount = 1,
        .BaseArrayLayer = 0,
        .ArrayLayerCount = 1};
    const RGTextureRange mip1{
        .BaseMipLevel = 1,
        .MipLevelCount = 1,
        .BaseArrayLayer = 0,
        .ArrayLayerCount = 1};

    auto passWriteMip0 = graph.AddPass("write_mip0");
    passWriteMip0.WriteTexture(tex, mip0);

    auto passReadMip0 = graph.AddPass("read_mip0");
    passReadMip0.ReadTexture(tex, mip0);
    passReadMip0.WriteTexture(out0);
    graph.MarkOutput(out0);

    auto passReadMip1 = graph.AddPass("read_mip1");
    passReadMip1.ReadTexture(tex, mip1);
    passReadMip1.WriteTexture(out1);
    graph.MarkOutput(out1);

    const auto compiled = graph.Compile();
    ASSERT_TRUE(compiled.Success);

    EXPECT_TRUE(HasDependencyEdge(compiled, 0, 1));
    EXPECT_FALSE(HasDependencyEdge(compiled, 0, 2));
}

TEST(RenderGraphPhase2, disjoint_subresource_writes_do_not_depend) {
    RGGraphBuilder graph{};

    const auto tex = graph.CreateTexture(MakeTextureDesc("tex", 2));
    const RGTextureRange mip0{
        .BaseMipLevel = 0,
        .MipLevelCount = 1,
        .BaseArrayLayer = 0,
        .ArrayLayerCount = 1};
    const RGTextureRange mip1{
        .BaseMipLevel = 1,
        .MipLevelCount = 1,
        .BaseArrayLayer = 0,
        .ArrayLayerCount = 1};

    auto passWriteMip0 = graph.AddPass("write_mip0");
    passWriteMip0.WriteTexture(tex, mip0);
    graph.MarkOutput(tex, mip0);

    auto passWriteMip1 = graph.AddPass("write_mip1");
    passWriteMip1.WriteTexture(tex, mip1);
    graph.MarkOutput(tex, mip1);

    const auto compiled = graph.Compile();
    ASSERT_TRUE(compiled.Success);
    EXPECT_FALSE(HasDependencyEdge(compiled, 0, 1));
    EXPECT_FALSE(HasDependencyEdge(compiled, 1, 0));
}

TEST(RenderGraphPhase2, overlapping_buffer_ranges_are_split_into_atomic_lifetimes) {
    RGGraphBuilder graph{};

    RGBufferDescriptor bufferDesc{};
    bufferDesc.Size = 150;
    bufferDesc.Name = "history_buffer";
    const auto buffer = graph.CreateBuffer(bufferDesc);

    const auto out = graph.CreateTexture(MakeTextureDesc("out"));

    const RGBufferRange rangeA{
        .Offset = 0,
        .Size = 100};
    const RGBufferRange rangeB{
        .Offset = 50,
        .Size = 100};
    const RGBufferRange rangeC{
        .Offset = 0,
        .Size = 150};

    auto passA = graph.AddPass("write_a");
    passA.WriteBuffer(buffer, rangeA);

    auto passB = graph.AddPass("write_b");
    passB.WriteBuffer(buffer, rangeB);

    auto passC = graph.AddPass("read_all");
    passC.ReadBuffer(buffer, rangeC);
    passC.WriteTexture(out);
    graph.MarkOutput(out);

    const auto compiled = graph.Compile();
    ASSERT_TRUE(compiled.Success);
    EXPECT_TRUE(HasDependencyEdge(compiled, 0, 2));
    EXPECT_TRUE(HasDependencyEdge(compiled, 1, 2));

    const RGBufferRange segment0{
        .Offset = 0,
        .Size = 50};
    const RGBufferRange segment1{
        .Offset = 50,
        .Size = 50};
    const RGBufferRange segment2{
        .Offset = 100,
        .Size = 50};

    const auto* lt0 = FindBufferLifetime(compiled, buffer.Index, segment0);
    const auto* lt1 = FindBufferLifetime(compiled, buffer.Index, segment1);
    const auto* lt2 = FindBufferLifetime(compiled, buffer.Index, segment2);

    ASSERT_NE(lt0, nullptr);
    ASSERT_NE(lt1, nullptr);
    ASSERT_NE(lt2, nullptr);

    EXPECT_EQ(lt0->FirstPass, 0u);
    EXPECT_EQ(lt0->LastPass, 2u);
    EXPECT_EQ(lt1->FirstPass, 0u);
    EXPECT_EQ(lt1->LastPass, 2u);
    EXPECT_EQ(lt2->FirstPass, 1u);
    EXPECT_EQ(lt2->LastPass, 2u);
}

TEST(RenderGraphPhase2, output_resource_extends_to_last_pass) {
    RGGraphBuilder graph{};

    const auto outTex = graph.CreateTexture(MakeTextureDesc("out_tex"));
    const auto intermediate = graph.CreateTexture(MakeTextureDesc("intermediate"));

    auto passA = graph.AddPass("write_out");
    passA.WriteTexture(outTex);

    auto passB = graph.AddPass("read_out_write_inter");
    passB.ReadTexture(outTex);
    passB.WriteTexture(intermediate);

    const auto dummy = graph.CreateTexture(MakeTextureDesc("dummy"));
    auto passC = graph.AddPass("last_pass");
    passC.ReadTexture(intermediate);
    passC.WriteTexture(dummy);

    graph.MarkOutput(dummy);
    graph.MarkOutput(outTex);

    const auto compiled = graph.Compile();
    ASSERT_TRUE(compiled.Success);
    ASSERT_EQ(compiled.SortedPasses.size(), 3u);

    const RGTextureRange fullRange{
        .BaseMipLevel = 0,
        .MipLevelCount = 1,
        .BaseArrayLayer = 0,
        .ArrayLayerCount = 1};

    const auto* ltOut = FindTextureLifetime(compiled, outTex.Index, fullRange);
    ASSERT_NE(ltOut, nullptr);
    EXPECT_EQ(ltOut->FirstPass, 0u);
    // 因为 outTex 是 Output，即使最后一次被图内的 Pass 读是在 passB (1u)，它的生命周期也必须延伸到整个图序列结束 (2u)
    EXPECT_EQ(ltOut->LastPass, 2u);
}

TEST(RenderGraphPhase2, waw_and_war_hazards_are_recorded) {
    RGGraphBuilder graph{};

    const auto tex = graph.CreateTexture(MakeTextureDesc("tex"));
    const auto out1 = graph.CreateTexture(MakeTextureDesc("out1"));
    const auto out2 = graph.CreateTexture(MakeTextureDesc("out2"));

    auto passA = graph.AddPass("writer");
    passA.WriteTexture(tex);

    auto passB = graph.AddPass("reader");
    passB.ReadTexture(tex);
    passB.WriteTexture(out1);
    graph.MarkOutput(out1);

    auto passC = graph.AddPass("writer_again");
    passC.WriteTexture(tex);
    passC.WriteTexture(out2);
    graph.MarkOutput(out2);

    const auto compiled = graph.Compile();
    ASSERT_TRUE(compiled.Success);
    ASSERT_EQ(compiled.SortedPasses.size(), 3u);

    // RAW
    EXPECT_TRUE(HasDependencyEdge(compiled, 0, 1));
    // WAW
    EXPECT_TRUE(HasDependencyEdge(compiled, 0, 2));
    // WAR
    EXPECT_TRUE(HasDependencyEdge(compiled, 1, 2));
}

TEST(RenderGraphPhase2, read_write_access_creates_both_dependencies) {
    RGGraphBuilder graph{};

    const auto tex = graph.CreateTexture(MakeTextureDesc("tex"));
    const auto dummyOut = graph.CreateTexture(MakeTextureDesc("dummy"));

    auto passA = graph.AddPass("init");
    passA.WriteTexture(tex);

    auto passB = graph.AddPass("inplace_modify");
    passB.ReadWriteTexture(tex);

    auto passC = graph.AddPass("consume");
    passC.ReadTexture(tex);
    passC.WriteTexture(dummyOut);

    graph.MarkOutput(dummyOut);

    const auto compiled = graph.Compile();
    ASSERT_TRUE(compiled.Success);

    EXPECT_TRUE(HasDependencyEdge(compiled, 0, 1));
    EXPECT_TRUE(HasDependencyEdge(compiled, 1, 2));
}

TEST(RenderGraphPhase3, import_initial_mode_generates_first_transition_barrier) {
    RGGraphBuilder graph{};

    const auto imported = graph.ImportExternalTexture(
        "history_tex",
        RGResourceFlag::External | RGResourceFlag::Persistent,
        RGAccessMode::StorageRead);
    const auto out = graph.CreateTexture(MakeTextureDesc("out"));

    auto pass = graph.AddPass("write_imported");
    pass.WriteTexture(imported, {}, RGAccessMode::ColorAttachmentWrite);
    pass.WriteTexture(out);
    graph.MarkOutput(out);

    const auto compiled = graph.Compile();
    ASSERT_TRUE(compiled.Success);
    ASSERT_EQ(compiled.SortedPasses.size(), 1u);
    ASSERT_EQ(compiled.PassBarriers.size(), 1u);

    const RGTextureRange fullRange{
        .BaseMipLevel = 0,
        .MipLevelCount = 1,
        .BaseArrayLayer = 0,
        .ArrayLayerCount = 1};

    const auto* barrier = FindTextureBarrier(
        compiled.PassBarriers[0],
        imported.Index,
        fullRange,
        RGAccessMode::StorageRead,
        RGAccessMode::ColorAttachmentWrite);
    ASSERT_NE(barrier, nullptr);
}

TEST(RenderGraphPhase3, same_mode_writes_emit_flush_barrier) {
    RGGraphBuilder graph{};

    RGBufferDescriptor bufferDesc{};
    bufferDesc.Size = 128;
    bufferDesc.Name = "rw_buffer";
    const auto buffer = graph.CreateBuffer(bufferDesc);

    auto passA = graph.AddPass("write_a");
    passA.WriteBuffer(buffer, {}, RGAccessMode::StorageWrite);

    auto passB = graph.AddPass("write_b");
    passB.WriteBuffer(buffer, {}, RGAccessMode::StorageWrite);
    graph.MarkOutput(buffer);

    const auto compiled = graph.Compile();
    ASSERT_TRUE(compiled.Success);
    ASSERT_EQ(compiled.SortedPasses.size(), 2u);
    ASSERT_EQ(compiled.PassBarriers.size(), 2u);

    const uint32_t passBIndex = FindSortedPassPosition(compiled, 1u);
    ASSERT_NE(passBIndex, RGSubresourceLifetime::InvalidPassIndex);

    const RGBufferRange fullRange{
        .Offset = 0,
        .Size = 128};
    const auto* barrier = FindBufferBarrier(
        compiled.PassBarriers[passBIndex],
        buffer.Index,
        fullRange,
        RGAccessMode::StorageWrite,
        RGAccessMode::StorageWrite);
    ASSERT_NE(barrier, nullptr);
}

TEST(RenderGraphPhase3, fragmented_texture_barriers_merge_into_single_range) {
    RGGraphBuilder graph{};

    const auto tex = graph.CreateTexture(MakeTextureDesc("source_tex", 2));
    const auto out0 = graph.CreateTexture(MakeTextureDesc("out0"));
    const auto out1 = graph.CreateTexture(MakeTextureDesc("out1"));
    const auto finalOut = graph.CreateTexture(MakeTextureDesc("final"));

    const RGTextureRange mip0{
        .BaseMipLevel = 0,
        .MipLevelCount = 1,
        .BaseArrayLayer = 0,
        .ArrayLayerCount = 1};
    const RGTextureRange mip1{
        .BaseMipLevel = 1,
        .MipLevelCount = 1,
        .BaseArrayLayer = 0,
        .ArrayLayerCount = 1};
    const RGTextureRange fullRange{
        .BaseMipLevel = 0,
        .MipLevelCount = 2,
        .BaseArrayLayer = 0,
        .ArrayLayerCount = 1};

    auto passA = graph.AddPass("read_mip0");
    passA.ReadTexture(tex, mip0, RGAccessMode::SampledRead);
    passA.WriteTexture(out0);
    graph.MarkOutput(out0);

    auto passB = graph.AddPass("read_mip1");
    passB.ReadTexture(tex, mip1, RGAccessMode::SampledRead);
    passB.WriteTexture(out1);
    graph.MarkOutput(out1);

    auto passC = graph.AddPass("rewrite_full");
    passC.WriteTexture(tex, fullRange, RGAccessMode::StorageWrite);
    passC.WriteTexture(finalOut);
    graph.MarkOutput(finalOut);

    const auto compiled = graph.Compile();
    ASSERT_TRUE(compiled.Success);
    ASSERT_EQ(compiled.SortedPasses.size(), 3u);

    const uint32_t passCIndex = FindSortedPassPosition(compiled, 2u);
    ASSERT_NE(passCIndex, RGSubresourceLifetime::InvalidPassIndex);

    const auto& barriers = compiled.PassBarriers[passCIndex];
    const auto mergedCount = std::count_if(
        barriers.begin(),
        barriers.end(),
        [tex](const RGBarrier& barrier) {
            return barrier.Handle.Index == tex.Index && barrier.StateBefore == RGAccessMode::SampledRead && barrier.StateAfter == RGAccessMode::StorageWrite;
        });
    EXPECT_EQ(mergedCount, 1u);

    const auto* mergedBarrier = FindTextureBarrier(
        barriers,
        tex.Index,
        fullRange,
        RGAccessMode::SampledRead,
        RGAccessMode::StorageWrite);
    ASSERT_NE(mergedBarrier, nullptr);
}

TEST(RenderGraphPhase3, unbounded_buffer_ranges_split_into_non_overlapping_atomic_segments) {
    RGGraphBuilder graph{};

    const auto buffer = graph.ImportExternalBuffer(
        "external_blackbox",
        RGResourceFlag::External | RGResourceFlag::Persistent,
        RGAccessMode::Unknown);

    const RGBufferRange rangeA{
        .Offset = 0,
        .Size = 100};
    const RGBufferRange rangeB{
        .Offset = 50,
        .Size = RGBufferRange::All};

    auto passA = graph.AddPass("write_finite");
    passA.WriteBuffer(buffer, rangeA, RGAccessMode::StorageWrite);

    auto passB = graph.AddPass("write_unbounded");
    passB.WriteBuffer(buffer, rangeB, RGAccessMode::StorageWrite);
    graph.MarkOutput(buffer);

    const auto compiled = graph.Compile();
    ASSERT_TRUE(compiled.Success);
    ASSERT_EQ(compiled.SortedPasses.size(), 2u);

    const uint32_t passBIndex = FindSortedPassPosition(compiled, 1u);
    ASSERT_NE(passBIndex, RGSubresourceLifetime::InvalidPassIndex);

    const RGBufferRange seg0{
        .Offset = 0,
        .Size = 50};
    const RGBufferRange seg1{
        .Offset = 50,
        .Size = 50};
    const RGBufferRange seg2{
        .Offset = 100,
        .Size = RGBufferRange::All};

    const auto* lt0 = FindBufferLifetime(compiled, buffer.Index, seg0);
    const auto* lt1 = FindBufferLifetime(compiled, buffer.Index, seg1);
    const auto* lt2 = FindBufferLifetime(compiled, buffer.Index, seg2);
    ASSERT_NE(lt0, nullptr);
    ASSERT_NE(lt1, nullptr);
    ASSERT_NE(lt2, nullptr);

    const auto& barriers = compiled.PassBarriers[passBIndex];
    const auto* flushBarrier = FindBufferBarrier(
        barriers,
        buffer.Index,
        seg1,
        RGAccessMode::StorageWrite,
        RGAccessMode::StorageWrite);
    const auto* transitionBarrier = FindBufferBarrier(
        barriers,
        buffer.Index,
        seg2,
        RGAccessMode::Unknown,
        RGAccessMode::StorageWrite);

    ASSERT_NE(flushBarrier, nullptr);
    ASSERT_NE(transitionBarrier, nullptr);
}
