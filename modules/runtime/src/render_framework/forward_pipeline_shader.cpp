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

// 构造前向着色 pass。blend / depthWrite / cull 属 PSO 固定状态, keyword 变体无法控制,
// 故按 opaque / transparent 分别显式配置。
ShaderPassDesc MakeForwardPass(
    const string& source,
    std::string_view shaderRoot,
    render::TextureFormat colorFormat,
    bool transparent) {
    ShaderPassDesc pass{};
    pass.PassTag = string{forward_pipeline::kForwardPassTag};
    pass.Source = source;
    pass.VertexEntry = "VSMain";
    pass.PixelEntry = "PSMain";
    pass.Primitive = render::PrimitiveState::Default();
    pass.MultiSample = render::MultiSampleState::Default();

    render::DepthStencilState ds = render::DepthStencilState::Default();
    ds.Format = render::TextureFormat::D32_FLOAT;
    render::ColorTargetState color = render::ColorTargetState::Default(colorFormat);
    if (transparent) {
        // 透明: alpha blend, 关闭深度写 (复用不透明已写深度做遮挡), 双面可见 (不剔除)。
        ds.DepthWriteEnable = false;
        pass.Primitive.Cull = render::CullMode::None;
        color.Blend = render::BlendState{
            render::BlendComponent{
                render::BlendFactor::SrcAlpha,
                render::BlendFactor::OneMinusSrcAlpha,
                render::BlendOperation::Add},
            render::BlendComponent{
                render::BlendFactor::One,
                render::BlendFactor::OneMinusSrcAlpha,
                render::BlendOperation::Add}};
    }
    pass.DepthStencil = ds;
    pass.ColorTargets.push_back(color);
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
    return kw;
}

std::optional<ForwardShaderPair> BuildForwardShaderPair(
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

    // shadow caster 深度 shader (所有不透明材质共用同一份源)。
    std::optional<string> shadowSource;
    if (withShadowCaster) {
        shadowSource = ReadForwardShaderSource(shaderRoot, forward_pipeline::kShadowPassFile);
    }

    vector<ShaderPassDesc> opaquePasses;
    opaquePasses.push_back(MakeForwardPass(source.value(), shaderRoot, colorFormat, /*transparent*/ false));
    if (shadowSource.has_value()) {
        opaquePasses.push_back(MakeShadowCasterPass(shadowSource.value(), shaderRoot));
    }

    vector<ShaderPassDesc> transparentPasses;
    transparentPasses.push_back(MakeForwardPass(source.value(), shaderRoot, colorFormat, /*transparent*/ true));

    ForwardShaderPair pair{};
    pair.Opaque = assets.AddReady<ShaderAsset>(
        Guid::NewGuid(),
        make_unique<ShaderAsset>(keywords, std::move(opaquePasses)));
    pair.Transparent = assets.AddReady<ShaderAsset>(
        Guid::NewGuid(),
        make_unique<ShaderAsset>(keywords, std::move(transparentPasses)));
    return pair;
}

}  // namespace radray
