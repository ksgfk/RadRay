#pragma once

#include <radray/vertex_data.h>
#include <radray/render/common.h>
#include <radray/render/dxc.h>
#include <radray/render/bind_bridge.h>

namespace radray::render {

struct SemanticMapping {
    std::string_view Semantic{};
    uint32_t SemanticIndex{0};
    uint32_t Location{0};
    VertexFormat Format{VertexFormat::UNKNOWN};
};
std::optional<vector<VertexElement>> MapVertexElements(std::span<const VertexBufferEntry> layouts, std::span<const SemanticMapping> semantics) noexcept;

Nullable<unique_ptr<RootSignature>> CreateSerializedRootSignature(Device* device, std::span<const byte> data) noexcept;

#if defined(RADRAY_ENABLE_SPIRV_CROSS) && defined(RADRAY_ENABLE_SPIRV_CROSS)
struct ShaderCompileResult {
    unique_ptr<Shader> VSResult;
    unique_ptr<Shader> PSResult;
    render::BindBridgeLayout BindLayout;
};
std::optional<ShaderCompileResult> CompileShaderFromHLSL(
    Dxc* dxc,
    Device* device,
    std::string_view hlsl,
    RenderBackend backend,
    const vector<BindBridgeStaticSampler> staticSamplers = {},
    std::string_view entryVS = "VSMain",
    std::string_view entryPS = "PSMain",
    render::HlslShaderModel shaderModel = render::HlslShaderModel::SM60,
    const vector<string>& defines = {},
    const vector<string>& includes = {}) noexcept;

struct ComputeShaderCompileResult {
    unique_ptr<Shader> CSResult;
    render::BindBridgeLayout BindLayout;
};
std::optional<ComputeShaderCompileResult> CompileComputeShaderFromHLSL(
    Dxc* dxc,
    Device* device,
    std::string_view hlsl,
    RenderBackend backend,
    std::string_view entryCS = "CSMain",
    render::HlslShaderModel shaderModel = render::HlslShaderModel::SM60,
    const vector<string>& defines = {},
    const vector<string>& includes = {}) noexcept;
#endif

}  // namespace radray::render
