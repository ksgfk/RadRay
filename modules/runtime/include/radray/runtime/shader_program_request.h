#pragma once

#include <radray/render/backend/pipeline_layout_types.h>
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

}  // namespace radray
