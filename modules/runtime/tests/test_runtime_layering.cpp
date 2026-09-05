#include <gtest/gtest.h>

#include <filesystem>
#include <regex>
#include <type_traits>

#include <radray/file.h>
#include "forward_pipeline/forward_frame.h"
#include <radray/runtime/render_framework/render_pipeline.h>

namespace radray {
namespace {

const std::filesystem::path kRoot{RADRAY_PROJECT_DIR};

string ReadSource(const std::filesystem::path& path) {
    auto text = ReadTextFile(kRoot / path);
    EXPECT_TRUE(text.has_value()) << path.string();
    return text.value_or(string{});
}

TEST(RuntimeLayering, LegacyPipelineScaffoldingRemoved) {
    for (const auto root : {"modules/runtime/src", "modules/runtime/include", "examples"}) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator{kRoot / root}) {
            if (entry.path().extension() != ".h" && entry.path().extension() != ".cpp") {
                continue;
            }
            const string source = ReadSource(entry.path());
            for (const auto symbol : {"BindingGroupPlan", "RenderPassEvent", "RenderPipelinePass", "RenderCameraList",
                                      "PrepareParameterSet", "GetResidentParameterSet", "OnRenderView", "RenderViewContent", "RenderPipelineTarget", "MeshDrawList", "MeshDrawItem", "ForwardFrameDraw"}) {
                EXPECT_EQ(source.find(symbol), string::npos) << entry.path().string() << ": " << symbol;
            }
        }
    }
}

TEST(RuntimeLayering, GenericPipelineDoesNotDependOnForward) {
    vector<std::filesystem::path> paths{
        "modules/runtime/include/radray/runtime/render_system.h", "modules/runtime/src/render_system.cpp",
        "modules/runtime/include/radray/runtime/application.h", "modules/runtime/src/application.cpp"};
    for (const auto root : {"modules/runtime/include/radray/runtime/render_framework", "modules/runtime/src/render_framework"}) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator{kRoot / root}) {
            if (entry.is_regular_file()) {
                paths.push_back(entry.path());
            }
        }
    }
    for (const auto& path : paths) {
        const auto source = ReadSource(path);
        for (const auto symbol : {"forward_pipeline/", "ForwardPipeline", "ForwardDrawPass", "BeforeRenderingOpaques", "BeforeRenderingTransparents"}) {
            EXPECT_EQ(source.find(symbol), string::npos) << path.string() << ": " << symbol;
        }
    }
}

TEST(RuntimeLayering, ForwardSpecificDrawPolicyDoesNotLeakFurther) {
    for (const auto path : {"modules/runtime/include/radray/runtime/render_framework/render_pipeline.h",
                            "modules/runtime/src/render_framework/render_pipeline.cpp",
                            "modules/runtime/include/radray/runtime/render_system.h", "modules/runtime/src/render_system.cpp"}) {
        const auto source = ReadSource(path);
        for (const auto symbol : {"RenderQueue", "opaque", "transparent"}) {
            EXPECT_EQ(source.find(symbol), string::npos) << path << ": " << symbol;
        }
    }
    const auto recording = ReadSource("modules/runtime/src/forward_pipeline/forward_pipeline.cpp");
    for (const auto symbol : {"->Primitives(", "->Lights(", "->GetMaterial(", "->GetParameterStorage(",
                              "->ComputeViewMatrix(", "->GetEyePosition(", ".AsAny(", "AssetManager", "DepthTargets", "ResourceBarrier(", "GetCommandBuffer(", "AppWindow"}) {
        EXPECT_EQ(recording.find(symbol), string::npos) << symbol;
    }
    const auto material = ReadSource("modules/runtime/src/material.cpp");
    for (const auto symbol : {"ShaderParameterSet", "GetOrCreateSrv", "FlightIndex", "flightCount"}) {
        EXPECT_EQ(material.find(symbol), string::npos) << symbol;
    }
}

template <class T>
concept ExposesCommandBuffer = requires(T& value) { value.GetCommandBuffer(); };
template <class T>
concept ExposesFrame = requires(T& value) { value.Frame; };
template <class T>
concept ExposesCommands = requires(T& value) { value.Commands(); };

TEST(RuntimeLayering, PipelineAndPassContextsHaveNoRawCommandEscape) {
    static_assert(!ExposesFrame<RenderPipelineContext>);
    static_assert(!ExposesCommands<RenderPipelineContext>);
    static_assert(!ExposesCommandBuffer<RenderGraphGraphicsCommands>);
    static_assert(!ExposesCommandBuffer<RenderGraphComputeCommands>);
    static_assert(!std::is_convertible_v<RenderGraphGraphicsCommands&, render::GraphicsCommandEncoder&>);
    static_assert(!std::is_convertible_v<RenderGraphComputeCommands&, render::ComputeCommandEncoder&>);
}

string StructBody(const string& source, std::string_view name) {
    const size_t start = source.find(fmt::format("struct {} {{", name));
    EXPECT_NE(start, string::npos) << name;
    if (start == string::npos) {
        return {};
    }
    const size_t brace = source.find('{', start);
    int depth = 0;
    for (size_t index = brace; index < source.size(); ++index) {
        if (source[index] == '{') {
            ++depth;
        }
        if (source[index] == '}' && --depth == 0) {
            return source.substr(brace, index - brace + 1);
        }
    }
    ADD_FAILURE() << "Unclosed struct " << name;
    return {};
}

TEST(RuntimeLayering, SnapshotContainsNoGameThreadObjectsOrAssetRefs) {
    static_assert(std::is_same_v<decltype(MeshBatch::Geometry), Nullable<const GpuMesh::DrawData*>>);
    static_assert(std::is_same_v<decltype(MaterialTextureFrameData::Texture), TextureAsset*>);
    static_assert(std::is_same_v<decltype(MaterialPassRenderData::Program), Nullable<ShaderProgram*>>);
    const auto frame = ReadSource("modules/runtime/include/radray/runtime/render_framework/render_scene_snapshot.h");
    const auto batch = ReadSource("modules/runtime/include/radray/runtime/render_framework/mesh_batch.h");
    const auto material = ReadSource("modules/runtime/include/radray/runtime/material.h");
    const auto command = ReadSource("modules/runtime/include/radray/runtime/render_framework/mesh_draw_command.h");
    string bodies;
    for (const auto name : {"RenderPrimitiveData", "RenderLightData", "RenderSceneSnapshot"}) bodies += StructBody(frame, name);
    bodies += StructBody(batch, "MeshBatch");
    bodies += StructBody(command, "MeshDrawCommand");
    for (const auto name : {"MaterialRenderData", "MaterialPassRenderData", "MaterialTextureFrameData", "MaterialSamplerFrameData"}) bodies += StructBody(material, name);
    for (const auto symbol : {"Scene", "PrimitiveSceneProxy", "LightSceneProxy", "CameraComponent", "Material", "MaterialTechnique", "StreamingAssetRef", "StreamingAssetRefAny", "AssetManager"}) {
        const std::regex pattern{fmt::format("\\b{}\\b{}", symbol, std::string_view{symbol} == "Material" ? "\\s*[*&>]" : "")};
        EXPECT_FALSE(std::regex_search(bodies.begin(), bodies.end(), pattern)) << symbol;
    }
}

}  // namespace
}  // namespace radray
