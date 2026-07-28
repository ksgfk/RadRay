#include <radray/shader/shader_asset_template.h>

#include <algorithm>
#include <array>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <radray/basic_math.h>
#include <radray/enum_flags.h>
#include <radray/json.h>

#if defined(RADRAY_ENABLE_SPIRV_CROSS)
#include <radray/shader/spvc.h>
#endif

#include "shader_manifest_json.h"
#include "shader_reflection_map.h"

namespace radray {

namespace {

/// "_TODO" 的键名。下划线前缀标明它不属于 manifest schema。
constexpr std::string_view kTodoKey = "_TODO";

ShaderAssetDiagnostic MakeTemplateDiag(
    string message,
    std::string_view passName = {},
    std::optional<render::ShaderStage> stage = std::nullopt) {
    ShaderAssetDiagnostic diag;
    diag.Message = std::move(message);
    diag.PassName = string{passName};
    diag.Stage = stage;
    return diag;
}

}  // namespace

// ============================ keyword pragma ============================

namespace {

constexpr std::string_view kKeywordPragmaName = "radray_keyword_group";

bool IsIdentifierChar(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

bool IsHorizontalSpace(char c) noexcept {
    return c == ' ' || c == '\t';
}

/// 按 '\n' 切出下一行 (去掉尾部 '\r')。返回是否取到了行。
/// cursor 推进到下一行起点; 用 cursor <= size 的循环条件使最后一行无换行符也能取到。
bool NextLine(std::string_view text, size_t& cursor, std::string_view& outLine) noexcept {
    if (cursor > text.size()) {
        return false;
    }
    const size_t lineEnd = text.find('\n', cursor);
    if (lineEnd == std::string_view::npos) {
        outLine = text.substr(cursor);
        cursor = text.size() + 1;
    } else {
        outLine = text.substr(cursor, lineEnd - cursor);
        cursor = lineEnd + 1;
    }
    if (!outLine.empty() && outLine.back() == '\r') {
        outLine.remove_suffix(1);
    }
    return true;
}

/// 若该行是预处理指令, 返回 '#' 之后的内容; 否则返回 nullopt。
/// 允许 '#' 前有空白, 也允许 "# pragma" 这种 '#' 与指令名之间有空格的写法。
std::optional<std::string_view> AsDirective(std::string_view line) noexcept {
    size_t i = 0;
    while (i < line.size() && IsHorizontalSpace(line[i])) {
        ++i;
    }
    if (i >= line.size() || line[i] != '#') {
        return std::nullopt;
    }
    return line.substr(i + 1);
}

/// 一行内的游标式扫描器。刻意不用正则: 需要精确的错误定位, 而正则失败只能报
/// "整行不匹配"。
class LineCursor {
public:
    explicit LineCursor(std::string_view text) noexcept : _text(text) {}

    void SkipSpace() noexcept {
        while (_pos < _text.size() && IsHorizontalSpace(_text[_pos])) {
            ++_pos;
        }
    }

    bool Eat(char expected) noexcept {
        SkipSpace();
        if (_pos < _text.size() && _text[_pos] == expected) {
            ++_pos;
            return true;
        }
        return false;
    }

    /// 读一个标识符。返回空表示当前位置不是标识符起始。
    std::string_view Identifier() noexcept {
        SkipSpace();
        const size_t begin = _pos;
        while (_pos < _text.size() && IsIdentifierChar(_text[_pos])) {
            ++_pos;
        }
        return _text.substr(begin, _pos - begin);
    }

    bool AtEnd() noexcept {
        SkipSpace();
        return _pos >= _text.size();
    }

    /// 剩余文本, 用于报告多余的尾随内容。
    std::string_view Rest() noexcept {
        SkipSpace();
        return _text.substr(_pos);
    }

private:
    std::string_view _text;
    size_t _pos{0};
};

/// 该行是否是一条 keyword pragma。是则返回参数部分 (pragma 名之后的内容)。
std::optional<std::string_view> AsKeywordPragma(std::string_view line) noexcept {
    const std::optional<std::string_view> directive = AsDirective(line);
    if (!directive.has_value()) {
        return std::nullopt;
    }
    LineCursor cursor{directive.value()};
    if (cursor.Identifier() != "pragma") {
        return std::nullopt;
    }
    if (cursor.Identifier() != kKeywordPragmaName) {
        return std::nullopt;
    }
    return cursor.Rest();
}

/// 解析 `#line N "path"` 里的路径。DXC 的预处理输出用它标记后续行的归属文件。
std::optional<std::string_view> AsLineDirectivePath(std::string_view line) noexcept {
    const std::optional<std::string_view> directive = AsDirective(line);
    if (!directive.has_value()) {
        return std::nullopt;
    }
    LineCursor cursor{directive.value()};
    if (cursor.Identifier() != "line") {
        return std::nullopt;
    }
    // 行号本身无用: 我们只关心归属文件。
    if (cursor.Identifier().empty()) {
        return std::nullopt;
    }
    const std::string_view rest = cursor.Rest();
    if (rest.size() < 2 || rest.front() != '"') {
        return std::nullopt;
    }
    const size_t closing = rest.find('"', 1);
    if (closing == std::string_view::npos) {
        return std::nullopt;
    }
    return rest.substr(1, closing - 1);
}

/// 路径归一化到可比较形式: 反斜杠转正斜杠 + 转小写。
///
/// 【为何不用 filesystem::equivalent】: #line 里的路径可能已不存在于磁盘 (来自内存
/// 编译), 且 equivalent 会做 IO。这里只需判断"是不是同一个源文件", 文本归一化足够,
/// 且 DXC 输出的路径与我们传入的路径同源。
string NormalizePathForCompare(std::string_view path) noexcept {
    string result;
    result.reserve(path.size());
    for (const char c : path) {
        // DXC 的 #line 会把反斜杠转义成 "\\", 归一化时按单个分隔符处理即可。
        const char normalized = (c == '\\') ? '/' : c;
        if (normalized == '/' && !result.empty() && result.back() == '/') {
            continue;
        }
        result.push_back(static_cast<char>(
            (normalized >= 'A' && normalized <= 'Z') ? (normalized - 'A' + 'a') : normalized));
    }
    return result;
}

ShaderAssetDiagnostic MakePragmaDiag(std::string_view detail) {
    return MakeTemplateDiag(fmt::format("#pragma {}: {}", kKeywordPragmaName, detail));
}

/// stage 名 -> ShaderStage。只认这三个: 生成器只支持 vertex/pixel/compute 三种 entry。
std::optional<render::ShaderStage> ParseStageName(std::string_view name) noexcept {
    if (EqualsIgnoreCase(name, "Vertex")) {
        return render::ShaderStage::Vertex;
    }
    if (EqualsIgnoreCase(name, "Pixel")) {
        return render::ShaderStage::Pixel;
    }
    if (EqualsIgnoreCase(name, "Compute")) {
        return render::ShaderStage::Compute;
    }
    return std::nullopt;
}

/// 解析 `(Name, KW1, KW2, ...)`。
bool ParseGroupBody(
    LineCursor& cursor,
    ShaderKeywordGroupDesc& group,
    ShaderAssetDiagnostic& outDiag) noexcept {
    if (!cursor.Eat('(')) {
        outDiag = MakePragmaDiag("expected '(' after the pragma name");
        return false;
    }
    const std::string_view name = cursor.Identifier();
    if (name.empty()) {
        outDiag = MakePragmaDiag("expected a group name");
        return false;
    }
    group.Name = string{name};
    while (cursor.Eat(',')) {
        const std::string_view keyword = cursor.Identifier();
        if (keyword.empty()) {
            outDiag = MakePragmaDiag(
                fmt::format("group '{}' has an empty or trailing keyword", group.Name));
            return false;
        }
        group.Keywords.push_back(string{keyword});
    }
    if (!cursor.Eat(')')) {
        outDiag = MakePragmaDiag(
            fmt::format("group '{}' is missing the closing ')'", group.Name));
        return false;
    }
    if (group.Keywords.empty()) {
        // 一个组至少要有一个取值。"全关"由 IsOptional 表达, 不是取值之一。
        outDiag = MakePragmaDiag(
            fmt::format("group '{}' must declare at least one keyword", group.Name));
        return false;
    }
    return true;
}

/// 解析 `stages(A, B)` 的括号内容。
bool ParseStagesClause(
    LineCursor& cursor,
    ShaderKeywordGroupDesc& group,
    ShaderAssetDiagnostic& outDiag) noexcept {
    if (!cursor.Eat('(')) {
        outDiag = MakePragmaDiag(
            fmt::format("group '{}': expected '(' after 'stages'", group.Name));
        return false;
    }
    render::ShaderStages stages{render::ShaderStage::UNKNOWN};
    bool first = true;
    while (true) {
        if (!first && !cursor.Eat(',')) {
            break;
        }
        const std::string_view stageName = cursor.Identifier();
        if (stageName.empty()) {
            outDiag = MakePragmaDiag(
                first
                    ? fmt::format("group '{}': 'stages' must list at least one stage", group.Name)
                    : fmt::format("group '{}': trailing ',' in 'stages'", group.Name));
            return false;
        }
        const std::optional<render::ShaderStage> stage = ParseStageName(stageName);
        if (!stage.has_value()) {
            outDiag = MakePragmaDiag(fmt::format(
                "group '{}': unknown stage '{}' (expected Vertex, Pixel or Compute)",
                group.Name,
                stageName));
            return false;
        }
        stages |= stage.value();
        first = false;
    }
    if (!cursor.Eat(')')) {
        outDiag = MakePragmaDiag(
            fmt::format("group '{}': 'stages' is missing the closing ')'", group.Name));
        return false;
    }
    group.Stages = stages;
    return true;
}

/// 解析一条 pragma 的参数部分为一个组。
bool ParseOneKeywordPragma(
    std::string_view arguments,
    ShaderKeywordGroupDesc& group,
    ShaderAssetDiagnostic& outDiag) noexcept {
    LineCursor cursor{arguments};
    if (!ParseGroupBody(cursor, group, outDiag)) {
        return false;
    }
    // 修饰符可按任意顺序出现, 各自最多一次。
    bool sawStages = false;
    bool sawRequired = false;
    while (!cursor.AtEnd()) {
        const std::string_view modifier = cursor.Identifier();
        if (modifier.empty()) {
            outDiag = MakePragmaDiag(fmt::format(
                "group '{}': unexpected trailing text '{}'", group.Name, cursor.Rest()));
            return false;
        }
        if (EqualsIgnoreCase(modifier, "stages")) {
            if (sawStages) {
                outDiag = MakePragmaDiag(
                    fmt::format("group '{}': duplicate 'stages'", group.Name));
                return false;
            }
            if (!ParseStagesClause(cursor, group, outDiag)) {
                return false;
            }
            sawStages = true;
            continue;
        }
        if (EqualsIgnoreCase(modifier, "required")) {
            if (sawRequired) {
                outDiag = MakePragmaDiag(
                    fmt::format("group '{}': duplicate 'required'", group.Name));
                return false;
            }
            group.IsOptional = false;
            sawRequired = true;
            continue;
        }
        outDiag = MakePragmaDiag(fmt::format(
            "group '{}': unknown modifier '{}' (expected 'stages' or 'required')",
            group.Name,
            modifier));
        return false;
    }
    return true;
}

}  // namespace

std::optional<vector<ShaderKeywordGroupDesc>> ParseShaderKeywordPragmas(
    std::string_view preprocessedText,
    std::string_view entrySourcePath,
    ShaderAssetDiagnostic& outDiag) noexcept {
    outDiag = ShaderAssetDiagnostic{};
    vector<ShaderKeywordGroupDesc> groups;

    const bool filterByFile = !entrySourcePath.empty();
    const string wantedFile = filterByFile ? NormalizePathForCompare(entrySourcePath) : string{};
    // 预处理输出的首个 #line 之前的内容属于入口文件本身。
    bool inEntryFile = true;

    size_t cursor = 0;
    std::string_view line;
    while (NextLine(preprocessedText, cursor, line)) {
        if (const std::optional<std::string_view> path = AsLineDirectivePath(line);
            path.has_value()) {
            inEntryFile = !filterByFile || NormalizePathForCompare(path.value()) == wantedFile;
            continue;
        }
        const std::optional<std::string_view> arguments = AsKeywordPragma(line);
        if (!arguments.has_value()) {
            continue;
        }
        if (!inEntryFile) {
            continue;
        }
        ShaderKeywordGroupDesc group{};
        if (!ParseOneKeywordPragma(arguments.value(), group, outDiag)) {
            return std::nullopt;
        }
        groups.push_back(std::move(group));
    }
    return groups;
}

string StripShaderKeywordPragmas(std::string_view preprocessedText) noexcept {
    string result;
    result.reserve(preprocessedText.size());

    size_t cursor = 0;
    std::string_view line;
    bool first = true;
    while (NextLine(preprocessedText, cursor, line)) {
        if (!first) {
            result.push_back('\n');
        }
        first = false;
        // 命中则留一个空行占位, 保持后续行号不变。
        if (!AsKeywordPragma(line).has_value()) {
            result.append(line);
        }
    }
    return result;
}

#if defined(RADRAY_ENABLE_SHADER_JIT)

namespace {

/// 合并中的一条绑定。跨 stage 与跨 probe 轮次累积。
struct MergedBinding {
    string Name;
    uint32_t Group{0};
    uint32_t Binding{0};
    render::ShaderParameterBindingType Type{render::ShaderParameterBindingType::UNKNOWN};
    /// 已折叠为 manifest 口径: unbounded 落成 1, 并记在 IsUnbounded 上。
    uint32_t Count{1};
    bool IsUnbounded{false};
    render::ShaderStages Stages{render::ShaderStage::UNKNOWN};
};

/// 合并中的一条顶点属性。
struct MergedAttribute {
    string Semantic;
    uint32_t SemanticIndex{0};
    /// SPIRV location 与 DXIL 的输入寄存器号。DXC 对 VS 输入两者一致, 故直接采用,
    /// 使生成的模板同时满足 DXIL 与 SPIRV 两条核对路径。
    uint32_t Location{0};
    render::VertexFormat Format{render::VertexFormat::UNKNOWN};
    /// true 表示位宽/归一化是猜的, 需要人工确认。
    bool FormatIsGuess{true};
};

/// 一个 pass 的反射合并结果。
struct MergedPass {
    vector<MergedBinding> Bindings;
    vector<MergedAttribute> Attributes;
    /// SPIRV 反射认出的 push constant。DXIL 认不出来 (见 shader_manifest.h 头注释)。
    bool HasPushConstant{false};
    string PushConstantName;
    uint32_t PushConstantSize{0};
    render::ShaderStages PushConstantStages{render::ShaderStage::UNKNOWN};
};

/// DXIL 分量类型 + 写掩码 -> VertexFormat。
///
/// 【必然是猜测】: 反射只说"4 个 float32 分量", 而 VertexFormat 描述的是 vertex
/// buffer 里的存储格式 —— UNORM8X4 与 FLOAT32X4 在 shader 里都是 float4, 反射完全
/// 无法区分。故这里一律返回最宽的直译形式, 并让调用方标记为待确认。
std::optional<render::VertexFormat> GuessVertexFormat(
    render::HlslRegisterComponentType component,
    uint8_t mask) noexcept {
    uint32_t count = 0;
    for (uint32_t bit = 0; bit < 4; ++bit) {
        if ((mask & (1u << bit)) != 0) {
            ++count;
        }
    }
    if (count == 0 || count > 4) {
        return std::nullopt;
    }
    using CT = render::HlslRegisterComponentType;
    switch (component) {
        case CT::FLOAT32:
        case CT::FLOAT16:
        case CT::FLOAT64: {
            constexpr std::array<render::VertexFormat, 4> table{
                render::VertexFormat::FLOAT32,
                render::VertexFormat::FLOAT32X2,
                render::VertexFormat::FLOAT32X3,
                render::VertexFormat::FLOAT32X4};
            return table[count - 1];
        }
        case CT::UINT32:
        case CT::UINT16:
        case CT::UINT64: {
            constexpr std::array<render::VertexFormat, 4> table{
                render::VertexFormat::UINT32,
                render::VertexFormat::UINT32X2,
                render::VertexFormat::UINT32X3,
                render::VertexFormat::UINT32X4};
            return table[count - 1];
        }
        case CT::SINT32:
        case CT::SINT16:
        case CT::SINT64: {
            constexpr std::array<render::VertexFormat, 4> table{
                render::VertexFormat::SINT32,
                render::VertexFormat::SINT32X2,
                render::VertexFormat::SINT32X3,
                render::VertexFormat::SINT32X4};
            return table[count - 1];
        }
        default:
            return std::nullopt;
    }
}

/// 把一条反射绑定并入合并集。冲突写告警但不失败 —— 不同变体对同一槽位给出不同
/// 类型是 HLSL 侧的真实问题, 但模板仍应生成出来供作者查看。
void MergeBinding(
    MergedPass& merged,
    const ReflectedBinding& item,
    render::ShaderStage stage,
    std::string_view passName,
    vector<ShaderAssetDiagnostic>& warnings) {
    const uint32_t count = item.Count == 0 ? 1u : item.Count;
    for (MergedBinding& existing : merged.Bindings) {
        if (existing.Group != item.Group || existing.Binding != item.Binding) {
            continue;
        }
        if (existing.Name != item.Name || existing.Type != item.Type) {
            warnings.push_back(MakeTemplateDiag(
                fmt::format(
                    "reflection disagrees at (group {}, binding {}): '{}' {} vs '{}' {}; "
                    "kept the first and left the slot for review",
                    item.Group,
                    item.Binding,
                    existing.Name,
                    EnumNameOr(existing.Type, "UNKNOWN"),
                    item.Name,
                    EnumNameOr(item.Type, "UNKNOWN")),
                passName,
                stage));
        }
        existing.Stages |= stage;
        existing.IsUnbounded |= item.Count == 0;
        existing.Count = std::max(existing.Count, count);
        return;
    }
    MergedBinding fresh{};
    fresh.Name = string{item.Name};
    fresh.Group = item.Group;
    fresh.Binding = item.Binding;
    fresh.Type = item.Type;
    fresh.Count = count;
    fresh.IsUnbounded = item.Count == 0;
    fresh.Stages = render::ShaderStages{stage};
    merged.Bindings.push_back(std::move(fresh));
}

/// 反射一轮 DXIL: 绑定 + (VS 时) 顶点输入。
bool AbsorbHlslReflection(
    MergedPass& merged,
    const render::HlslShaderDesc& reflection,
    render::ShaderStage stage,
    std::string_view passName,
    bool generateVertexInput,
    vector<ShaderAssetDiagnostic>& warnings,
    ShaderAssetDiagnostic& outDiag) {
    for (const render::HlslInputBindDesc& bind : reflection.BoundResources) {
        std::optional<ReflectedBinding> item = MakeReflectedBinding(bind);
        if (!item.has_value()) {
            outDiag = MakeTemplateDiag(
                fmt::format(
                    "reflection reports resource '{}' with a type that has no RHI binding equivalent",
                    bind.Name),
                passName,
                stage);
            return false;
        }
        MergeBinding(merged, item.value(), stage, passName, warnings);
    }

    if (!generateVertexInput || stage != render::ShaderStage::Vertex) {
        return true;
    }
    for (const render::HlslSignatureParameterDesc& input : reflection.InputParameters) {
        std::string_view baseName{};
        uint32_t nameIndex = 0;
        SplitSemantic(input.SemanticName, baseName, nameIndex);
        if (IsSystemSemantic(baseName)) {
            continue;  // 系统值不由 vertex buffer 提供。
        }
        const uint32_t semanticIndex =
            EffectiveSemanticIndex(input.SemanticName, input.SemanticIndex);
        const bool known = std::any_of(
            merged.Attributes.begin(),
            merged.Attributes.end(),
            [&](const MergedAttribute& attribute) noexcept {
                return EqualsIgnoreCase(attribute.Semantic, baseName) &&
                       attribute.SemanticIndex == semanticIndex;
            });
        if (known) {
            continue;
        }
        std::optional<render::VertexFormat> format =
            GuessVertexFormat(input.ComponentType, input.Mask);
        if (!format.has_value()) {
            outDiag = MakeTemplateDiag(
                fmt::format(
                    "cannot infer a VertexFormat for semantic '{}' (component type {}, mask {:#x})",
                    input.SemanticName,
                    EnumNameOr(input.ComponentType, "UNKNOWN"),
                    input.Mask),
                passName,
                stage);
            return false;
        }
        MergedAttribute attribute{};
        attribute.Semantic = string{baseName};
        attribute.SemanticIndex = semanticIndex;
        attribute.Location = input.Register;
        attribute.Format = format.value();
        merged.Attributes.push_back(std::move(attribute));
    }
    return true;
}

#if defined(RADRAY_ENABLE_SPIRV_CROSS)

/// 反射一轮 SPIR-V。只取 DXIL 拿不到的东西: push constant 的身份与大小。
///
/// 绑定集合【不】从这里并入: SPIR-V 侧的 set/binding 经过 DXC 的重映射, 与 HLSL 的
/// (space, register) 不一定一致, 而 manifest 的 Group/Binding 定义在 HLSL 口径上。
void AbsorbSpirvReflection(
    MergedPass& merged,
    const render::SpirvShaderDesc& reflection,
    render::ShaderStage stage,
    std::string_view passName,
    vector<ShaderAssetDiagnostic>& warnings) {
    if (reflection.ConstantRanges.empty()) {
        return;
    }
    if (reflection.ConstantRanges.size() > 1) {
        warnings.push_back(MakeTemplateDiag(
            fmt::format(
                "reflection reports {} push constant ranges; only the first was used",
                reflection.ConstantRanges.size()),
            passName,
            stage));
    }
    const render::SpirvPushConstantRange& range = reflection.ConstantRanges.front();
    const uint32_t size = static_cast<uint32_t>(Align(range.Offset + range.Size, 4u));
    if (merged.HasPushConstant && merged.PushConstantName != range.Name) {
        warnings.push_back(MakeTemplateDiag(
            fmt::format(
                "push constant name differs across stages: '{}' vs '{}'; kept the first",
                merged.PushConstantName,
                range.Name),
            passName,
            stage));
    } else if (!merged.HasPushConstant) {
        merged.PushConstantName = range.Name;
    }
    merged.HasPushConstant = true;
    merged.PushConstantSize = std::max(merged.PushConstantSize, size);
    merged.PushConstantStages |= stage;
}

#endif

/// 在给定宏环境下预处理一遍源文件, 返回展开后的 HLSL 文本。
///
/// stage / entry point 对预处理无作用, 但 DXC 的参数构建要求它们存在, 故取 pass 的
/// 首个 stage 充当。
std::optional<string> PreprocessRound(
    render::Dxc& dxc,
    const std::filesystem::path& sourceFile,
    std::string_view sourceLabel,
    const ShaderTemplatePassSeed& seed,
    const vector<string>& defines,
    std::span<const std::string_view> includes,
    ShaderAssetDiagnostic& outDiag) {
    vector<std::string_view> defineViews;
    defineViews.reserve(defines.size());
    for (const string& define : defines) {
        defineViews.emplace_back(define);
    }
    const ShaderStageDesc& anyStage = seed.Stages.front();
    const render::DxcCompileOptions preprocessOptions{
        .EntryPoint = anyStage.EntryPoint,
        .Stage = anyStage.Stage,
        .SM = seed.ShaderModel,
        .Defines = defineViews,
        .Includes = includes,
        .IsOptimize = seed.IsOptimize,
        .IsSpirv = false,
        .EnableUnbounded = seed.EnableUnbounded};
    std::optional<string> text = dxc.PreprocessFile(sourceFile, preprocessOptions);
    if (!text.has_value()) {
        outDiag = MakeTemplateDiag(
            fmt::format("failed to preprocess '{}'", sourceLabel),
            seed.Name);
        return std::nullopt;
    }
    return text;
}

/// 一轮编译 + 反射。编译失败不是硬错误 —— probe 组合可能本就无效, 此时只是这一轮
/// 的绑定没被并进来。返回 false 表示【反射内容】不可接受, 应中止。
bool ProbeOnce(
    render::Dxc& dxc,
    MergedPass& merged,
    std::string_view sourceCode,
    std::string_view sourceLabel,
    const ShaderTemplatePassSeed& seed,
    const ShaderStageDesc& stageDesc,
    std::span<const std::string_view> defines,
    std::span<const std::string_view> includes,
    const ShaderTemplateOptions& options,
    bool useSpirv,
    vector<ShaderAssetDiagnostic>& warnings,
    ShaderAssetDiagnostic& outDiag) {
    const render::ShaderStage stage = stageDesc.Stage;

    // sourceCode 已是【本轮 defines 下预处理并剥掉 keyword pragma】的文本。故此处
    // 仍要传 defines: 预处理只展开了 #if, 源码里可能还有直接引用宏值的地方
    // (如 `float x = MY_SCALE;`), 那些要靠编译期的 -D 才有定义。
    render::DxcCompileOptions compileOptions{
        .EntryPoint = stageDesc.EntryPoint,
        .Stage = stage,
        .SM = seed.ShaderModel,
        .Defines = defines,
        .Includes = includes,
        .IsOptimize = seed.IsOptimize,
        .IsSpirv = false,
        .EnableUnbounded = seed.EnableUnbounded};

    std::optional<render::DxcOutput> dxil = dxc.CompileMemory(sourceCode, sourceLabel, compileOptions);
    if (!dxil.has_value() || dxil->Refl.empty()) {
        warnings.push_back(MakeTemplateDiag(
            fmt::format(
                "skipped a reflection round: compiling '{}' with defines [{}] produced no DXIL reflection",
                sourceLabel,
                fmt::join(defines, ", ")),
            seed.Name,
            stage));
        return true;
    }
    std::optional<render::HlslShaderDesc> hlsl = dxc.GetShaderDescFromOutput(dxil->Refl);
    if (!hlsl.has_value()) {
        outDiag = MakeTemplateDiag("failed to parse the DXIL reflection blob", seed.Name, stage);
        return false;
    }
    if (!AbsorbHlslReflection(
            merged,
            hlsl.value(),
            stage,
            seed.Name,
            options.GenerateVertexInput,
            warnings,
            outDiag)) {
        return false;
    }

    if (!useSpirv) {
        return true;
    }
#if defined(RADRAY_ENABLE_SPIRV_CROSS)
    compileOptions.IsSpirv = true;
    std::optional<render::DxcOutput> spirv = dxc.CompileMemory(sourceCode, sourceLabel, compileOptions);
    if (!spirv.has_value() || spirv->Data.empty()) {
        warnings.push_back(MakeTemplateDiag(
            fmt::format("skipped SPIR-V cross-check: compiling '{}' as SPIR-V failed", sourceLabel),
            seed.Name,
            stage));
        return true;
    }
    // ReflectSpirv 不是 noexcept。按仓库异常政策不在此捕获: 反射失败属于工具链或
    // 输入的不变量破坏, 应在 noexcept 边界终止而非降级为 false。
    std::optional<render::SpirvShaderDesc> spirvDesc = render::ReflectSpirv(render::SpirvBytecodeView{
        .Data = spirv->Data,
        .EntryPointName = stageDesc.EntryPoint,
        .Stage = stage});
    if (!spirvDesc.has_value()) {
        warnings.push_back(MakeTemplateDiag(
            "skipped SPIR-V cross-check: reflection failed", seed.Name, stage));
        return true;
    }
    AbsorbSpirvReflection(merged, spirvDesc.value(), stage, seed.Name, warnings);
#endif
    return true;
}

/// 合并结果 -> manifest 的 pass 声明, 同时产出待确认项。
ShaderPassDesc BuildPass(
    const ShaderTemplatePassSeed& seed,
    MergedPass& merged,
    size_t passIndex,
    bool spirvAvailable,
    vector<ShaderTemplateTodo>& todos) {
    ShaderPassDesc pass{};
    pass.Name = seed.Name;
    pass.Source = seed.Source;
    pass.Stages = seed.Stages;
    pass.ShaderModel = seed.ShaderModel;
    pass.Defines = seed.Defines;
    pass.IsOptimize = seed.IsOptimize;
    pass.EnableUnbounded = seed.EnableUnbounded;

    const string prefix = fmt::format("Passes[{}]", passIndex);

    // push constant 先认领, 使它不再作为普通 cbuffer 出现在 BindingGroups 里。
    // 位置只能来自 DXIL: SPIR-V 的 VkPushConstantRange 不带 (space, register)。
    if (merged.HasPushConstant) {
        auto it = std::find_if(
            merged.Bindings.begin(),
            merged.Bindings.end(),
            [&](const MergedBinding& binding) noexcept {
                return binding.Name == merged.PushConstantName &&
                       binding.Type == render::ShaderParameterBindingType::CBuffer;
            });
        if (it != merged.Bindings.end()) {
            ShaderPushConstantDesc pc{};
            pc.Name = it->Name;
            pc.Location = render::ShaderBindingLocation{it->Group, it->Binding};
            pc.Size = merged.PushConstantSize;
            // stage 取两侧的并: SPIR-V 只在编出 range 的那一轮看到它, 而 DXIL 侧的
            // cbuffer 出现在所有用到它的 stage 上。
            pc.Stages = merged.PushConstantStages | it->Stages;
            pass.PushConstant = std::move(pc);
            merged.Bindings.erase(it);
        }
    }

    std::ranges::sort(merged.Bindings, [](const MergedBinding& a, const MergedBinding& b) noexcept {
        return a.Group != b.Group ? a.Group < b.Group : a.Binding < b.Binding;
    });

    for (const MergedBinding& binding : merged.Bindings) {
        ShaderBindingGroupDesc* group = nullptr;
        for (ShaderBindingGroupDesc& candidate : pass.BindingGroups) {
            if (candidate.Group == binding.Group) {
                group = &candidate;
                break;
            }
        }
        if (group == nullptr) {
            pass.BindingGroups.push_back(ShaderBindingGroupDesc{binding.Group, {}});
            group = &pass.BindingGroups.back();
        }
        ShaderBindingDesc desc{};
        desc.Name = binding.Name;
        desc.Binding = binding.Binding;
        desc.Type = binding.Type;
        desc.Count = binding.Count;
        desc.Stages = binding.Stages;
        desc.Residency = ShaderBindingResidency::DescriptorTable;
        group->Bindings.push_back(std::move(desc));

        if (binding.IsUnbounded) {
            todos.push_back(ShaderTemplateTodo{
                fmt::format("{}.BindingGroups[{}].Bindings['{}'].Count", prefix, binding.Group, binding.Name),
                "reflection only reports 'unbounded'; both backends reject Count == 0, "
                "so the real capacity must be declared here (generated value is 1)"});
        }
        if (binding.Type == render::ShaderParameterBindingType::Sampler) {
            todos.push_back(ShaderTemplateTodo{
                fmt::format(
                    "{}.BindingGroups[{}].Bindings['{}'].ImmutableSampler",
                    prefix,
                    binding.Group,
                    binding.Name),
                "static samplers are a pipeline-layout concept and are absent from the bytecode; "
                "add ImmutableSampler here if this sampler should be baked into the layout"});
        }
    }

    if (!pass.BindingGroups.empty()) {
        todos.push_back(ShaderTemplateTodo{
            fmt::format("{}.BindingGroups[*].Bindings[*].Residency", prefix),
            "every binding was generated as DescriptorTable; promoting one to RootDescriptor "
            "(D3D12 root CBV / Vulkan dynamic descriptor) is a performance decision that "
            "reflection cannot make"});
    }

    if (!merged.Attributes.empty()) {
        std::ranges::sort(merged.Attributes, [](const MergedAttribute& a, const MergedAttribute& b) noexcept {
            return a.Location < b.Location;
        });
        ShaderVertexInputDesc vertexInput{};
        uint32_t offset = 0;
        for (const MergedAttribute& attribute : merged.Attributes) {
            ShaderVertexAttributeDesc desc{};
            desc.Semantic = attribute.Semantic;
            desc.SemanticIndex = attribute.SemanticIndex;
            desc.Format = attribute.Format;
            desc.BufferBinding = 0;
            desc.Offset = offset;
            desc.Location = attribute.Location;
            offset += render::GetVertexFormatSizeInBytes(attribute.Format);
            vertexInput.Attributes.push_back(std::move(desc));
        }
        ShaderVertexBufferDesc buffer{};
        buffer.Binding = 0;
        buffer.ArrayStride = offset;
        buffer.StepMode = render::VertexStepMode::Vertex;
        vertexInput.Buffers.push_back(buffer);
        pass.VertexInput = std::move(vertexInput);

        todos.push_back(ShaderTemplateTodo{
            fmt::format("{}.VertexInput.Attributes[*].Format", prefix),
            "reflection only gives the component type and write mask, so every format was "
            "widened to 32 bits per component; narrow or normalized formats "
            "(UNORM8X4, FLOAT16X2, ...) must be corrected by hand"});
        todos.push_back(ShaderTemplateTodo{
            fmt::format("{}.VertexInput.Buffers / Attributes[*].Offset", prefix),
            "all attributes were packed tightly into a single buffer; the real layout is a "
            "property of the mesh, not the shader, so split buffers and offsets as needed"});
    }

    if (!spirvAvailable && !pass.PushConstant.has_value()) {
        todos.push_back(ShaderTemplateTodo{
            fmt::format("{}.PushConstant", prefix),
            "SPIR-V reflection was unavailable, and DXIL reports a [[vk::push_constant]] cbuffer "
            "as an ordinary CBuffer binding; if this pass has a push constant, move that binding "
            "into PushConstant by hand"});
    }
    return pass;
}

}  // namespace

std::optional<ShaderAssetTemplate> GenerateShaderAssetTemplate(
    render::Dxc& dxc,
    const ShaderTemplateSeed& seed,
    const ShaderTemplateOptions& options,
    ShaderAssetDiagnostic& outDiag) noexcept {
    outDiag = ShaderAssetDiagnostic{};

    if (seed.Passes.empty()) {
        outDiag = MakeTemplateDiag("the seed must declare at least one pass");
        return std::nullopt;
    }

    bool spirvAvailable = options.UseSpirvReflection;
#if !defined(RADRAY_ENABLE_SPIRV_CROSS)
    spirvAvailable = false;
#endif

    ShaderAssetTemplate result{};
    if (options.UseSpirvReflection && !spirvAvailable) {
        result.Warnings.push_back(MakeTemplateDiag(
            "SPIR-V reflection was requested but spirv-cross is not compiled in; "
            "push constants cannot be told apart from ordinary cbuffers"));
    }

    result.Asset.Name = seed.Name;
    result.Asset.Source = seed.Source;

    const string rootString = options.ShaderRoot.string();
    const std::array<std::string_view, 1> includes{rootString};

    for (size_t passIndex = 0; passIndex < seed.Passes.size(); ++passIndex) {
        ShaderTemplatePassSeed passSeed = seed.Passes[passIndex];
        if (passSeed.Name.empty()) {
            passSeed.Name = "main";
        }
        if (passSeed.Stages.empty()) {
            outDiag = MakeTemplateDiag("pass must declare at least one stage", passSeed.Name);
            return std::nullopt;
        }

        const string source = passSeed.Source.empty() ? seed.Source : passSeed.Source;
        if (source.empty()) {
            outDiag = MakeTemplateDiag("pass has no source file", passSeed.Name);
            return std::nullopt;
        }
        const std::filesystem::path sourceFile =
            options.ShaderRoot / std::filesystem::path{source};
        std::error_code error;
        if (!std::filesystem::is_regular_file(sourceFile, error) || error) {
            outDiag = MakeTemplateDiag(
                fmt::format("shader source does not exist: {}", sourceFile.generic_string()),
                passSeed.Name);
            return std::nullopt;
        }

        // 先在"只有 pass.Defines"的默认宏环境下预处理一次, 从中读出 keyword 声明。
        //
        // 【keyword 只认默认轮】: pragma 必须写在无条件位置 (见头文件), 因此默认轮
        // 就能看到全部声明。若改在 probe 轮里收集, 反而会因为某轮的 -D 打开了某个
        // #ifdef 而多出本不该存在的组。
        const std::optional<string> defaultPreprocessed = PreprocessRound(
            dxc, sourceFile, source, passSeed, passSeed.Defines, includes, outDiag);
        if (!defaultPreprocessed.has_value()) {
            return std::nullopt;
        }

        vector<ShaderKeywordGroupDesc> declaredGroups;
        if (options.ParseKeywordPragmas) {
            // 只在 EntryFileOnly 下给出入口路径 —— 留空即表示采纳整条 include 链。
            const string entryFilter =
                options.KeywordPragmaScope == ShaderKeywordPragmaScope::EntryFileOnly
                    ? sourceFile.generic_string()
                    : string{};
            std::optional<vector<ShaderKeywordGroupDesc>> parsed = ParseShaderKeywordPragmas(
                defaultPreprocessed.value(), entryFilter, outDiag);
            if (!parsed.has_value()) {
                outDiag.PassName = passSeed.Name;
                return std::nullopt;
            }
            declaredGroups = std::move(parsed.value());
        }

        // 第一轮是默认变体, 之后每个 probe 组合各一轮。
        // 未显式给 probe 时按声明的 keyword 组【逐组各一轮】自动推导 —— 否则被
        // #ifdef 包住的绑定会静默缺失 (见 ProbeDeclaredKeywords)。
        vector<vector<string>> rounds;
        rounds.push_back({});
        if (!passSeed.ProbeDefineSets.empty()) {
            for (const vector<string>& probe : passSeed.ProbeDefineSets) {
                rounds.push_back(probe);
            }
        } else if (options.ProbeDeclaredKeywords) {
            for (const ShaderKeywordGroupDesc& group : declaredGroups) {
                // 组内 keyword 互斥, 故每个取值各占一轮而非同时开启。
                for (const string& keyword : group.Keywords) {
                    rounds.push_back(vector<string>{keyword});
                }
            }
        }

        MergedPass merged{};
        for (size_t roundIndex = 0; roundIndex < rounds.size(); ++roundIndex) {
            const vector<string>& round = rounds[roundIndex];
            vector<string> defines = passSeed.Defines;
            defines.insert(defines.end(), round.begin(), round.end());
            vector<std::string_view> defineViews;
            defineViews.reserve(defines.size());
            for (const string& define : defines) {
                defineViews.emplace_back(define);
            }

            // 【每轮各预处理一次】: 预处理会按本轮的 -D 求解 #if, 复用默认轮的文本
            // 会把条件分支冻结在默认宏环境上, 使 probe 完全失效。
            string roundCode;
            if (roundIndex == 0) {
                roundCode = StripShaderKeywordPragmas(defaultPreprocessed.value());
            } else {
                const std::optional<string> preprocessed = PreprocessRound(
                    dxc, sourceFile, source, passSeed, defines, includes, outDiag);
                if (!preprocessed.has_value()) {
                    // 预处理失败通常意味着这组宏本身不合法, 与编译失败同性质:
                    // 记一条告警并跳过该轮, 不牵连整份模板。
                    result.Warnings.push_back(MakeTemplateDiag(
                        fmt::format(
                            "skipped a reflection round: preprocessing '{}' with defines [{}] failed",
                            source,
                            fmt::join(defines, ", ")),
                        passSeed.Name));
                    outDiag = ShaderAssetDiagnostic{};
                    continue;
                }
                roundCode = StripShaderKeywordPragmas(preprocessed.value());
            }

            for (const ShaderStageDesc& stageDesc : passSeed.Stages) {
                if (!ProbeOnce(
                        dxc,
                        merged,
                        roundCode,
                        source,
                        passSeed,
                        stageDesc,
                        defineViews,
                        includes,
                        options,
                        spirvAvailable,
                        result.Warnings,
                        outDiag)) {
                    return std::nullopt;
                }
            }
        }

        // KeywordGroups 是资产级字段, 而声明来自各 pass 的源文件。多 pass 共享同一
        // 组名时按名字去重 —— 例如 forward/shadow 两个 pass 都声明 AlphaMode。
        for (ShaderKeywordGroupDesc& group : declaredGroups) {
            const bool exists = std::ranges::any_of(
                result.Asset.KeywordGroups,
                [&](const ShaderKeywordGroupDesc& existing) noexcept {
                    return existing.Name == group.Name;
                });
            if (!exists) {
                result.Asset.KeywordGroups.push_back(std::move(group));
            }
        }

        result.Asset.Passes.push_back(
            BuildPass(passSeed, merged, passIndex, spirvAvailable, result.Todos));
    }

    if (result.Asset.Name.empty()) {
        result.Asset.Name =
            std::filesystem::path{result.Asset.Source}.stem().generic_string();
    }
    // KeywordGroups 已由源码里的 pragma 生成, 故不再进 TODO。但"一条声明都没有"
    // 值得提一句: 可能是作者还没声明, 也可能这个资产本就没有变体。
    if (result.Asset.KeywordGroups.empty()) {
        result.Todos.push_back(ShaderTemplateTodo{
            "KeywordGroups",
            "no '#pragma radray_keyword_group' was found in the entry source, so this asset has "
            "no shader variants; declare the groups in the HLSL itself if that is wrong"});
    }
    result.Todos.push_back(ShaderTemplateTodo{
        "BakeVariants",
        "the bake set is a release decision, not a shader property (the same HLSL may bake "
        "different sets per platform), so reflection cannot infer it; declare here which "
        "combinations to precompile, or leave empty to bake only the default variant"});

    std::ranges::sort(result.Todos, [](const ShaderTemplateTodo& a, const ShaderTemplateTodo& b) noexcept {
        return a.Path < b.Path;
    });

    // 生成的模板必须自洽: 它同时要通过 manifest 自校验 (ValidateAsset) 才算可用。
    // 走一趟序列化 + ParseShaderAssetDesc 是最诚实的检查 —— 它与作者拿到文件后
    // 真正经历的路径完全相同, 且能给出精确的诊断 (例如同一 group 里 b0 与 t0 撞号,
    // 这是本 ABI 刻意禁止的, 属真实需要作者处理的问题)。
    std::optional<string> json = SerializeShaderAssetDesc(result.Asset);
    if (!json.has_value()) {
        outDiag = MakeTemplateDiag("failed to serialize the generated template");
        return std::nullopt;
    }
    if (!ParseShaderAssetDesc(json.value(), outDiag).has_value()) {
        return std::nullopt;
    }
    return result;
}

#endif

// ============================ 序列化 ============================

/// ShaderTemplateTodo 不属于 manifest schema, 故不在 shader_manifest.h 的 codec 声明表
/// 里 —— 它只在本 TU 被序列化, 且刻意没有 Deserializer: "_TODO" 是给人看的单向附注,
/// 回读时应当被忽略而非解析。
template <>
struct JsonSerializer<ShaderTemplateTodo> {
    static bool Write(JsonWriteContext& context, const ShaderTemplateTodo& value) noexcept {
        JsonObjectWriter object = context.BeginObject();
        return object.IsValid() &&
               object.Member("Path", value.Path) &&
               object.Member("Reason", value.Reason);
    }
};

std::optional<string> SerializeShaderAssetTemplate(
    const ShaderAssetTemplate& value,
    bool pretty) noexcept {
    JsonWriter writer;
    if (!writer.IsValid()) {
        return std::nullopt;
    }
    JsonWriteContext context{writer};
    JsonObjectWriter object = context.BeginObject();
    if (!WriteShaderAssetMembers(object, value.Asset)) {
        return std::nullopt;
    }
    // 放在 manifest 字段之后: 先读到的是真正的声明, TODO 是附注。
    if (!value.Todos.empty() && !object.Member(kTodoKey, value.Todos)) {
        return std::nullopt;
    }
    return writer.Write(pretty);
}

}  // namespace radray
