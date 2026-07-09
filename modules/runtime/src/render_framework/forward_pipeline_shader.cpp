#include <radray/runtime/render_framework/forward_pipeline_shader.h>

#include <filesystem>

#include <radray/file.h>
#include <radray/logger.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/render_system.h>

namespace radray {

namespace {

// 追加交错顶点布局 (POSITION/NORMAL/TEXCOORD/TANGENT, 单 buffer) 到 pass。
// 统一 shader 始终消费 TANGENT (程序化几何与 glTF 网格都会生成切线)。
void AppendVertexLayout(ShaderPassDesc& pass) {
    OwningVertexBufferLayout layout{};
    layout.ArrayStride = forward_pipeline::kVertexStride;
    layout.StepMode = render::VertexStepMode::Vertex;
    layout.Elements.push_back(render::VertexElement{
        .Offset = 0,
        .Semantic = "POSITION",
        .SemanticIndex = 0,
        .Format = render::VertexFormat::FLOAT32X3,
        .Location = 0});
    layout.Elements.push_back(render::VertexElement{
        .Offset = sizeof(float) * 3,
        .Semantic = "NORMAL",
        .SemanticIndex = 0,
        .Format = render::VertexFormat::FLOAT32X3,
        .Location = 1});
    layout.Elements.push_back(render::VertexElement{
        .Offset = sizeof(float) * 6,
        .Semantic = "TEXCOORD",
        .SemanticIndex = 0,
        .Format = render::VertexFormat::FLOAT32X2,
        .Location = 2});
    layout.Elements.push_back(render::VertexElement{
        .Offset = sizeof(float) * 8,
        .Semantic = "TANGENT",
        .SemanticIndex = 0,
        .Format = render::VertexFormat::FLOAT32X4,
        .Location = 3});
    pass.VertexLayouts.push_back(std::move(layout));
}

// 构造前向着色 pass 的【基线】固定状态 (不透明: 背面剔除、深度写开、无混合)。
// blend / depthWrite / cull 属 PSO 固定状态, keyword 变体无法控制; opaque / transparent / 双面
// 的差异不在此烘死, 而由材质经 MaterialRenderState 在 PSO 构建时覆盖 (对齐 Unity [_Prop])。
ShaderPassDesc MakeForwardPass(
    const string& source,
    std::string_view shaderRoot,
    render::TextureFormat colorFormat) {
    ShaderPassDesc pass{};
    pass.PassTag = string{forward_pipeline::kForwardPassTag};
    pass.Source = source;
    pass.ProgramName = string{forward_pipeline::kForwardProgramName};
    pass.VertexEntry = "VSMain";
    pass.PixelEntry = "PSMain";
    pass.Primitive = render::PrimitiveState::Default();
    pass.MultiSample = render::MultiSampleState::Default();

    render::DepthStencilState ds = render::DepthStencilState::Default();
    ds.Format = render::TextureFormat::D32_FLOAT;
    pass.DepthStencil = ds;
    pass.ColorTargets.push_back(render::ColorTargetState::Default(colorFormat));
    pass.IncludeDirs.push_back(string{shaderRoot});
    AppendVertexLayout(pass);
    return pass;
}

// 构造 ShadowCaster pass (depth-only): 用 shadow_pass.hlsl, 无 color target,
// 深度写 + 正常深度测试。顶点布局与 forward pass 一致。
ShaderPassDesc MakeShadowCasterPass(const string& source, std::string_view shaderRoot) {
    ShaderPassDesc pass{};
    pass.PassTag = string{forward_pipeline::kShadowPassTag};
    pass.Source = source;
    pass.ProgramName = string{forward_pipeline::kShadowProgramName};
    pass.VertexEntry = "VSMain";
    pass.PixelEntry = "PSMain";
    pass.Primitive = render::PrimitiveState::Default();
    pass.MultiSample = render::MultiSampleState::Default();

    render::DepthStencilState ds = render::DepthStencilState::Default();
    ds.Format = render::TextureFormat::D32_FLOAT;
    ds.DepthWriteEnable = true;
    pass.DepthStencil = ds;
    // depth-only: 无 color target。
    pass.IncludeDirs.push_back(string{shaderRoot});
    AppendVertexLayout(pass);
    return pass;
}

// 读一个 forward-pipeline shader 源文件 (位于 <shaderRoot>/forward_pipeline/<file>)。
std::optional<string> ReadForwardShaderSource(std::string_view shaderRoot, std::string_view file) {
    const std::filesystem::path shaderPath =
        std::filesystem::path{string{shaderRoot}} / "forward_pipeline" / string{file};
    std::optional<string> source = ReadTextFile(shaderPath);
    if (!source.has_value()) {
        RADRAY_ERR_LOG("forward_pipeline: cannot read shader {}", shaderPath.string());
    }
    return source;
}

}  // namespace

ShaderKeywordSet MakeForwardKeywordSet() {
    ShaderKeywordSet kw{};
    kw.Add(forward_pipeline::kKwBaseColorMap);
    kw.Add(forward_pipeline::kKwMetalRoughMap);
    kw.Add(forward_pipeline::kKwNormalMap);
    kw.Add(forward_pipeline::kKwOcclusionMap);
    kw.Add(forward_pipeline::kKwEmissiveMap);
    kw.Add(forward_pipeline::kKwAlphaTest);
    kw.Add(forward_pipeline::kKwAlphaBlend);
    kw.Add(forward_pipeline::kKwDoubleSided);
    kw.Add(forward_pipeline::kKwPointShadows);  // 管线级全局 keyword (有阴影点光源时启用)
    kw.Add(forward_pipeline::kKwDirectionalShadows);  // 管线级全局 keyword (有阴影方向光时启用)
    return kw;
}

std::optional<StreamingAssetRef<ShaderAsset>> BuildForwardShader(
    AssetManager& assets,
    RenderSystem& renderSystem,
    const ShaderKeywordSet& keywords,
    render::TextureFormat colorFormat,
    bool withShadowCaster) {
    const string shaderRoot = renderSystem.GetShaderIncludeRoot();

    std::optional<string> source = ReadForwardShaderSource(shaderRoot, forward_pipeline::kForwardPassFile);
    if (!source.has_value()) {
        return std::nullopt;
    }

    // opaque / transparent 共享同一 forward pass (基线固定状态); blend / zwrite / cull 差异由材质
    // 侧 MaterialRenderState 覆盖, 故只需一个 ShaderAsset。
    vector<ShaderPassDesc> passes;
    passes.push_back(MakeForwardPass(source.value(), shaderRoot, colorFormat));

    // shadow caster 深度 pass (depth-only, 所有投影材质共用同一份源)。
    if (withShadowCaster) {
        std::optional<string> shadowSource = ReadForwardShaderSource(shaderRoot, forward_pipeline::kShadowPassFile);
        if (shadowSource.has_value()) {
            passes.push_back(MakeShadowCasterPass(shadowSource.value(), shaderRoot));
        }
    }

    return assets.AddReady<ShaderAsset>(
        Guid::NewGuid(),
        make_unique<ShaderAsset>(keywords, std::move(passes)));
}

}  // namespace radray
