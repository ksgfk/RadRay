#pragma once

#include <optional>

#include <radray/nullable.h>
#include <radray/render/backend_shader_artifact.h>
#include <radray/render/pipeline_layout_types.h>
#include <radray/shader/shader_compiler_contract.h>
#include <radray/types.h>

namespace radray {

/// 一个 shader program 请求。它显式拥有决定身份的全部输入: 逻辑源名、结构化 defines、keyword
/// assignments、完整 compile policy 与按 target 分开的 layout recipe。discovery 与 compile 都由同一个
/// 请求驱动, 因此两者不会在不同 policy 下看到不同的 contract。
struct ShaderProgramRequest {
    string SourceName;
    vector<shader::Define> Defines{};
    vector<shader::KeywordAssignment> Assignments{};
    shader::CompilePolicy Policy{};
    /// 只影响 program/layout 身份, 不参与 compiler artifact 身份: 换掉非当前 backend 的 recipe 既不会
    /// 重新编译, 也不会新建 program。
    render::ShaderProgramLayoutRecipe LayoutRecipe{};
};

class ShaderProgram {
public:
    // The artifact already carries the layout the recipe produced, so the recipe itself is not an
    // input here: the resolved layout is what everything downstream reads.
    static Nullable<unique_ptr<ShaderProgram>> Create(
        render::Device* device,
        render::BackendShaderArtifact artifact) noexcept;

    ShaderProgram(const ShaderProgram&) = delete;
    ShaderProgram(ShaderProgram&&) = delete;
    ShaderProgram& operator=(const ShaderProgram&) = delete;
    ShaderProgram& operator=(ShaderProgram&&) = delete;
    ~ShaderProgram() noexcept;

    /// Borrowed stage and entry name; valid for this program's lifetime. Missing stages return nullopt.
    /// The caller creates and owns pipeline states through the RHI device.
    std::optional<render::ShaderEntry> GetStage(shader::ShaderStage stage) const noexcept;
    const render::BackendShaderArtifact& GetArtifact() const noexcept { return _artifact; }
    render::PipelineLayout* GetPipelineLayout() const noexcept { return _artifact.Layout.get(); }
    render::Device* GetDevice() const noexcept { return _device; }
    // True when the named buffer declaration takes its offset at bind time. Dynamic-ness is a
    // property of one declaration, not of a whole descriptor group.
    bool IsBufferDynamic(std::string_view declarationName) const noexcept;

    ShaderProgram(
        render::Device* device,
        render::BackendShaderArtifact artifact,
        unique_ptr<render::Shader> vertexShader,
        string vertexEntry,
        unique_ptr<render::Shader> pixelShader,
        string pixelEntry,
        unique_ptr<render::Shader> computeShader,
        string computeEntry) noexcept;

private:
    render::Device* _device;
    render::BackendShaderArtifact _artifact;
    unique_ptr<render::Shader> _vertexShader;
    string _vertexEntry;
    unique_ptr<render::Shader> _pixelShader;
    string _pixelEntry;
    unique_ptr<render::Shader> _computeShader;
    string _computeEntry;
};

}  // namespace radray
