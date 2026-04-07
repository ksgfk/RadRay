#include <algorithm>
#include <functional>
#include <limits>
#include <string_view>

#include <gtest/gtest.h>

#include <radray/render/common.h>
#include <radray/runtime/render_graph.h>

using namespace radray;
using namespace radray::render;

namespace {

void ExpectContains(std::string_view text, std::string_view needle) {
    EXPECT_NE(text.find(needle), std::string_view::npos) << "missing substring: " << needle << "\nactual: " << text;
}

void ExpectValidateOk(const RenderGraph& graph) {
    const auto result = graph.Validate();
    EXPECT_TRUE(result.IsValid) << result.Message;
}

void ExpectValidateFailContains(const RenderGraph& graph, std::string_view needle) {
    const auto result = graph.Validate();
    EXPECT_FALSE(result.IsValid) << "expected failure containing: " << needle;
    ExpectContains(result.Message, needle);
}

void ExpectValidateFailContainsOneOf(const RenderGraph& graph, std::initializer_list<std::string_view> needles) {
    const auto result = graph.Validate();
    EXPECT_FALSE(result.IsValid);
    bool matched = false;
    for (std::string_view needle : needles) {
        if (result.Message.find(needle) != std::string_view::npos) {
            matched = true;
            break;
        }
    }
    EXPECT_TRUE(matched) << "unexpected message: " << result.Message;
}

GpuBufferHandle MakeValidGpuBufferHandle(uint64_t handle = 1) {
    GpuBufferHandle result{};
    result.Handle = handle;
    return result;
}

GpuTextureHandle MakeValidGpuTextureHandle(uint64_t handle = 1) {
    GpuTextureHandle result{};
    result.Handle = handle;
    return result;
}

class LambdaRasterPass final : public IRDGRasterPass {
public:
    explicit LambdaRasterPass(std::function<void(Builder&)> setup) noexcept
        : _setup(std::move(setup)) {}

    void Setup(Builder& builder) override {
        _setup(builder);
    }

    void Execute(GraphicsCommandEncoder* encoder, GpuAsyncContext* context) override {
        (void)encoder;
        (void)context;
    }

private:
    std::function<void(Builder&)> _setup;
};

class LambdaComputePass final : public IRDGComputePass {
public:
    explicit LambdaComputePass(std::function<void(Builder&)> setup) noexcept
        : _setup(std::move(setup)) {}

    void Setup(Builder& builder) override {
        _setup(builder);
    }

    void Execute(ComputeCommandEncoder* encoder, GpuAsyncContext* context) override {
        (void)encoder;
        (void)context;
    }

private:
    std::function<void(Builder&)> _setup;
};

unique_ptr<IRDGRasterPass> MakeRasterPass(std::function<void(IRDGRasterPass::Builder&)> setup) {
    return make_unique<LambdaRasterPass>(std::move(setup));
}

unique_ptr<IRDGComputePass> MakeComputePass(std::function<void(IRDGComputePass::Builder&)> setup) {
    return make_unique<LambdaComputePass>(std::move(setup));
}

RDGBufferNode* GetBufferNode(RenderGraph& graph, RDGBufferHandle handle) {
    return static_cast<RDGBufferNode*>(graph.Resolve(handle));
}

RDGTextureNode* GetTextureNode(RenderGraph& graph, RDGTextureHandle handle) {
    return static_cast<RDGTextureNode*>(graph.Resolve(handle));
}

RDGGraphicsPassNode* GetGraphicsPassNode(RenderGraph& graph, RDGPassHandle handle) {
    return static_cast<RDGGraphicsPassNode*>(graph.Resolve(handle));
}

RDGComputePassNode* GetComputePassNode(RenderGraph& graph, RDGPassHandle handle) {
    return static_cast<RDGComputePassNode*>(graph.Resolve(handle));
}

RDGCopyPassNode* GetCopyPassNode(RenderGraph& graph, RDGPassHandle handle) {
    return static_cast<RDGCopyPassNode*>(graph.Resolve(handle));
}

RDGResourceDependencyEdge* FindResourceEdge(RenderGraph& graph, RDGNodeHandle from, RDGNodeHandle to) {
    for (const auto& edgeHolder : graph._edges) {
        if (!edgeHolder || edgeHolder->GetTag() != RDGEdgeTag::ResourceDependency) {
            continue;
        }
        auto* edge = static_cast<RDGResourceDependencyEdge*>(edgeHolder.get());
        if (edge->_from != nullptr && edge->_to != nullptr && edge->_from->_id == from.Id && edge->_to->_id == to.Id) {
            return edge;
        }
    }
    return nullptr;
}

void RemoveEdge(vector<RDGEdge*>& edges, RDGEdge* edge) {
    edges.erase(std::remove(edges.begin(), edges.end(), edge), edges.end());
}

RDGPassHandle AddEmptyComputePass(RenderGraph& graph, std::string_view name) {
    return graph.AddComputePass(name, MakeComputePass([](IRDGComputePass::Builder&) {}));
}

class UnknownNode final : public RDGNode {
public:
    using RDGNode::RDGNode;

    RDGNodeTags GetTag() const noexcept override { return RDGNodeTag::UNKNOWN; }
};

class GenericResourceNode final : public RDGResourceNode {
public:
    GenericResourceNode(uint64_t id, std::string_view name, RDGResourceOwnership ownership) noexcept
        : RDGResourceNode(id, name, ownership) {}

    RDGNodeTags GetTag() const noexcept override { return RDGNodeTag::Resource; }
};

class GenericPassNode final : public RDGPassNode {
public:
    using RDGPassNode::RDGPassNode;

    RDGNodeTags GetTag() const noexcept override { return RDGNodeTag::Pass; }
};

class UnknownEdge final : public RDGEdge {
public:
    using RDGEdge::RDGEdge;

    RDGEdgeTags GetTag() const noexcept override { return RDGEdgeTag::UNKNOWN; }
};

struct BufferCopyGraph {
    RenderGraph Graph{};
    RDGBufferHandle Imported{};
    RDGBufferHandle Output{};
    RDGPassHandle Pass{};
};

BufferCopyGraph MakeValidBufferCopyGraph() {
    const auto allBuffer = BufferRange::AllRange();

    BufferCopyGraph result{};
    result.Imported = result.Graph.ImportBuffer(MakeValidGpuBufferHandle(1), RDGExecutionStage::Host, RDGMemoryAccess::HostWrite, allBuffer, "imported-buffer");
    result.Output = result.Graph.AddBuffer(64, MemoryType::Device, BufferUse::CopySource | BufferUse::CopyDestination, "output-buffer");

    RDGCopyPassBuilder builder{};
    builder.SetName("copy-pass");
    builder.CopyBufferToBuffer(result.Output, 0, result.Imported, 0, 64);
    builder._buffers.emplace_back(result.Imported, RDGBufferState{RDGExecutionStage::Copy, RDGMemoryAccess::TransferRead, allBuffer});
    builder._buffers.emplace_back(result.Output, RDGBufferState{RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, allBuffer});
    result.Pass = builder.Build(&result.Graph);
    return result;
}

struct ComputeBufferGraph {
    RenderGraph Graph{};
    RDGBufferHandle Input{};
    RDGBufferHandle Output{};
    RDGPassHandle Pass{};
};

ComputeBufferGraph MakeValidComputeBufferGraph() {
    const auto allBuffer = BufferRange::AllRange();

    ComputeBufferGraph result{};
    result.Input = result.Graph.ImportBuffer(MakeValidGpuBufferHandle(11), RDGExecutionStage::Host, RDGMemoryAccess::HostWrite, allBuffer, "imported-buffer");
    result.Output = result.Graph.AddBuffer(64, MemoryType::Device, BufferUse::Resource | BufferUse::UnorderedAccess, "storage-buffer");
    const auto input = result.Input;
    const auto output = result.Output;
    result.Pass = result.Graph.AddComputePass(
        "compute-pass",
        MakeComputePass([allBuffer, input, output](IRDGComputePass::Builder& builder) {
            builder.UseBuffer(input, allBuffer).UseRWBuffer(output, allBuffer);
        }));
    return result;
}

struct ComputeTextureGraph {
    RenderGraph Graph{};
    RDGTextureHandle Input{};
    RDGTextureHandle Output{};
    RDGPassHandle Pass{};
};

ComputeTextureGraph MakeValidComputeTextureGraph() {
    const auto allTexture = SubresourceRange::AllSub();

    ComputeTextureGraph result{};
    result.Input = result.Graph.ImportTexture(
        MakeValidGpuTextureHandle(21),
        RDGExecutionStage::Host,
        RDGMemoryAccess::HostWrite,
        RDGTextureLayout::General,
        allTexture,
        "imported-texture");
    result.Output = result.Graph.AddTexture(
        TextureDimension::Dim2D,
        4,
        4,
        1,
        1,
        1,
        TextureFormat::RGBA8_UNORM,
        MemoryType::Device,
        TextureUse::Resource | TextureUse::UnorderedAccess,
        "storage-texture");
    const auto input = result.Input;
    const auto output = result.Output;
    result.Pass = result.Graph.AddComputePass(
        "compute-pass",
        MakeComputePass([allTexture, input, output](IRDGComputePass::Builder& builder) {
            builder.UseTexture(input, allTexture).UseRWTexture(output, allTexture);
        }));
    return result;
}

struct GraphicsGraph {
    RenderGraph Graph{};
    RDGBufferHandle SceneBuffer{};
    RDGTextureHandle ColorTexture{};
    RDGTextureHandle DepthTexture{};
    RDGPassHandle Pass{};
};

GraphicsGraph MakeValidGraphicsGraph() {
    const auto allBuffer = BufferRange::AllRange();
    const auto allTexture = SubresourceRange::AllSub();

    GraphicsGraph result{};
    result.SceneBuffer = result.Graph.ImportBuffer(MakeValidGpuBufferHandle(31), RDGExecutionStage::Host, RDGMemoryAccess::HostWrite, allBuffer, "scene-buffer");
    result.ColorTexture = result.Graph.AddTexture(
        TextureDimension::Dim2D,
        4,
        4,
        1,
        1,
        1,
        TextureFormat::RGBA8_UNORM,
        MemoryType::Device,
        TextureUse::RenderTarget | TextureUse::Resource,
        "color-texture");
    result.DepthTexture = result.Graph.AddTexture(
        TextureDimension::Dim2D,
        4,
        4,
        1,
        1,
        1,
        TextureFormat::D32_FLOAT,
        MemoryType::Device,
        TextureUse::DepthStencilWrite | TextureUse::Resource,
        "depth-texture");
    const auto sceneBuffer = result.SceneBuffer;
    const auto colorTexture = result.ColorTexture;
    const auto depthTexture = result.DepthTexture;
    result.Pass = result.Graph.AddRasterPass(
        "graphics-pass",
        MakeRasterPass([allBuffer, allTexture, sceneBuffer, colorTexture, depthTexture](IRDGRasterPass::Builder& builder) {
            builder.UseCBuffer(sceneBuffer, ShaderStage::Graphics, allBuffer)
                .UseColorAttachment(
                    0,
                    colorTexture,
                    allTexture,
                    LoadAction::Clear,
                    StoreAction::Store,
                    ColorClearValue{{0.1f, 0.2f, 0.3f, 1.0f}})
                .UseDepthStencilAttachment(
                    depthTexture,
                    allTexture,
                    LoadAction::Clear,
                    StoreAction::Store,
                    LoadAction::DontCare,
                    StoreAction::Discard,
                    DepthStencilClearValue{1.0f, uint8_t{0}});
        }));
    return result;
}

TEST(RenderGraphValidateTest, Requirement_1_1_HandleMustBeValid) {
    RenderGraph graph{};
    auto node = make_unique<RDGBufferNode>(0, "buffer", RDGResourceOwnership::Internal);
    node->_id = std::numeric_limits<uint64_t>::max();
    graph._nodes.emplace_back(std::move(node));
    ExpectValidateFailContains(graph, "has invalid handle/id");
}

TEST(RenderGraphValidateTest, Requirement_1_2_NodeIdMustMatchIndex) {
    RenderGraph graph{};
    graph._nodes.emplace_back(make_unique<RDGBufferNode>(0, "buffer-a", RDGResourceOwnership::Internal));
    auto node = make_unique<RDGBufferNode>(1, "buffer-b", RDGResourceOwnership::Internal);
    node->_id = 0;
    graph._nodes.emplace_back(std::move(node));
    ExpectValidateFailContains(graph, "id/index mismatch");
}

TEST(RenderGraphValidateTest, Requirement_1_3_NodeTagMustNotBeUnknown) {
    RenderGraph graph{};
    graph._nodes.emplace_back(make_unique<UnknownNode>(0, "unknown"));
    ExpectValidateFailContains(graph, "has UNKNOWN tag");
}

TEST(RenderGraphValidateTest, Requirement_1_3_ResourceTagMustBeConcrete) {
    RenderGraph graph{};
    graph._nodes.emplace_back(make_unique<GenericResourceNode>(0, "resource", RDGResourceOwnership::Internal));
    ExpectValidateFailContains(graph, "resource tag is not concrete");
}

TEST(RenderGraphValidateTest, Requirement_1_3_PassTagMustBeConcrete) {
    RenderGraph graph{};
    graph._nodes.emplace_back(make_unique<GenericPassNode>(0, "pass"));
    ExpectValidateFailContains(graph, "pass tag is not concrete");
}

TEST(RenderGraphValidateTest, Requirement_2_1_EdgeEndpointsMustBeNonNull) {
    RenderGraph graph{};
    graph._nodes.emplace_back(make_unique<RDGBufferNode>(0, "buffer", RDGResourceOwnership::Internal));
    graph._nodes.emplace_back(make_unique<RDGCopyPassNode>(1, "pass"));
    graph._edges.emplace_back(make_unique<RDGPassDependencyEdge>(nullptr, graph._nodes[1].get()));
    ExpectValidateFailContains(graph, "has null endpoint");
}

TEST(RenderGraphValidateTest, Requirement_2_2_EdgeEndpointsMustExistInGraph) {
    RenderGraph graph{};
    graph._nodes.emplace_back(make_unique<RDGCopyPassNode>(0, "pass"));
    auto outsider = make_unique<RDGBufferNode>(1, "outsider", RDGResourceOwnership::Internal);
    auto edge = make_unique<RDGResourceDependencyEdge>(outsider.get(), graph._nodes[0].get(), RDGExecutionStage::Copy, RDGMemoryAccess::TransferRead);
    edge->_bufferRange = BufferRange::AllRange();
    graph._edges.emplace_back(std::move(edge));
    ExpectValidateFailContains(graph, "references node outside graph");
}

TEST(RenderGraphValidateTest, Requirement_2_3_EdgeTagMustBeConcrete) {
    RenderGraph graph{};
    graph._nodes.emplace_back(make_unique<RDGBufferNode>(0, "buffer", RDGResourceOwnership::Internal));
    graph._nodes.emplace_back(make_unique<RDGCopyPassNode>(1, "pass"));
    graph._edges.emplace_back(make_unique<UnknownEdge>(graph._nodes[0].get(), graph._nodes[1].get()));
    ExpectValidateFailContains(graph, "has UNKNOWN tag");
}

TEST(RenderGraphValidateTest, Requirement_2_4_EdgeMustBeRegisteredOnSource) {
    auto graphData = MakeValidBufferCopyGraph();
    auto* edge = FindResourceEdge(graphData.Graph, graphData.Imported, graphData.Pass);
    ASSERT_NE(edge, nullptr);
    RemoveEdge(GetBufferNode(graphData.Graph, graphData.Imported)->_outEdges, edge);
    ExpectValidateFailContains(graphData.Graph, "missing from source out-edges");
}

TEST(RenderGraphValidateTest, Requirement_2_4_EdgeMustBeRegisteredOnTarget) {
    auto graphData = MakeValidBufferCopyGraph();
    auto* edge = FindResourceEdge(graphData.Graph, graphData.Imported, graphData.Pass);
    ASSERT_NE(edge, nullptr);
    RemoveEdge(GetCopyPassNode(graphData.Graph, graphData.Pass)->_inEdges, edge);
    ExpectValidateFailContains(graphData.Graph, "missing from target in-edges");
}

TEST(RenderGraphValidateTest, Requirement_2_4_NodeInEdgesMustBeGraphOwned) {
    auto graphData = MakeValidBufferCopyGraph();
    auto foreignEdge = make_unique<RDGPassDependencyEdge>(GetCopyPassNode(graphData.Graph, graphData.Pass), GetCopyPassNode(graphData.Graph, graphData.Pass));
    GetBufferNode(graphData.Graph, graphData.Output)->_inEdges.emplace_back(foreignEdge.get());
    ExpectValidateFailContains(graphData.Graph, "has in-edge not owned by graph");
}

TEST(RenderGraphValidateTest, Requirement_2_4_NodeOutEdgesMustBeGraphOwned) {
    auto graphData = MakeValidBufferCopyGraph();
    auto foreignEdge = make_unique<RDGPassDependencyEdge>(GetCopyPassNode(graphData.Graph, graphData.Pass), GetCopyPassNode(graphData.Graph, graphData.Pass));
    GetBufferNode(graphData.Graph, graphData.Output)->_outEdges.emplace_back(foreignEdge.get());
    ExpectValidateFailContains(graphData.Graph, "has out-edge not owned by graph");
}

TEST(RenderGraphValidateTest, Requirement_2_4_NodeInEdgeDestinationMustMatch) {
    auto graphData = MakeValidBufferCopyGraph();
    auto* edge = FindResourceEdge(graphData.Graph, graphData.Imported, graphData.Pass);
    ASSERT_NE(edge, nullptr);
    GetBufferNode(graphData.Graph, graphData.Output)->_inEdges.emplace_back(edge);
    ExpectValidateFailContains(graphData.Graph, "has in-edge with mismatched destination");
}

TEST(RenderGraphValidateTest, Requirement_2_4_NodeOutEdgeSourceMustMatch) {
    auto graphData = MakeValidBufferCopyGraph();
    auto* edge = FindResourceEdge(graphData.Graph, graphData.Imported, graphData.Pass);
    ASSERT_NE(edge, nullptr);
    GetBufferNode(graphData.Graph, graphData.Output)->_outEdges.emplace_back(edge);
    ExpectValidateFailContains(graphData.Graph, "has out-edge with mismatched source");
}

TEST(RenderGraphValidateTest, Requirement_2_5_EdgesMustNotSelfLoop) {
    auto graphData = MakeValidBufferCopyGraph();
    auto* passNode = GetCopyPassNode(graphData.Graph, graphData.Pass);
    auto edge = make_unique<RDGPassDependencyEdge>(passNode, passNode);
    auto* raw = edge.get();
    graphData.Graph._edges.emplace_back(std::move(edge));
    passNode->_outEdges.emplace_back(raw);
    passNode->_inEdges.emplace_back(raw);
    ExpectValidateFailContains(graphData.Graph, "self loop");
}

TEST(RenderGraphValidateTest, Requirement_3_1_ResourceDependencyMustConnectPassAndResource) {
    auto graphData = MakeValidBufferCopyGraph();
    auto* from = GetBufferNode(graphData.Graph, graphData.Imported);
    auto* to = GetBufferNode(graphData.Graph, graphData.Output);
    auto edge = make_unique<RDGResourceDependencyEdge>(from, to, RDGExecutionStage::Copy, RDGMemoryAccess::TransferRead);
    edge->_bufferRange = BufferRange::AllRange();
    auto* raw = edge.get();
    graphData.Graph._edges.emplace_back(std::move(edge));
    from->_outEdges.emplace_back(raw);
    to->_inEdges.emplace_back(raw);
    ExpectValidateFailContains(graphData.Graph, "must connect one resource and one pass");
}

TEST(RenderGraphValidateTest, Requirement_3_2_ReadOnlyResourceDependencyMustPointResourceToPass) {
    auto graphData = MakeValidBufferCopyGraph();
    auto* passNode = GetCopyPassNode(graphData.Graph, graphData.Pass);
    auto* resourceNode = GetBufferNode(graphData.Graph, graphData.Imported);
    auto edge = make_unique<RDGResourceDependencyEdge>(passNode, resourceNode, RDGExecutionStage::Copy, RDGMemoryAccess::TransferRead);
    edge->_bufferRange = BufferRange::AllRange();
    auto* raw = edge.get();
    graphData.Graph._edges.emplace_back(std::move(edge));
    passNode->_outEdges.emplace_back(raw);
    resourceNode->_inEdges.emplace_back(raw);
    ExpectValidateFailContains(graphData.Graph, "must point Resource -> Pass");
}

TEST(RenderGraphValidateTest, Requirement_3_3_WriteResourceDependencyMustPointPassToResource) {
    auto graphData = MakeValidBufferCopyGraph();
    auto* passNode = GetCopyPassNode(graphData.Graph, graphData.Pass);
    auto* resourceNode = GetBufferNode(graphData.Graph, graphData.Output);
    auto edge = make_unique<RDGResourceDependencyEdge>(resourceNode, passNode, RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite);
    edge->_bufferRange = BufferRange::AllRange();
    auto* raw = edge.get();
    graphData.Graph._edges.emplace_back(std::move(edge));
    resourceNode->_outEdges.emplace_back(raw);
    passNode->_inEdges.emplace_back(raw);
    ExpectValidateFailContains(graphData.Graph, "must point Pass -> Resource");
}

TEST(RenderGraphValidateTest, Requirement_3_4_BufferEdgeMustNotCarryTextureState) {
    auto graphData = MakeValidBufferCopyGraph();
    auto* edge = FindResourceEdge(graphData.Graph, graphData.Imported, graphData.Pass);
    ASSERT_NE(edge, nullptr);
    edge->_textureLayout = RDGTextureLayout::General;
    edge->_textureRange = SubresourceRange::AllSub();
    ExpectValidateFailContains(graphData.Graph, "buffer edge");
    ExpectValidateFailContains(graphData.Graph, "carries texture state");
}

TEST(RenderGraphValidateTest, Requirement_3_5_TextureEdgeMustNotCarryBufferRange) {
    auto graphData = MakeValidComputeTextureGraph();
    auto* edge = FindResourceEdge(graphData.Graph, graphData.Input, graphData.Pass);
    ASSERT_NE(edge, nullptr);
    edge->_bufferRange = BufferRange::AllRange();
    ExpectValidateFailContains(graphData.Graph, "texture edge");
    ExpectValidateFailContains(graphData.Graph, "carries buffer range");
}

TEST(RenderGraphValidateTest, Requirement_3_6_ResourceDependencyStageMustNotBeNone) {
    auto graphData = MakeValidBufferCopyGraph();
    auto* edge = FindResourceEdge(graphData.Graph, graphData.Imported, graphData.Pass);
    ASSERT_NE(edge, nullptr);
    edge->_stage = RDGExecutionStage::NONE;
    ExpectValidateFailContains(graphData.Graph, "has NONE stage");
}

TEST(RenderGraphValidateTest, Requirement_3_7_ResourceDependencyAccessMustNotBeNone) {
    auto graphData = MakeValidBufferCopyGraph();
    auto* edge = FindResourceEdge(graphData.Graph, graphData.Imported, graphData.Pass);
    ASSERT_NE(edge, nullptr);
    edge->_access = RDGMemoryAccess::NONE;
    ExpectValidateFailContains(graphData.Graph, "has NONE access");
}

TEST(RenderGraphValidateTest, Requirement_3_8_ResourceDependencyMustBeUnique) {
    auto graphData = MakeValidBufferCopyGraph();
    graphData.Graph.Link(graphData.Imported, graphData.Pass, RDGExecutionStage::Copy, RDGMemoryAccess::TransferRead, BufferRange::AllRange());
    ExpectValidateFailContains(graphData.Graph, "duplicate resource dependency edge");
}

TEST(RenderGraphValidateTest, Requirement_4_1_PassDependencyMustConnectTwoPasses) {
    auto graphData = MakeValidBufferCopyGraph();
    auto* resourceNode = GetBufferNode(graphData.Graph, graphData.Imported);
    auto* passNode = GetCopyPassNode(graphData.Graph, graphData.Pass);
    auto edge = make_unique<RDGPassDependencyEdge>(resourceNode, passNode);
    auto* raw = edge.get();
    graphData.Graph._edges.emplace_back(std::move(edge));
    resourceNode->_outEdges.emplace_back(raw);
    passNode->_inEdges.emplace_back(raw);
    ExpectValidateFailContains(graphData.Graph, "must connect two passes");
}

TEST(RenderGraphValidateTest, Requirement_4_2_PassDependencyMustNotSelfDepend) {
    auto graphData = MakeValidBufferCopyGraph();
    graphData.Graph.AddPassDependency(graphData.Pass, graphData.Pass);
    ExpectValidateFailContains(graphData.Graph, "self loop");
}

TEST(RenderGraphValidateTest, Requirement_4_3_PassDependencyMustBeUnique) {
    auto graphData = MakeValidBufferCopyGraph();
    const auto otherPass = graphData.Graph.AddCopyPass("other-copy-pass");
    graphData.Graph.AddPassDependency(graphData.Pass, otherPass);
    graphData.Graph.AddPassDependency(graphData.Pass, otherPass);
    ExpectValidateFailContains(graphData.Graph, "duplicate pass dependency edge");
}

TEST(RenderGraphValidateTest, Requirement_5_1_GraphMustBeDag) {
    auto graphData = MakeValidBufferCopyGraph();
    graphData.Graph.Link(graphData.Output, graphData.Pass, RDGExecutionStage::Copy, RDGMemoryAccess::TransferRead, BufferRange::AllRange());
    ExpectValidateFailContains(graphData.Graph, "graph contains a cycle");
}

TEST(RenderGraphValidateTest, Requirement_5_2_PassMustBeReachableFromRootResource) {
    RenderGraph graph{};
    const auto output = graph.AddBuffer(64, MemoryType::Device, BufferUse::CopyDestination, "output");
    const auto pass = graph.AddCopyPass("orphan-pass");
    graph.Link(pass, output, RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, BufferRange::AllRange());
    ExpectValidateFailContains(graph, "is not reachable from any import/root resource");
}

TEST(RenderGraphValidateTest, Requirement_5_3_PassMustHaveResourceDependency) {
    auto graphData = MakeValidBufferCopyGraph();
    const auto emptyPass = graphData.Graph.AddCopyPass("empty-pass");
    graphData.Graph.AddPassDependency(graphData.Pass, emptyPass);
    ExpectValidateFailContains(graphData.Graph, "has no resource dependency edge");
}

TEST(RenderGraphValidateTest, Requirement_5_4_ResourceMustBeReferencedByPass) {
    auto graphData = MakeValidBufferCopyGraph();
    graphData.Graph.AddBuffer(16, MemoryType::Device, BufferUse::Common, "unused-buffer");
    ExpectValidateFailContains(graphData.Graph, "is not referenced by any pass");
}

TEST(RenderGraphValidateTest, Requirement_6_1_1_ResourceOwnershipMustNotBeUnknown) {
    auto graphData = MakeValidBufferCopyGraph();
    GetBufferNode(graphData.Graph, graphData.Output)->_ownership = RDGResourceOwnership::UNKNOWN;
    ExpectValidateFailContains(graphData.Graph, "has UNKNOWN ownership");
}

TEST(RenderGraphValidateTest, Requirement_6_1_2_ExternalBufferMustHaveImportState) {
    auto graphData = MakeValidBufferCopyGraph();
    GetBufferNode(graphData.Graph, graphData.Imported)->_importState.reset();
    ExpectValidateFailContains(graphData.Graph, "has no import state");
}

TEST(RenderGraphValidateTest, Requirement_6_1_3_InternalBufferMustNotCarryImportState) {
    auto graphData = MakeValidBufferCopyGraph();
    auto* node = GetBufferNode(graphData.Graph, graphData.Output);
    node->_importBuffer = MakeValidGpuBufferHandle(77);
    node->_importState = RDGBufferState{RDGExecutionStage::Host, RDGMemoryAccess::HostWrite, BufferRange::AllRange()};
    ExpectValidateFailContains(graphData.Graph, "must not carry import state");
}

TEST(RenderGraphValidateTest, Requirement_6_2_1_InternalBufferSizeMustBePositive) {
    auto graphData = MakeValidBufferCopyGraph();
    GetBufferNode(graphData.Graph, graphData.Output)->_size = 0;
    ExpectValidateFailContains(graphData.Graph, "size must be > 0");
}

TEST(RenderGraphValidateTest, Requirement_6_2_2_BufferUsageMustBeKnown) {
    auto graphData = MakeValidBufferCopyGraph();
    GetBufferNode(graphData.Graph, graphData.Output)->_usage = BufferUse::UNKNOWN;
    ExpectValidateFailContains(graphData.Graph, "usage is UNKNOWN");
}

TEST(RenderGraphValidateTest, Requirement_6_2_3_ExternalBufferImportHandleMustBeValid) {
    auto graphData = MakeValidBufferCopyGraph();
    GetBufferNode(graphData.Graph, graphData.Imported)->_importBuffer.Invalidate();
    ExpectValidateFailContains(graphData.Graph, "has invalid import handle");
}

TEST(RenderGraphValidateTest, Requirement_6_2_4_BufferRangesMustStayInBounds) {
    auto graphData = MakeValidBufferCopyGraph();
    auto* edge = FindResourceEdge(graphData.Graph, graphData.Pass, graphData.Output);
    ASSERT_NE(edge, nullptr);
    edge->_bufferRange = BufferRange{48, 32};
    ExpectValidateFailContains(graphData.Graph, "range is invalid");
}

TEST(RenderGraphValidateTest, Requirement_6_3_1_TextureDimensionMustBeKnown) {
    auto graphData = MakeValidComputeTextureGraph();
    GetTextureNode(graphData.Graph, graphData.Output)->_dim = TextureDimension::UNKNOWN;
    ExpectValidateFailContains(graphData.Graph, "dimension is UNKNOWN");
}

TEST(RenderGraphValidateTest, Requirement_6_3_2_TextureExtentAndMipValuesMustBeValid) {
    auto graphData = MakeValidComputeTextureGraph();
    GetTextureNode(graphData.Graph, graphData.Output)->_width = 0;
    ExpectValidateFailContains(graphData.Graph, "has invalid extent/mip/sample values");
}

TEST(RenderGraphValidateTest, Requirement_6_3_3_TextureFormatMustBeKnown) {
    auto graphData = MakeValidComputeTextureGraph();
    GetTextureNode(graphData.Graph, graphData.Output)->_format = TextureFormat::UNKNOWN;
    ExpectValidateFailContains(graphData.Graph, "format is UNKNOWN");
}

TEST(RenderGraphValidateTest, Requirement_6_3_4_TextureUsageMustBeKnown) {
    auto graphData = MakeValidComputeTextureGraph();
    GetTextureNode(graphData.Graph, graphData.Output)->_usage = TextureUse::UNKNOWN;
    ExpectValidateFailContains(graphData.Graph, "usage is UNKNOWN");
}

TEST(RenderGraphValidateTest, Requirement_6_3_5_ExternalTextureImportHandleMustBeValid) {
    auto graphData = MakeValidComputeTextureGraph();
    GetTextureNode(graphData.Graph, graphData.Input)->_importTexture.Invalidate();
    ExpectValidateFailContains(graphData.Graph, "has invalid import handle");
}

TEST(RenderGraphValidateTest, Requirement_6_3_6_TextureRangesMustStayInBounds) {
    auto graphData = MakeValidComputeTextureGraph();
    auto* edge = FindResourceEdge(graphData.Graph, graphData.Pass, graphData.Output);
    ASSERT_NE(edge, nullptr);
    edge->_textureRange = SubresourceRange{1, 1, 0, 1};
    ExpectValidateFailContains(graphData.Graph, "subresource range is invalid");
}

TEST(RenderGraphValidateTest, Requirement_7_1_1_PassMustHaveWriteOutput) {
    const auto allBuffer = BufferRange::AllRange();
    RenderGraph graph{};
    const auto input = graph.ImportBuffer(MakeValidGpuBufferHandle(101), RDGExecutionStage::Host, RDGMemoryAccess::HostWrite, allBuffer, "input");
    graph.AddComputePass(
        "read-only-pass",
        MakeComputePass([=](IRDGComputePass::Builder& builder) {
            builder.UseBuffer(input, allBuffer);
        }));
    ExpectValidateFailContains(graph, "has no resource write output");
}

TEST(RenderGraphValidateTest, Requirement_7_2_1_GraphicsPassImplMustNotBeNull) {
    auto graphData = MakeValidGraphicsGraph();
    GetGraphicsPassNode(graphData.Graph, graphData.Pass)->_impl.reset();
    ExpectValidateFailContains(graphData.Graph, "has null implementation");
}

TEST(RenderGraphValidateTest, Requirement_7_2_2_ColorAttachmentSlotsMustBeUnique) {
    auto graphData = MakeValidGraphicsGraph();
    auto* passNode = GetGraphicsPassNode(graphData.Graph, graphData.Pass);
    passNode->_colorAttachments.emplace_back(passNode->_colorAttachments.front());
    ExpectValidateFailContains(graphData.Graph, "duplicate color attachment slot");
}

TEST(RenderGraphValidateTest, Requirement_7_2_3_ColorAttachmentSlotsMustBeContiguous) {
    auto graphData = MakeValidGraphicsGraph();
    auto* passNode = GetGraphicsPassNode(graphData.Graph, graphData.Pass);
    auto attachment = passNode->_colorAttachments.front();
    attachment.Slot = 2;
    passNode->_colorAttachments.emplace_back(attachment);
    ExpectValidateFailContains(graphData.Graph, "color attachment slots are not contiguous");
}

TEST(RenderGraphValidateTest, Requirement_7_2_4_ColorAttachmentHandleMustBeValid) {
    auto graphData = MakeValidGraphicsGraph();
    GetGraphicsPassNode(graphData.Graph, graphData.Pass)->_colorAttachments.front().Texture = RDGTextureHandle{999};
    ExpectValidateFailContains(graphData.Graph, "invalid color attachment handle");
}

TEST(RenderGraphValidateTest, Requirement_7_2_5_ColorAttachmentMustNotUseDepthFormatTexture) {
    auto graphData = MakeValidGraphicsGraph();
    GetGraphicsPassNode(graphData.Graph, graphData.Pass)->_colorAttachments.front().Texture = graphData.DepthTexture;
    ExpectValidateFailContains(graphData.Graph, "uses depth format texture as color attachment");
}

TEST(RenderGraphValidateTest, Requirement_7_2_6_DepthAttachmentMustUseDepthFormatTexture) {
    auto graphData = MakeValidGraphicsGraph();
    GetGraphicsPassNode(graphData.Graph, graphData.Pass)->_depthStencilAttachment->Texture = graphData.ColorTexture;
    ExpectValidateFailContains(graphData.Graph, "depth-stencil attachment must use depth format");
}

TEST(RenderGraphValidateTest, Requirement_7_2_7_ColorAndDepthAttachmentsMustNotAlias) {
    auto graphData = MakeValidGraphicsGraph();
    const auto seed = graphData.Graph.ImportBuffer(
        MakeValidGpuBufferHandle(87),
        RDGExecutionStage::Host,
        RDGMemoryAccess::HostWrite,
        BufferRange::AllRange(),
        "keepalive-seed");
    const auto keepAliveOutput = graphData.Graph.AddBuffer(
        64,
        MemoryType::Device,
        BufferUse::Resource | BufferUse::UnorderedAccess,
        "keepalive-output");
    const auto keepAlivePass = AddEmptyComputePass(graphData.Graph, "keepalive-pass");
    const auto aliasTexture = graphData.Graph.ImportTexture(
        MakeValidGpuTextureHandle(88),
        RDGExecutionStage::Host,
        RDGMemoryAccess::HostWrite,
        RDGTextureLayout::General,
        SubresourceRange::AllSub(),
        "attachment-alias");
    auto* aliasNode = GetTextureNode(graphData.Graph, aliasTexture);
    auto* colorEdge = FindResourceEdge(graphData.Graph, graphData.Pass, graphData.ColorTexture);
    auto* depthEdge = FindResourceEdge(graphData.Graph, graphData.Pass, graphData.DepthTexture);
    ASSERT_NE(colorEdge, nullptr);
    ASSERT_NE(depthEdge, nullptr);
    graphData.Graph.Link(seed, keepAlivePass, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderRead, BufferRange::AllRange());
    graphData.Graph.Link(graphData.ColorTexture, keepAlivePass, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderRead, RDGTextureLayout::ShaderReadOnly, SubresourceRange::AllSub());
    graphData.Graph.Link(graphData.DepthTexture, keepAlivePass, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderRead, RDGTextureLayout::General, SubresourceRange::AllSub());
    graphData.Graph.Link(keepAlivePass, keepAliveOutput, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderWrite, BufferRange::AllRange());
    RemoveEdge(GetTextureNode(graphData.Graph, graphData.ColorTexture)->_inEdges, colorEdge);
    RemoveEdge(GetTextureNode(graphData.Graph, graphData.DepthTexture)->_inEdges, depthEdge);
    colorEdge->_to = aliasNode;
    depthEdge->_to = aliasNode;
    aliasNode->_inEdges.emplace_back(colorEdge);
    aliasNode->_inEdges.emplace_back(depthEdge);
    auto* passNode = GetGraphicsPassNode(graphData.Graph, graphData.Pass);
    passNode->_colorAttachments.front().Texture = aliasTexture;
    passNode->_depthStencilAttachment->Texture = aliasTexture;
    ExpectValidateFailContains(graphData.Graph, "uses same texture as color and depth-stencil attachment");
}

TEST(RenderGraphValidateTest, Requirement_7_2_8_ColorClearNeedsClearValue) {
    auto graphData = MakeValidGraphicsGraph();
    auto& attachment = GetGraphicsPassNode(graphData.Graph, graphData.Pass)->_colorAttachments.front();
    attachment.Load = LoadAction::Clear;
    attachment.ClearValue.reset();
    ExpectValidateFailContains(graphData.Graph, "clears without clear value");
}

TEST(RenderGraphValidateTest, Requirement_7_2_8_DepthClearNeedsClearValue) {
    auto graphData = MakeValidGraphicsGraph();
    auto& attachment = GetGraphicsPassNode(graphData.Graph, graphData.Pass)->_depthStencilAttachment.value();
    attachment.DepthLoad = LoadAction::Clear;
    attachment.ClearValue.reset();
    ExpectValidateFailContains(graphData.Graph, "depth-stencil attachment clears without clear value");
}

TEST(RenderGraphValidateTest, Requirement_7_2_9_GraphicsPassStagesMustBeGraphicsOnly) {
    auto graphData = MakeValidGraphicsGraph();
    auto* edge = FindResourceEdge(graphData.Graph, graphData.SceneBuffer, graphData.Pass);
    ASSERT_NE(edge, nullptr);
    edge->_stage = RDGExecutionStage::ComputeShader;
    ExpectValidateFailContains(graphData.Graph, "uses non-graphics stage");
}

TEST(RenderGraphValidateTest, Requirement_7_3_1_ComputePassImplMustNotBeNull) {
    auto graphData = MakeValidComputeBufferGraph();
    GetComputePassNode(graphData.Graph, graphData.Pass)->_impl.reset();
    ExpectValidateFailContains(graphData.Graph, "has null implementation");
}

TEST(RenderGraphValidateTest, Requirement_7_3_2_ComputePassStageMustBeComputeShader) {
    auto graphData = MakeValidComputeBufferGraph();
    auto* edge = FindResourceEdge(graphData.Graph, graphData.Input, graphData.Pass);
    ASSERT_NE(edge, nullptr);
    edge->_stage = RDGExecutionStage::PixelShader;
    ExpectValidateFailContains(graphData.Graph, "uses stage");
}

TEST(RenderGraphValidateTest, Requirement_7_3_3_ComputePassMustRejectGraphicsOnlyAccess) {
    auto graphData = MakeValidComputeBufferGraph();
    auto* edge = FindResourceEdge(graphData.Graph, graphData.Input, graphData.Pass);
    ASSERT_NE(edge, nullptr);
    edge->_access = RDGMemoryAccess::VertexRead;
    ExpectValidateFailContains(graphData.Graph, "incompatible with stage");
}

TEST(RenderGraphValidateTest, Requirement_7_4_1_CopyPassMustHaveCopyRecord) {
    auto graphData = MakeValidBufferCopyGraph();
    GetCopyPassNode(graphData.Graph, graphData.Pass)->_copys.clear();
    ExpectValidateFailContains(graphData.Graph, "has no copy record");
}

TEST(RenderGraphValidateTest, Requirement_7_4_2_CopyPassStageMustBeCopy) {
    auto graphData = MakeValidBufferCopyGraph();
    auto* edge = FindResourceEdge(graphData.Graph, graphData.Imported, graphData.Pass);
    ASSERT_NE(edge, nullptr);
    edge->_stage = RDGExecutionStage::Host;
    edge->_access = RDGMemoryAccess::HostRead;
    ExpectValidateFailContains(graphData.Graph, "uses stage");
}

TEST(RenderGraphValidateTest, Requirement_7_4_3_CopyPassAccessMustBeTransferOnly) {
    auto graphData = MakeValidBufferCopyGraph();
    auto* edge = FindResourceEdge(graphData.Graph, graphData.Imported, graphData.Pass);
    ASSERT_NE(edge, nullptr);
    edge->_access = RDGMemoryAccess::HostRead;
    ExpectValidateFailContains(graphData.Graph, "incompatible with stage");
}

TEST(RenderGraphValidateTest, Requirement_7_4_4_CopyRecordHandlesMustBeValid) {
    auto graphData = MakeValidBufferCopyGraph();
    auto* passNode = GetCopyPassNode(graphData.Graph, graphData.Pass);
    auto* record = std::get_if<RDGCopyBufferToBufferRecord>(&passNode->_copys.front());
    ASSERT_NE(record, nullptr);
    record->Dst = RDGBufferHandle{999};
    ExpectValidateFailContains(graphData.Graph, "invalid buffer-to-buffer record");
}

TEST(RenderGraphValidateTest, Requirement_7_4_5_BufferCopyWithinSameBufferMustNotOverlap) {
    auto graphData = MakeValidBufferCopyGraph();
    auto* passNode = GetCopyPassNode(graphData.Graph, graphData.Pass);
    auto* record = std::get_if<RDGCopyBufferToBufferRecord>(&passNode->_copys.front());
    ASSERT_NE(record, nullptr);
    record->Dst = record->Src;
    record->DstOffset = 8;
    record->SrcOffset = 16;
    record->Size = 16;
    ExpectValidateFailContains(graphData.Graph, "copies overlapping ranges within the same buffer");
}

TEST(RenderGraphValidateTest, Requirement_8_1_BufferImportStageAndAccessMustBeSpecified) {
    auto graphData = MakeValidBufferCopyGraph();
    GetBufferNode(graphData.Graph, graphData.Imported)->_importState->Access = RDGMemoryAccess::NONE;
    ExpectValidateFailContains(graphData.Graph, "buffer import state on");
    ExpectValidateFailContains(graphData.Graph, "is incomplete");
}

TEST(RenderGraphValidateTest, Requirement_8_2_TextureImportLayoutMustBeKnown) {
    auto graphData = MakeValidComputeTextureGraph();
    GetTextureNode(graphData.Graph, graphData.Input)->_importState->Layout = RDGTextureLayout::UNKNOWN;
    ExpectValidateFailContains(graphData.Graph, "texture import state on");
    ExpectValidateFailContains(graphData.Graph, "has UNKNOWN layout");
}

TEST(RenderGraphValidateTest, Requirement_8_3_BufferExportStageAndAccessMustBeSpecified) {
    auto graphData = MakeValidBufferCopyGraph();
    graphData.Graph.ExportBuffer(graphData.Output, RDGExecutionStage::Host, RDGMemoryAccess::HostRead, BufferRange::AllRange());
    GetBufferNode(graphData.Graph, graphData.Output)->_exportState->Stage = RDGExecutionStage::NONE;
    ExpectValidateFailContains(graphData.Graph, "buffer export state on");
    ExpectValidateFailContains(graphData.Graph, "is incomplete");
}

TEST(RenderGraphValidateTest, Requirement_8_4_TextureExportLayoutMustBeKnown) {
    auto graphData = MakeValidComputeTextureGraph();
    graphData.Graph.ExportTexture(graphData.Output, RDGExecutionStage::Host, RDGMemoryAccess::HostRead, RDGTextureLayout::General, SubresourceRange::AllSub());
    GetTextureNode(graphData.Graph, graphData.Output)->_exportState->Layout = RDGTextureLayout::UNKNOWN;
    ExpectValidateFailContains(graphData.Graph, "texture export state on");
    ExpectValidateFailContains(graphData.Graph, "has UNKNOWN layout");
}

TEST(RenderGraphValidateTest, Requirement_8_5_ExportedBufferMustHaveWriter) {
    const auto allBuffer = BufferRange::AllRange();
    RenderGraph graph{};
    const auto seed = graph.ImportBuffer(MakeValidGpuBufferHandle(201), RDGExecutionStage::Host, RDGMemoryAccess::HostWrite, allBuffer, "seed");
    const auto exported = graph.AddBuffer(64, MemoryType::Device, BufferUse::Resource, "exported");
    const auto output = graph.AddBuffer(64, MemoryType::Device, BufferUse::Resource | BufferUse::UnorderedAccess, "output");
    graph.AddComputePass(
        "compute-pass",
        MakeComputePass([=](IRDGComputePass::Builder& builder) {
            builder.UseBuffer(seed, allBuffer).UseBuffer(exported, allBuffer).UseRWBuffer(output, allBuffer);
        }));
    graph.ExportBuffer(exported, RDGExecutionStage::Host, RDGMemoryAccess::HostRead, allBuffer);
    ExpectValidateFailContains(graph, "exported buffer");
    ExpectValidateFailContains(graph, "has no pass write source");
}

TEST(RenderGraphValidateTest, Requirement_8_5_ExportedTextureMustHaveWriter) {
    const auto allTexture = SubresourceRange::AllSub();
    RenderGraph graph{};
    const auto seed = graph.ImportTexture(
        MakeValidGpuTextureHandle(202),
        RDGExecutionStage::Host,
        RDGMemoryAccess::HostWrite,
        RDGTextureLayout::General,
        allTexture,
        "seed");
    const auto exported = graph.AddTexture(
        TextureDimension::Dim2D,
        4,
        4,
        1,
        1,
        1,
        TextureFormat::RGBA8_UNORM,
        MemoryType::Device,
        TextureUse::Resource,
        "exported");
    const auto output = graph.AddTexture(
        TextureDimension::Dim2D,
        4,
        4,
        1,
        1,
        1,
        TextureFormat::RGBA8_UNORM,
        MemoryType::Device,
        TextureUse::Resource | TextureUse::UnorderedAccess,
        "output");
    graph.AddComputePass(
        "compute-pass",
        MakeComputePass([=](IRDGComputePass::Builder& builder) {
            builder.UseTexture(seed, allTexture).UseTexture(exported, allTexture).UseRWTexture(output, allTexture);
        }));
    graph.ExportTexture(exported, RDGExecutionStage::Host, RDGMemoryAccess::HostRead, RDGTextureLayout::General, allTexture);
    ExpectValidateFailContains(graph, "exported texture");
    ExpectValidateFailContains(graph, "has no pass write source");
}

TEST(RenderGraphValidateTest, Requirement_9_1_VertexInputStageRejectsShaderRead) {
    auto graphData = MakeValidGraphicsGraph();
    auto* edge = FindResourceEdge(graphData.Graph, graphData.SceneBuffer, graphData.Pass);
    ASSERT_NE(edge, nullptr);
    edge->_stage = RDGExecutionStage::VertexInput;
    edge->_access = RDGMemoryAccess::ShaderRead;
    ExpectValidateFailContains(graphData.Graph, "incompatible with stage");
}

TEST(RenderGraphValidateTest, Requirement_9_2_ComputeStageRejectsTransferRead) {
    auto graphData = MakeValidComputeBufferGraph();
    auto* edge = FindResourceEdge(graphData.Graph, graphData.Input, graphData.Pass);
    ASSERT_NE(edge, nullptr);
    edge->_access = RDGMemoryAccess::TransferRead;
    ExpectValidateFailContains(graphData.Graph, "incompatible with stage");
}

TEST(RenderGraphValidateTest, Requirement_9_3_HostStageRejectsShaderRead) {
    auto graphData = MakeValidBufferCopyGraph();
    auto* edge = FindResourceEdge(graphData.Graph, graphData.Imported, graphData.Pass);
    ASSERT_NE(edge, nullptr);
    edge->_stage = RDGExecutionStage::Host;
    edge->_access = RDGMemoryAccess::ShaderRead;
    ExpectValidateFailContains(graphData.Graph, "incompatible with stage");
}

TEST(RenderGraphValidateTest, Requirement_9_4_CombinedStagesAllowUnionOfAccesses) {
    auto graphData = MakeValidGraphicsGraph();
    ExpectValidateOk(graphData.Graph);
}

TEST(RenderGraphValidateTest, Requirement_10_1_SamePassOverlappingTextureUsagesMustHaveCompatibleLayouts) {
    const auto allTexture = SubresourceRange::AllSub();
    RenderGraph graph{};
    const auto input = graph.ImportTexture(
        MakeValidGpuTextureHandle(301),
        RDGExecutionStage::Host,
        RDGMemoryAccess::HostWrite,
        RDGTextureLayout::General,
        allTexture,
        "input");
    const auto output = graph.AddBuffer(64, MemoryType::Device, BufferUse::Resource | BufferUse::UnorderedAccess, "output");
    const auto pass = AddEmptyComputePass(graph, "compute-pass");
    graph.Link(input, pass, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderRead, RDGTextureLayout::ShaderReadOnly, allTexture);
    graph.Link(input, pass, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderRead, RDGTextureLayout::Present, allTexture);
    graph.Link(pass, output, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderWrite, BufferRange::AllRange());
    ExpectValidateFailContains(graph, "uses conflicting layouts on the same texture subresource");
}

TEST(RenderGraphValidateTest, Requirement_10_2_TextureLayoutMustMatchAccess) {
    auto graphData = MakeValidComputeTextureGraph();
    auto* edge = FindResourceEdge(graphData.Graph, graphData.Input, graphData.Pass);
    ASSERT_NE(edge, nullptr);
    edge->_textureLayout = RDGTextureLayout::TransferDestination;
    ExpectValidateFailContains(graphData.Graph, "layout");
    ExpectValidateFailContains(graphData.Graph, "incompatible with access");
}

TEST(RenderGraphValidateTest, Requirement_10_3_ImportExportTextureWithoutPassUsageMustKeepLayout) {
    RenderGraph graph{};
    const auto texture = graph.ImportTexture(
        MakeValidGpuTextureHandle(401),
        RDGExecutionStage::Host,
        RDGMemoryAccess::HostWrite,
        RDGTextureLayout::General,
        SubresourceRange::AllSub(),
        "external-texture");
    graph.ExportTexture(texture, RDGExecutionStage::Host, RDGMemoryAccess::HostRead, RDGTextureLayout::Present, SubresourceRange::AllSub());
    ExpectValidateFailContains(graph, "changes layout without any pass usage");
}

TEST(RenderGraphValidateTest, Requirement_11_1_OverlappingWritesMustBeOrdered) {
    const auto rangeA = BufferRange{0, 32};
    const auto rangeB = BufferRange{16, 32};
    RenderGraph graph{};
    const auto seedA = graph.ImportBuffer(MakeValidGpuBufferHandle(501), RDGExecutionStage::Host, RDGMemoryAccess::HostWrite, BufferRange::AllRange(), "seed-a");
    const auto seedB = graph.ImportBuffer(MakeValidGpuBufferHandle(502), RDGExecutionStage::Host, RDGMemoryAccess::HostWrite, BufferRange::AllRange(), "seed-b");
    const auto target = graph.AddBuffer(64, MemoryType::Device, BufferUse::Resource | BufferUse::UnorderedAccess, "target");
    const auto passA = AddEmptyComputePass(graph, "write-a");
    const auto passB = AddEmptyComputePass(graph, "write-b");
    graph.Link(seedA, passA, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderRead, BufferRange::AllRange());
    graph.Link(passA, target, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderWrite, rangeA);
    graph.Link(seedB, passB, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderRead, BufferRange::AllRange());
    graph.Link(passB, target, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderWrite, rangeB);
    ExpectValidateFailContains(graph, "unordered overlapping writes");
}

TEST(RenderGraphValidateTest, Requirement_11_2_InternalReadMustHaveEarlierWriter) {
    const auto allBuffer = BufferRange::AllRange();
    RenderGraph graph{};
    const auto seed = graph.ImportBuffer(MakeValidGpuBufferHandle(503), RDGExecutionStage::Host, RDGMemoryAccess::HostWrite, allBuffer, "seed");
    const auto target = graph.AddBuffer(64, MemoryType::Device, BufferUse::Resource, "target");
    const auto output = graph.AddBuffer(64, MemoryType::Device, BufferUse::Resource | BufferUse::UnorderedAccess, "output");
    const auto pass = AddEmptyComputePass(graph, "read-pass");
    graph.Link(seed, pass, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderRead, allBuffer);
    graph.Link(target, pass, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderRead, BufferRange{0, 32});
    graph.Link(pass, output, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderWrite, allBuffer);
    ExpectValidateFailContains(graph, "without an earlier write source");
}

TEST(RenderGraphValidateTest, Requirement_11_2_ReadsMustNotRaceLaterWrites) {
    const auto range = BufferRange{0, 32};
    RenderGraph graph{};
    const auto seedInit = graph.ImportBuffer(MakeValidGpuBufferHandle(504), RDGExecutionStage::Host, RDGMemoryAccess::HostWrite, BufferRange::AllRange(), "seed-init");
    const auto seedLate = graph.ImportBuffer(MakeValidGpuBufferHandle(505), RDGExecutionStage::Host, RDGMemoryAccess::HostWrite, BufferRange::AllRange(), "seed-late");
    const auto seedRead = graph.ImportBuffer(MakeValidGpuBufferHandle(506), RDGExecutionStage::Host, RDGMemoryAccess::HostWrite, BufferRange::AllRange(), "seed-read");
    const auto target = graph.AddBuffer(64, MemoryType::Device, BufferUse::Resource | BufferUse::UnorderedAccess, "target");
    const auto output = graph.AddBuffer(64, MemoryType::Device, BufferUse::Resource | BufferUse::UnorderedAccess, "output");
    const auto initPass = AddEmptyComputePass(graph, "init-pass");
    graph.Link(seedInit, initPass, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderRead, BufferRange::AllRange());
    graph.Link(initPass, target, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderWrite, range);

    const auto latePass = AddEmptyComputePass(graph, "late-pass");
    graph.Link(seedLate, latePass, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderRead, BufferRange::AllRange());
    graph.Link(latePass, target, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderWrite, range);
    graph.AddPassDependency(initPass, latePass);

    const auto readPass = AddEmptyComputePass(graph, "read-pass");
    graph.Link(seedRead, readPass, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderRead, BufferRange::AllRange());
    graph.Link(target, readPass, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderRead, range);
    graph.Link(readPass, output, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderWrite, BufferRange::AllRange());
    ExpectValidateFailContains(graph, "is read by");
    ExpectValidateFailContains(graph, "before write from");
}

TEST(RenderGraphValidateTest, Requirement_11_3_WritesMustNotRaceEarlierReads) {
    const auto range = BufferRange{0, 32};
    RenderGraph graph{};
    const auto seedInit = graph.ImportBuffer(MakeValidGpuBufferHandle(507), RDGExecutionStage::Host, RDGMemoryAccess::HostWrite, BufferRange::AllRange(), "seed-init");
    const auto seedRead = graph.ImportBuffer(MakeValidGpuBufferHandle(508), RDGExecutionStage::Host, RDGMemoryAccess::HostWrite, BufferRange::AllRange(), "seed-read");
    const auto seedLate = graph.ImportBuffer(MakeValidGpuBufferHandle(509), RDGExecutionStage::Host, RDGMemoryAccess::HostWrite, BufferRange::AllRange(), "seed-late");
    const auto target = graph.AddBuffer(64, MemoryType::Device, BufferUse::Resource | BufferUse::UnorderedAccess, "target");
    const auto output = graph.AddBuffer(64, MemoryType::Device, BufferUse::Resource | BufferUse::UnorderedAccess, "output");
    const auto initPass = AddEmptyComputePass(graph, "init-pass");
    graph.Link(seedInit, initPass, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderRead, BufferRange::AllRange());
    graph.Link(initPass, target, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderWrite, range);

    const auto readPass = AddEmptyComputePass(graph, "read-pass");
    graph.Link(seedRead, readPass, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderRead, BufferRange::AllRange());
    graph.Link(target, readPass, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderRead, range);
    graph.Link(readPass, output, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderWrite, BufferRange::AllRange());

    const auto latePass = AddEmptyComputePass(graph, "late-pass");
    graph.Link(seedLate, latePass, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderRead, BufferRange::AllRange());
    graph.Link(latePass, target, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderWrite, range);
    graph.AddPassDependency(initPass, latePass);
    ExpectValidateFailContains(graph, "is overwritten by");
    ExpectValidateFailContains(graph, "before read on");
}

TEST(RenderGraphValidateTest, Requirement_11_NonOverlappingWritesAreAllowed) {
    RenderGraph graph{};
    const auto seedA = graph.ImportBuffer(MakeValidGpuBufferHandle(510), RDGExecutionStage::Host, RDGMemoryAccess::HostWrite, BufferRange::AllRange(), "seed-a");
    const auto seedB = graph.ImportBuffer(MakeValidGpuBufferHandle(511), RDGExecutionStage::Host, RDGMemoryAccess::HostWrite, BufferRange::AllRange(), "seed-b");
    const auto target = graph.AddBuffer(64, MemoryType::Device, BufferUse::Resource | BufferUse::UnorderedAccess, "target");
    const auto passA = AddEmptyComputePass(graph, "write-a");
    const auto passB = AddEmptyComputePass(graph, "write-b");
    graph.Link(seedA, passA, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderRead, BufferRange::AllRange());
    graph.Link(passA, target, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderWrite, BufferRange{0, 16});
    graph.Link(seedB, passB, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderRead, BufferRange::AllRange());
    graph.Link(passB, target, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderWrite, BufferRange{32, 16});
    ExpectValidateOk(graph);
}

TEST(RenderGraphValidateTest, Requirement_12_1_RedundantPassDependencyShouldBeReported) {
    const auto allBuffer = BufferRange::AllRange();
    RenderGraph graph{};
    const auto seed = graph.ImportBuffer(MakeValidGpuBufferHandle(601), RDGExecutionStage::Host, RDGMemoryAccess::HostWrite, allBuffer, "seed");
    const auto middle = graph.AddBuffer(64, MemoryType::Device, BufferUse::Resource | BufferUse::UnorderedAccess, "middle");
    const auto output = graph.AddBuffer(64, MemoryType::Device, BufferUse::Resource | BufferUse::UnorderedAccess, "output");
    const auto writer = AddEmptyComputePass(graph, "writer");
    const auto reader = AddEmptyComputePass(graph, "reader");
    graph.Link(seed, writer, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderRead, allBuffer);
    graph.Link(writer, middle, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderWrite, allBuffer);
    graph.Link(middle, reader, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderRead, allBuffer);
    graph.Link(reader, output, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderWrite, allBuffer);
    graph.AddPassDependency(writer, reader);

    const auto result = graph.Validate();
    EXPECT_TRUE(result.IsValid) << result.Message;
    EXPECT_FALSE(result.Message.empty()) << "redundant pass dependency should be reported";
}

TEST(RenderGraphValidateTest, Requirement_12_2_PassDependencyMustNotContradictResourceOrder) {
    const auto allBuffer = BufferRange::AllRange();
    RenderGraph graph{};
    const auto seed = graph.ImportBuffer(MakeValidGpuBufferHandle(602), RDGExecutionStage::Host, RDGMemoryAccess::HostWrite, allBuffer, "seed");
    const auto middle = graph.AddBuffer(64, MemoryType::Device, BufferUse::Resource | BufferUse::UnorderedAccess, "middle");
    const auto output = graph.AddBuffer(64, MemoryType::Device, BufferUse::Resource | BufferUse::UnorderedAccess, "output");
    const auto writer = AddEmptyComputePass(graph, "writer");
    const auto reader = AddEmptyComputePass(graph, "reader");
    graph.Link(seed, writer, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderRead, allBuffer);
    graph.Link(writer, middle, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderWrite, allBuffer);
    graph.Link(middle, reader, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderRead, allBuffer);
    graph.Link(reader, output, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderWrite, allBuffer);
    graph.AddPassDependency(reader, writer);
    ExpectValidateFailContainsOneOf(graph, {"contradicts resource-derived order", "graph contains a cycle"});
}

TEST(RenderGraphValidateTest, Requirement_12_3_PassDependencyParticipatesInCycleDetection) {
    RenderGraph graph{};
    const auto passA = graph.AddCopyPass("pass-a");
    const auto passB = graph.AddCopyPass("pass-b");
    graph.AddPassDependency(passA, passB);
    graph.AddPassDependency(passB, passA);
    ExpectValidateFailContains(graph, "graph contains a cycle");
}

}  // namespace
