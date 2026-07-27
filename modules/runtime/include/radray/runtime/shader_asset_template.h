#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

#include <radray/render/dxc.h>
#include <radray/render/hlsl.h>
#include <radray/render/rhi.h>
#include <radray/runtime/shader_asset.h>
#include <radray/types.h>

// 从 HLSL 反射生成 *.shader.json 的【起始模板】。
//
// == 存在理由 ==
//
// shader_asset.h 开头列出了 manifest 相对反射的七项增量, 结论是"反射不足以构建 PSO"。
// 但反过来说, manifest 里【大部分体力活】其实都是反射能给的: 每个绑定的名字、
// (space, register)、类型、数组容量, 以及顶点输入的 semantic 列表。手抄这些既枯燥又
// 容易在 HLSL 改动后忘记同步 —— 而这类不同步只会在 cook 期的反射核对里才暴露。
//
// 于是分工是: 生成器负责把【反射能确定的】部分写成合法的初始 manifest, 作者只需补上
// 反射【原理上给不出】的少量决策。生成器与校验器共用同一套反射折叠规则
// (src/shader_reflection_map.h), 所以生成出的模板天然能通过 ValidateShaderReflection。
//
// == 反射给不出、必须人工确认的字段 ==
//
// 生成器为它们填保守默认值, 同时在 ShaderAssetTemplate::Todos 里逐条点名, 并在
// 序列化输出里写 "_TODO" 键 (JSON 无注释, 只能借键传达)。默认值的选取原则是
// 「宁可保守到需要改, 不可乐观到能跑但错」:
//
//   1. Residency —— 全部 DescriptorTable。做 root descriptor 是性能决策, 反射无从得知。
//   2. VertexFormat —— 按 32 位分量数直译 (FLOAT32X3 / UINT32X4 ...)。DXIL 只给
//      ComponentType + Mask, 推不出归一化与位宽, 故 UNORM8X4、FLOAT16X2 一律要手改。
//   3. VertexInput 的 ArrayStride / Offset —— 按属性声明顺序紧密打包。真实顶点布局
//      由 mesh 决定, 与 shader 无关, 生成值只是一个能自洽的起点。
//   4. unbounded 数组容量 —— 反射只说 unbounded, 两个后端都拒绝 Count == 0, 生成器
//      填 1 并点名。
//   5. ImmutableSampler —— 纯 pipeline-layout 期概念, 不在字节码里, 一律不生成。
//   6. BakeVariants —— 发布决策而非 shader 属性: 同一份 HLSL 在 PC 与移动端可以烘不同
//      的集合, 且改烘焙范围不该动 shader 源码 (会让所有产物 cache 失效)。故留在
//      manifest 由作者声明, 生成器只点名。
//   7. 被 DCE 或 #ifdef 消掉的绑定 —— 反射看不见。这是生成器的根本局限: 见下。
//
// KeywordGroups 【不】在此列 —— 它由 HLSL 里的 #pragma 声明并自动生成, 见下节。
//
// == 根本局限: 生成结果是"某一个变体"的下界 ==
//
// 反射只能看到【实际编译出来的那份字节码】里活着的绑定。因此:
//   - 不给 keyword 种子时, 生成的是默认变体的绑定集合;
//   - 被 #ifdef 关掉的分支所用的绑定不会出现。
// 由于 PipelineLayout 必须对所有变体一致 (否则同一 shader 的不同变体无法共用一个
// layout), 作者必须把缺失的槽位补进 manifest。为缓解这点, Seed 支持给多组 keyword:
// 生成器对每一组分别编译反射, 再把结果【求并集】, 覆盖面随种子组数增长。
//
// 因此本生成器的产物是【模板】, 不是可直接发布的 manifest —— 它把 manifest 的初稿
// 成本从"手抄全部"降到"审阅并补齐 TODO"。
//
// == keyword 组: 以 HLSL 里的 #pragma 为唯一权威 ==
//
// 一个组由哪些互斥宏构成、是否允许全关、影响哪些 stage —— 这些是 shader 源码的编译期
// 接口, 与函数签名同性质: 改了 #ifdef 就得改它。写在 manifest 里等于把同一份声明抄
// 两遍, 而两侧【无法】互相校验: manifest 里把 _BASECOLOR_MAP 拼成 _BASECOLOR_MAPP
// 不会有任何报错, 只会静默编出一个所有 #ifdef 都不成立的变体。故声明放回 HLSL:
//
//   #pragma radray_keyword_group(BaseColorMap, _BASECOLOR_MAP) stages(Pixel)
//   #pragma radray_keyword_group(AlphaMode, _ALPHATEST_ON, _ALPHABLEND_ON) stages(Pixel)
//   #pragma radray_keyword_group(Lighting, _LIT, _UNLIT) stages(Vertex, Pixel) required
//
// 第一个标识符是组名, 其后是组内【互斥】的 keyword (至少一个)。stages(...) 省略则取
// Graphics; required 关掉 IsOptional, 即该组不允许全关。DXC 忽略未知 pragma, 这些行
// 不影响编译 (实测 -WX 亦不告警)。
//
// 【刻意不做 #ifdef 交叉校验】: pragma 即契约本身。HLSL 里 #ifdef 写错宏名属于普通
// 代码 bug, 不该由本工具链承担 —— 那等于让"扫源码猜意图"参与定义 ABI。
//
// 解析基于 DXC 的预处理输出 (dxc -P), 不是自己写的词法扫描。于是块注释、续行符、
// #if 0 全部由编译器正确处理: 注释掉的 pragma 不会被误认为声明。
//
// 【默认采纳整条 include 链】: 预处理已把 include 展开, 于是提供某组绑定的那个头文件
// 可以把对应的 keyword 声明放在自己身边 —— 声明与被它守护的 #ifdef 同文件, 是唯一
// 不会失同步的位置。例如 forward_interface.hlsl 声明阴影绑定, 就由它自己声明
// PointShadows / DirectionalShadows, 每个 include 它的入口文件自动继承。
//
// 代价是"include 了某个头文件"就意味着"继承它的全部变体维度", 即使本 shader 并不
// 使用。若某个入口文件确实只想认自己的声明, 用 ShaderTemplateOptions::
// KeywordPragmaScope 切到 EntryFileOnly。
//
// 【pragma 必须在无条件位置】: 预处理 respect -D, 所以被 #ifdef 包住的 pragma 只在
// 对应宏开启时才可见 —— 那会形成"要先知道 keyword 才能发现 keyword"的循环。把
// keyword 声明写在任何条件块之外 (头文件的 include guard 不算, 它总是成立)。

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

    /// 额外的反射轮次: 每个元素是一组同时开启的宏。
    ///
    /// 存在理由见文件头"根本局限": 单轮反射只能看到默认变体活着的绑定。给出
    /// keyword 组合可以让生成器多编几遍并把绑定集合求并, 显著减少需要手补的槽位。
    /// 【不】影响生成的 KeywordGroups —— 那由源码里的 #pragma 决定。
    ///
    /// 留空时生成器按声明的 keyword 组自动推导轮次
    /// (见 ShaderTemplateOptions::ProbeDeclaredKeywords)。显式给出则取代自动推导 ——
    /// 需要"两个宏同时开启才出现的绑定"时才有必要手工指定。
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
    /// 同时用 SPIR-V 反射交叉核对。
    ///
    /// 【为何默认开启】: push constant 只有 SPIR-V 反射能识别 —— DXIL 把
    /// [[vk::push_constant]] 的 cbuffer 当普通 cbuffer, 完全不知道它特殊。关掉这项,
    /// 生成的模板会把 push constant 误写成一条普通 CBuffer 绑定, 作者必须手工搬移。
    /// 需要 RADRAY_ENABLE_SPIRV_CROSS; 未编入时该项被忽略并留下一条告警。
    bool UseSpirvReflection{true};
    /// 为 graphics pass 生成 VertexInput。
    bool GenerateVertexInput{true};
    /// 解析源码里的 `#pragma radray_keyword_group` 并写进生成的 KeywordGroups。
    bool ParseKeywordPragmas{true};
    /// 从哪些文件采纳 keyword 声明。默认整条 include 链, 使提供绑定的头文件能把
    /// keyword 声明放在它守护的 #ifdef 旁边 (见文件头)。
    ShaderKeywordPragmaScope KeywordPragmaScope{ShaderKeywordPragmaScope::IncludeChain};
    /// 未显式给 ProbeDefineSets 时, 按声明的 keyword 组自动推导探测轮次。
    ///
    /// 【为何默认开启】: 被 #ifdef 包住的绑定只在对应 keyword 开启时才进入字节码
    /// (见文件头"根本局限")。不探测就会静默漏掉 —— 生成的 manifest 合法、能 cook, 但
    /// 少了一半 ABI, 是最难发现的一类错误。既然 pragma 已把 keyword 全集告诉生成器,
    /// 就没有理由还要求使用者手工重复一遍。
    ///
    /// 推导方式是【逐组各开一轮】而非全组合: n 组时轮次为 O(n) 不是 O(2^n)。绑定与
    /// keyword 通常一一对应 (一张贴图一个宏), 逐组开启即可覆盖。
    bool ProbeDeclaredKeywords{true};
};

// ============================ 生成结果 ============================

/// 一条需要人工确认的项。对应序列化输出里的一条 "_TODO"。
struct ShaderTemplateTodo {
    /// 点位, 形如 "Passes[0].BindingGroups[2].Bindings[1].Residency"。
    /// 刻意用可读路径而非结构化字段: 使用者是人, 而路径能直接对上编辑器里的位置。
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
    /// 生成期的告警。空表示没有可疑之处。
    ///
    /// 【告警不算失败】: 例如"某个 probe 组合编译失败"只意味着这一轮的绑定没被并进来,
    /// 模板本身仍然可用。硬失败通过返回 nullopt 表达。
    vector<ShaderAssetDiagnostic> Warnings;
};

// ============================ keyword pragma ============================

/// 从【预处理后】的 HLSL 文本解析 `#pragma radray_keyword_group(...)`, 按出现顺序返回。
///
/// 语法与设计理由见文件头。text 应当是 Dxc::Preprocess* 的输出。
///
/// entrySourcePath 非空时只采纳该文件里的声明 (需要输出里的 `#line N "file"` 指令,
/// 路径按大小写与分隔符归一化后比较); 留空则采纳整条 include 链的声明。
///
/// 只做行内语法校验 (组名非空、至少一个 keyword、stage 名合法、修饰符已知)。组名
/// 重复、keyword 跨组撞名这类【跨组】约束交由 manifest 校验统一报错, 以免同一规则
/// 两处实现而口径分叉。语法错误返回 nullopt 并填 outDiag。
std::optional<vector<ShaderKeywordGroupDesc>> ParseShaderKeywordPragmas(
    std::string_view preprocessedText,
    std::string_view entrySourcePath,
    ShaderAssetDiagnostic& outDiag) noexcept;

/// 把 `#pragma radray_keyword_group(...)` 行清空, 返回处理后的文本。
///
/// 【为何替换为空行而非删除】: 保持行号不变, 使 DXC 对这份文本报的错误位置仍与
/// 预处理输出一一对应, 也不破坏其中的 #line 指令语义。
///
/// 留着这些 pragma 编译本身无害 (DXC 静默忽略未知 pragma), 剥离是为了不把仅供工具链
/// 消费的声明喂给编译器。
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

/// 序列化模板为 JSON 文本。
///
/// 与 SerializeShaderAssetDesc 的差别仅在于额外写入 "_TODO" 数组 (资产级一条,
/// 每个待确认点位一条对象)。JSON 不支持注释, 而模板必须能把"这里要人改"随文件一起
/// 交到作者手上, 故借一个下划线前缀的键传达 —— 它不属于 manifest schema, 由
/// ParseShaderAssetDesc 直接忽略, 因此生成的文件【可以直接解析与 cook】。
std::optional<string> SerializeShaderAssetTemplate(
    const ShaderAssetTemplate& value,
    bool pretty = true) noexcept;

}  // namespace radray
