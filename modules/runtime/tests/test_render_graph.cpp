#include <string_view>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <radray/render/common.h>
#include <radray/runtime/render_graph.h>

using namespace radray;
using namespace radray::render;

namespace {

void ExpectContains(std::string_view text, std::string_view needle) {
    EXPECT_NE(text.find(needle), std::string_view::npos) << "missing substring: " << needle;
}

TEST(RenderGraphTest, ExportGraphvizIncludesNodeFieldsAndNormalEdges) {
    RenderGraph graph{};

    const auto input = graph.AddBuffer(64, MemoryType::Device, BufferUse::CBuffer, "input-buffer");
    const auto output = graph.AddBuffer(128, MemoryType::Device, BufferUse::Resource | BufferUse::UnorderedAccess, "output-buffer");
    const auto pass = graph.AddCopyPass("copy-pass");

    graph.Link(
        input,
        pass,
        RDGExecutionStage::VertexShader | RDGExecutionStage::PixelShader,
        RDGMemoryAccess::ConstantRead,
        BufferRange::AllRange());
    graph.Link(
        pass,
        output,
        RDGExecutionStage::VertexShader | RDGExecutionStage::PixelShader,
        RDGMemoryAccess::ShaderRead | RDGMemoryAccess::ShaderWrite,
        BufferRange::AllRange());

    const auto dot = graph.ExportGraphviz();

    ExpectContains(dot, "digraph RenderGraph {");
    ExpectContains(dot, "n0 [label=\"id: 0\\nname: input-buffer\\nkind: Resource\\nownership: Internal\\ntag: [Buffer]\"]");
    ExpectContains(dot, "n1 [label=\"id: 1\\nname: output-buffer\\nkind: Resource\\nownership: Internal\\ntag: [Buffer]\"]");
    ExpectContains(dot, "n2 [label=\"id: 2\\nname: copy-pass\\nkind: Pass\\ntag: [Copy]\"]");
    ExpectContains(dot, "n0 -> n2 [label=\"stage: [VertexShader | PixelShader]\\naccess: [ConstantRead]\"]");
    ExpectContains(dot, "n2 -> n1 [label=\"stage: [VertexShader | PixelShader]\\naccess: [ShaderRead | ShaderWrite]\"]");
}

TEST(RenderGraphTest, ExportGraphvizUsesEnumFlagsFormattingForCombinedEdgeState) {
    RenderGraph graph{};

    const auto input = graph.AddBuffer(64, MemoryType::Device, BufferUse::CBuffer, "buffer-a");
    const auto pass = graph.AddCopyPass("copy-pass");

    graph.Link(
        input,
        pass,
        RDGExecutionStage::VertexShader | RDGExecutionStage::PixelShader,
        RDGMemoryAccess::ShaderRead | RDGMemoryAccess::ShaderWrite,
        BufferRange::AllRange());

    const auto dot = graph.ExportGraphviz();

    ExpectContains(dot, "stage: [VertexShader | PixelShader]");
    ExpectContains(dot, "access: [ShaderRead | ShaderWrite]");
}

TEST(RenderGraphTest, ExportGraphvizIncludesImportAndExportPseudoNodes) {
    RenderGraph graph{};

    const auto imported = graph.ImportBuffer(
        GpuBufferHandle{},
        RDGExecutionStage::Host,
        RDGMemoryAccess::HostWrite,
        BufferRange::AllRange(),
        "external-buffer");
    graph.ExportBuffer(imported, RDGExecutionStage::Host, RDGMemoryAccess::HostRead, BufferRange::AllRange());

    const auto dot = graph.ExportGraphviz();

    ExpectContains(dot, "n0 [label=\"id: 0\\nname: external-buffer\\nkind: Resource\\nownership: External\\ntag: [Buffer]\"]");
    ExpectContains(dot, "n0_import [shape=oval, style=dashed, label=\"kind: Import\\nownership: External\"]");
    ExpectContains(dot, "n0_export [shape=oval, style=dashed, label=\"kind: Export\\nownership: External\"]");
    ExpectContains(dot, "n0_import -> n0 [label=\"stage: [Host]\\naccess: [HostWrite]\"]");
    ExpectContains(dot, "n0 -> n0_export [label=\"stage: [Host]\\naccess: [HostRead]\"]");
}

TEST(RenderGraphTest, ExportGraphvizEscapesNodeNames) {
    RenderGraph graph{};

    graph.AddBuffer(16, MemoryType::Device, BufferUse::Common, "buffer \"quoted\"\nnext");

    const auto dot = graph.ExportGraphviz();

    ExpectContains(dot, "name: buffer \\\"quoted\\\"\\nnext");
}

}  // namespace
