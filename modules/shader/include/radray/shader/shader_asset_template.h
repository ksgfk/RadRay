#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

#include <radray/shader/dxc.h>
#include <radray/shader/hlsl.h>
#include <radray/shader/shader_types.h>
#include <radray/shader/shader_manifest.h>
#include <radray/types.h>

// 从 HLSL 反射生成 *.shader.json 的【起始模板】, 不是可直接发布的 manifest。
//
// 反射给不出哪些字段、生成器的根本局限 (结果是"某一个变体"的下界):
// docs/architecture/shader-pipeline.md
//
// keyword 组为何以 HLSL 的 #pragma 为唯一权威, 改前先读
// docs/adr/0005-keyword-groups-declared-in-hlsl.md。

namespace radray {

/// keyword 声明的采纳范围。
enum class ShaderKeywordPragmaScope {
    /// 采纳预处理展开后的全部声明, 含被 include 进来的头文件。
    IncludeChain,
    /// 只采纳 pass 入口文件自己的声明, 按 #line 指令过滤。
    EntryFileOnly,
};

// ============================ 生成输入 ============================

/// 一个 pass 的生成种子。stage / entry point / 源文件路径都是编译【输入】,
/// 反射不出来, 必须由调用方给出。
struct ShaderTemplatePassSeed {
    /// 生成到 manifest 的 pass 名。留空则用 "main"。
    string Name;
    /// 相对 shader root 的源文件路径。留空则沿用资产级 Source。
    string Source;
    /// 要编译反射的 stage 与 entry point。至少一项。
    vector<ShaderStageDesc> Stages;
    render::HlslShaderModel ShaderModel{render::HlslShaderModel::SM60};
    /// 无条件生效的宏, 直接落进生成的 manifest 的 Defines。
    vector<string> Defines;

    /// 额外的反射轮次: 每个元素是一组同时开启的宏, 结果与其他轮次求并集。
    /// 【不】影响生成的 KeywordGroups —— 那由源码里的 #pragma 决定。
    ///
    /// 留空则按声明的 keyword 组自动推导轮次 (见 ProbeDeclaredKeywords)。只有需要
    /// "两个宏同时开启才出现的绑定"时才有必要手工指定。
    vector<vector<string>> ProbeDefineSets;

    bool IsOptimize{true};
    bool EnableUnbounded{true};
};

/// 一份资产的生成种子。
struct ShaderTemplateSeed {
    /// 生成到 manifest 的资产名。留空则取源文件主名。
    string Name;
    /// 资产级默认源文件, 相对 shader root。
    string Source;
    vector<ShaderTemplatePassSeed> Passes;
};

struct ShaderTemplateOptions {
    /// shader include 根目录。
    std::filesystem::path ShaderRoot;
    /// 同时用 SPIR-V 反射交叉核对。关掉会把 push constant 误写成一条普通 CBuffer 绑定
    /// (只有 SPIR-V 反射能识别 push constant)。
    /// 需要 RADRAY_ENABLE_SPIRV_CROSS; 未编入时该项被忽略并留下一条告警。
    bool UseSpirvReflection{true};
    /// 为 graphics pass 生成 VertexInput。
    bool GenerateVertexInput{true};
    /// 解析源码里的 `#pragma radray_keyword_group` 并写进生成的 KeywordGroups。
    bool ParseKeywordPragmas{true};
    /// 从哪些文件采纳 keyword 声明。默认整条 include 链, 取舍见 docs/adr/0005-*.md。
    ShaderKeywordPragmaScope KeywordPragmaScope{ShaderKeywordPragmaScope::IncludeChain};
    /// 未显式给 ProbeDefineSets 时, 按声明的 keyword 组自动推导探测轮次。
    ///
    /// 【不要关】关掉会静默漏掉被 #ifdef 守护的绑定 —— 生成的 manifest 合法、能 cook,
    /// 但少了一半 ABI。推导是【逐组各开一轮】而非全组合, 故轮次是 O(n) 不是 O(2^n)。
    bool ProbeDeclaredKeywords{true};
};

// ============================ 生成结果 ============================

/// 一条需要人工确认的项。对应序列化输出里的一条 "_TODO"。
struct ShaderTemplateTodo {
    /// 点位, 形如 "Passes[0].BindingGroups[2].Bindings[1].Residency"。
    /// 刻意用可读路径而非结构化字段: 读者是人, 路径能直接对上编辑器里的位置。
    string Path;
    /// 为什么反射给不出这一项, 以及作者该按什么依据填。
    string Reason;

    friend bool operator==(const ShaderTemplateTodo&, const ShaderTemplateTodo&) = default;
};

struct ShaderAssetTemplate {
    /// 生成的 manifest。已通过 ValidateShaderReflection 与 manifest 自校验。
    ShaderAssetDesc Asset;
    /// 需要人工确认的项, 按 Path 排序。空表示反射覆盖了全部字段 (少见)。
    vector<ShaderTemplateTodo> Todos;
    /// 生成期的告警。【告警不算失败】: 例如某个 probe 组合编译失败只意味着这一轮的
    /// 绑定没被并进来, 模板本身仍然可用。硬失败通过返回 nullopt 表达。
    vector<ShaderAssetDiagnostic> Warnings;
};

// ============================ keyword pragma ============================

/// 从【预处理后】的 HLSL 文本解析 `#pragma radray_keyword_group(...)`, 按出现顺序返回。
/// preprocessedText 必须是 Dxc::Preprocess* 的输出, 不是原始源码。
///
/// entrySourcePath 非空时只采纳该文件里的声明 (依赖输出里的 `#line N "file"` 指令,
/// 路径按大小写与分隔符归一化后比较); 留空则采纳整条 include 链的声明。
///
/// 只做行内语法校验。跨组约束 (组名重复、keyword 跨组撞名) 交由 manifest 校验统一报错,
/// 以免同一规则两处实现而口径分叉。语法错误返回 nullopt 并填 outDiag。
std::optional<vector<ShaderKeywordGroupDesc>> ParseShaderKeywordPragmas(
    std::string_view preprocessedText,
    std::string_view entrySourcePath,
    ShaderAssetDiagnostic& outDiag) noexcept;

/// 把 `#pragma radray_keyword_group(...)` 行清空 (替换为空行), 返回处理后的文本。
/// 【必须保持行数不变】否则 DXC 报的错误位置与预处理输出对不上, 且破坏 #line 指令语义。
string StripShaderKeywordPragmas(std::string_view preprocessedText) noexcept;

// ============================ 生成 ============================
//
// 生成需要现场编译 HLSL, 故只在启用 shader JIT (即有 DXC) 的构建里可用。

#if defined(RADRAY_ENABLE_SHADER_JIT)

/// 编译种子给出的每个 (pass, stage), 反射结果折叠为 manifest 初稿。
/// 失败 (源文件缺失、编译错误、反射类型无 RHI 对应物、自校验不通过) 返回 nullopt。
std::optional<ShaderAssetTemplate> GenerateShaderAssetTemplate(
    render::Dxc& dxc,
    const ShaderTemplateSeed& seed,
    const ShaderTemplateOptions& options,
    ShaderAssetDiagnostic& outDiag) noexcept;

#endif

/// 序列化模板为 JSON 文本。与 SerializeShaderAssetDesc 的差别仅在于额外写入 "_TODO"
/// 数组。该键不属于 manifest schema, ParseShaderAssetDesc 直接忽略它, 因此生成的文件
/// 【可以直接解析与 cook】。
std::optional<string> SerializeShaderAssetTemplate(
    const ShaderAssetTemplate& value,
    bool pretty = true) noexcept;

}  // namespace radray
