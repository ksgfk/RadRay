#include <gtest/gtest.h>

#include <radray/runtime/render_graph.h>

using namespace radray;
using namespace radray::runtime;

TEST(RenderGraphTest, CompileSortsPassesAndTracksPresentState) {
    RenderGraph graph{};

    render::TextureDescriptor desc{};
    desc.Dim = render::TextureDimension::Dim2D;
    desc.Width = 32;
    desc.Height = 32;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleCount = 1;
    desc.Format = render::TextureFormat::RGBA8_UNORM;
    desc.Usage = render::TextureUse::RenderTarget | render::TextureUse::Resource;
    const RgResourceHandle backBuffer = graph.ImportTexture(
        "BackBuffer",
        ImportedTextureDesc{
            .Texture = nullptr,
            .DefaultView = nullptr,
            .Desc = desc,
            .InitialState = render::TextureState::Present,
        });

    graph.AddCopyPass("Upload", [](PassBuilder& builder) {
        builder.SetExecute([](RenderGraphPassContext&, Nullable<string*>) { return true; });
    });
    graph.AddRasterPass("Main", [backBuffer](PassBuilder& builder) {
        builder.SetColorAttachment(backBuffer);
        builder.SetExecute([](RenderGraphPassContext&, Nullable<string*>) { return true; });
    });
    graph.AddCopyPass("Present", [backBuffer](PassBuilder& builder) {
        builder.SetPresentTarget(backBuffer);
        builder.SetExecute([](RenderGraphPassContext&, Nullable<string*>) { return true; });
    });

    string reason{};
    ASSERT_TRUE(graph.Compile(&reason)) << reason;
    ASSERT_EQ(graph.GetCompiledPassOrder().size(), 3u);
    EXPECT_EQ(graph.GetCompiledPassOrder()[0].Id, 1u);
    EXPECT_EQ(graph.GetCompiledPassOrder()[1].Id, 2u);
    EXPECT_EQ(graph.GetCompiledPassOrder()[2].Id, 3u);
    ASSERT_TRUE(graph.GetCompiledFinalTextureState(backBuffer).has_value());
    EXPECT_EQ(graph.GetCompiledFinalTextureState(backBuffer).value(), render::TextureState::Present);
}

TEST(RenderGraphTest, CompileRejectsTransientReadBeforeWrite) {
    RenderGraph graph{};

    render::TextureDescriptor desc{};
    desc.Dim = render::TextureDimension::Dim2D;
    desc.Width = 8;
    desc.Height = 8;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleCount = 1;
    desc.Format = render::TextureFormat::RGBA8_UNORM;
    desc.Usage = render::TextureUse::RenderTarget | render::TextureUse::Resource;
    const RgResourceHandle transient = graph.CreateTransientTexture("Transient", desc);

    graph.AddRasterPass("Reader", [transient](PassBuilder& builder) {
        builder.ReadTexture(transient);
        builder.SetExecute([](RenderGraphPassContext&, Nullable<string*>) { return true; });
    });

    string reason{};
    EXPECT_FALSE(graph.Compile(&reason));
    EXPECT_FALSE(reason.empty());
}

TEST(RenderGraphTest, CompileTracksBufferDependenciesAndFinalState) {
    RenderGraph graph{};

    render::BufferDescriptor desc{};
    desc.Size = 256;
    desc.Memory = render::MemoryType::Device;
    desc.Usage = render::BufferUse::CopySource | render::BufferUse::CopyDestination;
    const RgResourceHandle transient = graph.CreateTransientBuffer("TransientBuffer", desc);

    graph.AddCopyPass("Writer", [transient](PassBuilder& builder) {
        builder.WriteBuffer(transient);
        builder.SetExecute([](RenderGraphPassContext&, Nullable<string*>) { return true; });
    });
    graph.AddCopyPass("Reader", [transient](PassBuilder& builder) {
        builder.ReadBuffer(transient);
        builder.SetExecute([](RenderGraphPassContext&, Nullable<string*>) { return true; });
    });

    string reason{};
    ASSERT_TRUE(graph.Compile(&reason)) << reason;
    ASSERT_EQ(graph.GetCompiledPassOrder().size(), 2u);
    EXPECT_EQ(graph.GetCompiledPassOrder()[0].Id, 1u);
    EXPECT_EQ(graph.GetCompiledPassOrder()[1].Id, 2u);
    ASSERT_TRUE(graph.GetCompiledFinalBufferState(transient).has_value());
    EXPECT_EQ(graph.GetCompiledFinalBufferState(transient).value(), render::BufferState::CopySource);
}

TEST(RenderGraphTest, CompileRejectsTransientBufferReadBeforeWrite) {
    RenderGraph graph{};

    render::BufferDescriptor desc{};
    desc.Size = 128;
    desc.Memory = render::MemoryType::Device;
    desc.Usage = render::BufferUse::CopySource | render::BufferUse::CopyDestination;
    const RgResourceHandle transient = graph.CreateTransientBuffer("TransientBuffer", desc);

    graph.AddCopyPass("Reader", [transient](PassBuilder& builder) {
        builder.ReadBuffer(transient);
        builder.SetExecute([](RenderGraphPassContext&, Nullable<string*>) { return true; });
    });

    string reason{};
    EXPECT_FALSE(graph.Compile(&reason));
    EXPECT_FALSE(reason.empty());
}
