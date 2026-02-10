#include <radray/logger.h>
#include <radray/file.h>
#include <radray/render/common.h>
#include <radray/render/bind_bridge.h>
#include <radray/render/dxc.h>
#include <radray/imgui/imgui_app.h>

using namespace radray;

int main(int argc, char** argv) {
    auto config = ImGuiApplication::ParseArgsSimple(argc, argv);
    string hlsl;
    {
        auto hlslOpt = file::ReadText(std::filesystem::path("assets") / "hello_bindless" / "bindless.hlsl");
        if (!hlslOpt.has_value()) {
            throw ImGuiApplicationException("Failed to read shader file bindless.hlsl");
        }
        hlsl = std::move(hlslOpt.value());
    }
    auto _dxc = render::CreateDxc();
    vector<std::string_view> defines;
    if (config.Backend == render::RenderBackend::Vulkan) {
        defines.emplace_back("VULKAN");
    } else if (config.Backend == render::RenderBackend::D3D12) {
        defines.emplace_back("D3D12");
    }
    vector<std::string_view> includes;
    includes.emplace_back("shaderlib");
    render::DxcOutput vsBin;
    {
        auto vs = _dxc->Compile(render::DxcCompileParams{
            hlsl,
            "VSMain",
            render::ShaderStage::Vertex,
            render::HlslShaderModel::SM60,
            defines, includes,
            true,
            config.Backend == render::RenderBackend::Vulkan,
            true});
        if (!vs.has_value()) {
            throw ImGuiApplicationException("Failed to compile vertex shader");
        }
        vsBin = std::move(vs.value());
    }
    render::DxcOutput psBin;
    {
        auto ps = _dxc->Compile(render::DxcCompileParams{
            hlsl,
            "PSMain",
            render::ShaderStage::Pixel,
            render::HlslShaderModel::SM60,
            defines, includes,
            true,
            config.Backend == render::RenderBackend::Vulkan,
            true});
        if (!ps.has_value()) {
            throw ImGuiApplicationException("Failed to compile pixel shader");
        }
        psBin = std::move(ps.value());
    }
    render::BindBridgeLayout bindLayout;
    if (config.Backend == render::RenderBackend::D3D12) {
        auto vsRefl = _dxc->GetShaderDescFromOutput(render::ShaderStage::Vertex, vsBin.Refl, vsBin.ReflExt).value();
        auto psRefl = _dxc->GetShaderDescFromOutput(render::ShaderStage::Pixel, psBin.Refl, psBin.ReflExt).value();
        const render::HlslShaderDesc* descs[] = {&vsRefl, &psRefl};
        auto mergedDesc = render::MergeHlslShaderDesc(descs).value();
        bindLayout = render::BindBridgeLayout{mergedDesc};
    } else if (config.Backend == render::RenderBackend::Vulkan) {
        render::SpirvBytecodeView spvs[] = {
            {vsBin.Data, "VSMain", render::ShaderStage::Vertex},
            {psBin.Data, "PSMain", render::ShaderStage::Pixel}};
        const render::DxcReflectionRadrayExt* extInfos[] = {&vsBin.ReflExt, &psBin.ReflExt};
        auto spirvDesc = render::ReflectSpirv(spvs, extInfos).value();
        bindLayout = render::BindBridgeLayout{spirvDesc};
    } else {
        throw ImGuiApplicationException("unsupported render backend for shader reflection");
    }
    RADRAY_INFO_LOG("{}", bindLayout.GetBindingId("_Obj").value());
    FlushLog();
    return 0;
}
