#include <radray/runtime/graph.h>
#include <radray/logger.h>
#include <algorithm>
#include <iostream>
#include <gtest/gtest.h>
namespace radray::test {
using namespace radray::render;
namespace {
struct LogCapture {
    vector<string> errors;
    LogCapture() {
        SetLogCallback([](LogLevel level, std::string_view message, void* data) {
            if (level >= LogLevel::Err) static_cast<LogCapture*>(data)->errors.emplace_back(message);
        },
                       this);
    }
    ~LogCapture() { ClearLogCallback(); }
};
void Check(bool condition, const char* message) { EXPECT_TRUE(condition) << message; }
CompiledGraph Compile(GraphBuilder& g) {
    CompiledGraph c;
    EXPECT_TRUE(g.Compile(c));
    return c;
}
void Reject(GraphBuilder& g, const char* part) {
    CompiledGraph c;
    LogCapture logs;
    Check(!g.Compile(c), "Invalid graph compiled");
    ASSERT_EQ(logs.errors.size(), 1u);
    EXPECT_NE(logs.errors[0].find(part), string::npos) << logs.errors[0];
}
BufferDescriptor BufferDesc() { return {64, MemoryType::Device, BufferUse::CopySource | BufferUse::CopyDestination | BufferUse::Resource | BufferUse::UnorderedAccess}; }
TextureDescriptor TextureDesc() { return {TextureDimension::Dim2D, 16, 16, 1, 1, 1, TextureFormat::RGBA8_UNORM, MemoryType::Device, TextureUse::RenderTarget | TextureUse::Resource | TextureUse::CopySource}; }
void Dependencies() {
    GraphBuilder g;
    auto a = g.CreateBuffer("a", BufferDesc()), b = g.CreateBuffer("b", BufferDesc());
    g.AddComputePass("write", {{a, Access::StorageWrite}}, {});
    g.AddComputePass("read", {{a, Access::StorageRead}}, {});
    g.AddComputePass("overwrite", {{a, Access::StorageWrite}}, {});
    g.AddComputePass("unused", {{b, Access::StorageWrite}}, {});
    g.AddComputePass("side effect", {}, {}, true);
    g.MarkOutput(a);
    auto c = Compile(g);
    Check(c.plan.size() == 4, "dead pass was retained or side effect culled");
    Check(c.plan[1].dependencies == vector<uint32_t>{0}, "RAW missing");
    Check(c.plan[2].dependencies == vector<uint32_t>({0, 1}), "WAR/WAW missing");
    Check(c.plan[1].barriers.size() == 1 && c.plan[1].barriers[0].uav, "same-state UAV write/read barrier missing");
    Check(c.plan[2].barriers.size() == 1 && c.plan[2].barriers[0].uav, "same-state UAV read/write barrier missing");
    Check(c.lifetimes[a.index].first == 0 && c.lifetimes[a.index].last == 2 && c.lifetimes[b.index].first == -1, "live intervals incorrect");
    Check(c.Dump().find("UAV") != string::npos, "diagnostic barrier missing");
}
void Validation() {
    {
        GraphBuilder g;
        g.CreateBuffer("buffer", BufferDesc());
        Check(!g.Find(nullptr), "Null lookup matched a resource");
    }
    {
        GraphBuilder g;
        auto a = g.CreateBuffer("uninitialized", BufferDesc());
        g.AddComputePass("read", {{a, Access::StorageRead}}, {}, true);
        Reject(g, "Uninitialized read");
    }
    {
        GraphBuilder other, g;
        auto a = other.CreateBuffer("foreign", BufferDesc());
        g.AddComputePass("foreign", {{a, Access::StorageWrite}}, {}, true);
        Reject(g, "graph handle");
    }
    {
        GraphBuilder g;
        auto a = g.CreateBuffer("wrong usage", {16, MemoryType::Device, BufferUse::Vertex});
        g.AddComputePass("write", {{a, Access::StorageWrite}}, {}, true);
        Reject(g, "incompatible");
    }
    {
        GraphBuilder g;
        auto a = g.CreateBuffer("buffer", BufferDesc());
        g.AddCopyPass("bad copy", {{a, Access::StorageWrite}}, {}, true);
        Reject(g, "Non-copy");
    }
    {
        GraphBuilder g;
        auto a = g.CreateBuffer("buffer", BufferDesc());
        g.AddComputePass("bad compute", {{a, Access::CopyDestination}}, {}, true);
        Reject(g, "Copy access");
    }
    {
        GraphBuilder g;
        auto a = g.CreateBuffer("buffer", BufferDesc());
        g.AddComputePass("feedback", {{a, Access::StorageWrite}, {a, Access::Sample}}, {}, true);
        Reject(g, "Conflicting");
    }
    {
        GraphBuilder g;
        auto a = g.CreateBuffer("buffer", BufferDesc());
        g.AddComputePass("stage", {{a, Access::StorageWrite, ShaderStage::Pixel}}, {}, true);
        Reject(g, "Shader stage");
    }
    {
        GraphBuilder g;
        auto a = g.CreateTexture("color", TextureDesc());
        g.AddRasterPass("bad load", {}, {{{a, LoadAction::Load}}, {}, 16, 16}, {});
        Reject(g, "uninitialized Load");
    }
    {
        GraphBuilder g;
        auto a = g.CreateTexture("color", TextureDesc());
        g.AddRasterPass("size", {}, {{{a}}, {}, 8, 16}, {});
        Reject(g, "attachment");
    }
    {
        GraphBuilder g;
        auto a = g.CreateTexture("color", TextureDesc());
        g.AddRasterPass("depth", {}, {{}, DepthAttachment{a}, 16, 16}, {});
        Reject(g, "incompatible");
    }
    {
        GraphBuilder g;
        auto a = g.CreateTexture("color", TextureDesc());
        g.AddRasterPass("duplicate", {}, {{{a}, {a}}, {}, 16, 16}, {});
        Reject(g, "Conflicting");
    }
    {
        GraphBuilder g;
        auto a = g.CreateTexture("color", TextureDesc()), b = g.CreateTexture("unbound", TextureDesc());
        g.AddRasterPass("unbound", {{b, Access::Color}}, {{{a}}, {}, 16, 16}, {});
        Reject(g, "no attachment");
    }
    {
        GraphBuilder g;
        auto desc = TextureDesc();
        desc.Dim = TextureDimension::Dim3D;
        auto a = g.CreateTexture("volume", desc);
        g.AddRasterPass("volume attachment", {}, {{{a}}, {}, 16, 16}, {});
        Reject(g, "attachment");
    }
    {
        GraphBuilder g;
        auto a = g.CreateTexture("discarded", TextureDesc());
        g.AddRasterPass("discard", {}, {{{a, LoadAction::Clear, StoreAction::Discard}}, {}, 16, 16}, {});
        g.MarkOutput(a);
        Reject(g, "Uninitialized output");
    }
}
void AttachmentsAndExtraction() {
    GraphBuilder g;
    auto a = g.CreateTexture("image", TextureDesc());
    auto b = g.CreateBuffer("result", BufferDesc());
    g.AddRasterPass("clear", {}, {{{a}}, {}, 16, 16}, {});
    g.AddRasterPass("load", {}, {{{a, LoadAction::Load}}, {}, 16, 16}, {});
    g.AddComputePass("sample", {{a, Access::Sample}, {b, Access::StorageWrite}}, {});
    g.Extract(b);
    auto c = Compile(g);
    Check(c.plan.size() == 3 && c.resources[b.index].extracted, "extraction root missing");
    Check(c.plan[2].barriers[0].after.texture == TextureState::ShaderRead, "raster-to-sample transition missing");
    Check(c.plan[2].barriers[0].after.stages == ShaderStage::Compute, "compute stage inference missing");
}
}  // namespace
TEST(RuntimeGraph, DependenciesAndCulling) { Dependencies(); }
TEST(RuntimeGraph, Validation) { Validation(); }
TEST(RuntimeGraph, ConstructionErrorsAreLoggedImmediately) {
    LogCapture logs;
    CompiledGraph compiled;
    GraphBuilder texture;
    texture.ImportTexture("null", nullptr, {});
    ASSERT_EQ(logs.errors.size(), 1u);
    EXPECT_EQ(logs.errors.back(), "Cannot import a null texture");
    EXPECT_FALSE(texture.Compile(compiled));
    EXPECT_FALSE(texture.Compile(compiled));
    EXPECT_EQ(logs.errors.size(), 1u);

    GraphBuilder buffer;
    buffer.ImportBuffer("null", nullptr, {});
    ASSERT_EQ(logs.errors.size(), 2u);
    EXPECT_EQ(logs.errors.back(), "Cannot import a null buffer");
    EXPECT_FALSE(buffer.Compile(compiled));
    EXPECT_EQ(logs.errors.size(), 2u);

    GraphBuilder output, other;
    output.MarkOutput(other.CreateBuffer("foreign", BufferDesc()));
    ASSERT_EQ(logs.errors.size(), 3u);
    EXPECT_EQ(logs.errors.back(), "Output has a foreign or invalid graph handle");
    EXPECT_FALSE(output.Compile(compiled));
    EXPECT_EQ(logs.errors.size(), 3u);
}
TEST(RuntimeGraph, AttachmentsAndExtraction) { AttachmentsAndExtraction(); }

}  // namespace radray::test
