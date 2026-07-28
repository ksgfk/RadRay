#include <radray/shader/shader_manifest.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <type_traits>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <radray/basic_math.h>
#include <radray/binary_io.h>
#include <radray/enum_flags.h>
#include <radray/file.h>
#include <radray/json.h>
#include <radray/logger.h>
#include <radray/shader/dxc.h>

#if defined(RADRAY_ENABLE_SHADER_JIT) && defined(RADRAY_ENABLE_SPIRV_CROSS)
#include <radray/shader/spvc.h>
#endif

#include "shader_manifest_json.h"
#include "shader_reflection_map.h"

namespace radray {

// 文件组织: 先是全部内部实现细节 (单个匿名 namespace), 然后按
// 数据类型成员 -> 功能类成员 -> 自由函数 的顺序给出公开定义,
// 与 shader_manifest.h 的声明顺序一致。

namespace {

/// 诊断文本用的 stage 名。与序列化表同源, 保证 JSON 里的名字和报错里的名字一致。
std::string_view StageName(render::ShaderStage stage) noexcept {
    return EnumNameOr(stage, "UNKNOWN");
}

// ============================ 解析辅助 ============================

/// 解析期错误累积器。第一个错误即终止, 保留完整上下文。
class ParseScope {
public:
    explicit ParseScope(ShaderAssetDiagnostic& diag) noexcept : _diag(diag) {}

    bool Failed() const noexcept { return _failed; }

    /// 记录错误并返回 false, 便于 `return scope.Fail(...)`。
    bool Fail(string message) noexcept {
        if (!_failed) {
            _failed = true;
            _diag.Message = std::move(message);
            _diag.PassName = _passName;
            _diag.BindingName = _bindingName;
            _diag.Group = _group;
            _diag.Binding = _binding;
            _diag.Stage = _stage;
        }
        return false;
    }

    void SetPass(std::string_view name) noexcept { _passName = string{name}; }
    void SetBinding(std::string_view name) noexcept { _bindingName = string{name}; }
    void SetGroup(std::optional<uint32_t> group) noexcept { _group = group; }
    void SetBindingIndex(std::optional<uint32_t> binding) noexcept { _binding = binding; }
    void SetStage(std::optional<render::ShaderStage> stage) noexcept { _stage = stage; }

    void ClearBinding() noexcept {
        _bindingName.clear();
        _binding.reset();
    }

private:
    ShaderAssetDiagnostic& _diag;
    bool _failed{false};
    string _passName;
    string _bindingName;
    std::optional<uint32_t> _group{};
    std::optional<uint32_t> _binding{};
    std::optional<render::ShaderStage> _stage{};
};

// ============================ 校验 ============================

/// D3D12 root signature 的硬上限 (D3D12_MAX_ROOT_COST)。
constexpr uint32_t kMaxRootDwords = 64;

/// 该绑定类型是否允许做 root descriptor。
bool IsRootDescriptorCapable(render::ShaderParameterBindingType type) noexcept {
    switch (type) {
        case render::ShaderParameterBindingType::CBuffer:
        case render::ShaderParameterBindingType::Buffer:
        case render::ShaderParameterBindingType::RWBuffer:
            return true;
        default:
            return false;
    }
}

bool ValidateBinding(const ShaderBindingDesc& binding, ParseScope& scope) noexcept {
    scope.SetBinding(binding.Name);
    scope.SetBindingIndex(binding.Binding);

    if (binding.Name.empty()) {
        return scope.Fail("binding Name must not be empty");
    }
    if (binding.Type == render::ShaderParameterBindingType::UNKNOWN) {
        return scope.Fail("binding Type must be declared");
    }
    if (render::IsDynamicShaderParameterBindingType(binding.Type)) {
        return scope.Fail(fmt::format(
            "binding Type {} is internal; declare the base Type and use RootDescriptor Residency",
            EnumNameOr(binding.Type, "UNKNOWN")));
    }
    // 两个后端都拒绝 Count == 0, unbounded 数组必须在 manifest 里给出实际容量。
    if (binding.Count == 0) {
        return scope.Fail("binding Count must be at least 1 (unbounded arrays must declare a capacity)");
    }
    if (binding.Count - 1 > std::numeric_limits<uint32_t>::max() - binding.Binding) {
        return scope.Fail(fmt::format(
            "binding register range overflows: Binding {} Count {}",
            binding.Binding,
            binding.Count));
    }
    if (binding.Stages == render::ShaderStages{render::ShaderStage::UNKNOWN}) {
        return scope.Fail("binding Stages must list at least one stage");
    }
    if (binding.Residency == ShaderBindingResidency::RootDescriptor) {
        if (!IsRootDescriptorCapable(binding.Type)) {
            return scope.Fail(fmt::format(
                "RootDescriptor residency requires CBuffer / Buffer / RWBuffer, got {}",
                EnumNameOr(binding.Type, "UNKNOWN")));
        }
        if (binding.Count != 1) {
            return scope.Fail(fmt::format("RootDescriptor residency requires Count 1, got {}", binding.Count));
        }
    }
    if (binding.ImmutableSampler.has_value()) {
        const render::SamplerDescriptor& sampler = binding.ImmutableSampler.value();
        if (binding.Type != render::ShaderParameterBindingType::Sampler) {
            return scope.Fail("ImmutableSampler requires Sampler type");
        }
        if (binding.Count != 1) {
            return scope.Fail(fmt::format("ImmutableSampler requires Count 1, got {}", binding.Count));
        }
        if (sampler.LodMax < sampler.LodMin) {
            return scope.Fail(fmt::format(
                "ImmutableSampler LodMax {} is less than LodMin {}",
                sampler.LodMax,
                sampler.LodMin));
        }
    }
    return true;
}

/// 估算该 group 占用的 root DWORD 数, 规则与 D3D12 后端一致:
/// root descriptor 2 DWORD, 资源表 1 DWORD, 采样器表 1 DWORD。
uint32_t EstimateGroupRootDwords(const ShaderBindingGroupDesc& group) noexcept {
    uint32_t dwords = 0;
    bool hasResourceTable = false;
    bool hasSamplerTable = false;
    for (const ShaderBindingDesc& binding : group.Bindings) {
        if (binding.Residency == ShaderBindingResidency::RootDescriptor) {
            dwords += 2;
        } else if (binding.Type == render::ShaderParameterBindingType::Sampler) {
            hasSamplerTable |= !binding.ImmutableSampler.has_value();
        } else {
            hasResourceTable = true;
        }
    }
    dwords += hasResourceTable ? 1u : 0u;
    dwords += hasSamplerTable ? 1u : 0u;
    return dwords;
}

bool ValidatePass(const ShaderPassDesc& pass, ParseScope& scope) noexcept {
    scope.SetPass(pass.Name);
    scope.SetGroup(std::nullopt);
    scope.ClearBinding();

    if (pass.Stages.empty()) {
        return scope.Fail("pass must declare at least one stage");
    }
    render::ShaderStages seen{render::ShaderStage::UNKNOWN};
    for (const ShaderStageDesc& stage : pass.Stages) {
        if (stage.Stage == render::ShaderStage::UNKNOWN) {
            return scope.Fail("stage must declare a known Stage");
        }
        if (stage.EntryPoint.empty()) {
            scope.SetStage(stage.Stage);
            return scope.Fail("stage EntryPoint must not be empty");
        }
        if (seen.HasFlag(stage.Stage)) {
            return scope.Fail(fmt::format("stage {} declared more than once", StageName(stage.Stage)));
        }
        seen |= stage.Stage;
    }
    const bool hasCompute = seen.HasFlag(render::ShaderStage::Compute);
    const bool hasGraphics =
        seen.HasFlag(render::ShaderStage::Vertex) || seen.HasFlag(render::ShaderStage::Pixel);
    if (hasCompute && hasGraphics) {
        return scope.Fail("pass must not mix Compute with graphics stages");
    }
    if (hasGraphics && !seen.HasFlag(render::ShaderStage::Vertex)) {
        return scope.Fail("graphics pass must declare a Vertex stage");
    }
    if (hasCompute && pass.VertexInput.has_value()) {
        return scope.Fail("compute pass must not declare VertexInput");
    }

    const render::ShaderStages passStages = seen;

    // ---- binding group ----
    vector<uint32_t> groupIndices;
    groupIndices.reserve(pass.BindingGroups.size());
    uint32_t rootDwords = 0;
    for (const ShaderBindingGroupDesc& group : pass.BindingGroups) {
        scope.SetGroup(group.Group);
        scope.ClearBinding();
        if (std::find(groupIndices.begin(), groupIndices.end(), group.Group) != groupIndices.end()) {
            return scope.Fail(fmt::format("duplicate binding group {}", group.Group));
        }
        groupIndices.push_back(group.Group);
        if (group.Bindings.empty()) {
            return scope.Fail("binding group must declare at least one binding");
        }
        for (size_t i = 0; i < group.Bindings.size(); ++i) {
            const ShaderBindingDesc& binding = group.Bindings[i];
            if (!ValidateBinding(binding, scope)) {
                return false;
            }
            // ValidateBinding 已保证 Stages 非 UNKNOWN, 此处只需检查子集关系。
            if ((binding.Stages & passStages) != binding.Stages) {
                return scope.Fail("binding Stages reference a stage the pass does not declare");
            }
            // 同 group 内 binding 号必须跨寄存器类别唯一。
            // D3D12 不要求 (b/t/s/u 是独立命名空间), Vulkan 要求 (一个 set 内 binding
            // 号全局唯一)。在此强制, 可在编译前抓住"DX 能跑、VK 炸掉"的一整类 bug。
            for (size_t j = 0; j < i; ++j) {
                const ShaderBindingDesc& other = group.Bindings[j];
                const uint32_t aLast = binding.Binding + binding.Count - 1;
                const uint32_t bLast = other.Binding + other.Count - 1;
                if (binding.Binding <= bLast && other.Binding <= aLast) {
                    return scope.Fail(fmt::format(
                        "binding range [{}, {}] overlaps '{}' range [{}, {}] in the same group; "
                        "b/t/s/u share one binding number space because Vulkan requires it",
                        binding.Binding,
                        aLast,
                        other.Name,
                        other.Binding,
                        bLast));
                }
                if (binding.Name == other.Name) {
                    return scope.Fail(fmt::format("duplicate binding name '{}' in group", binding.Name));
                }
            }
        }
        const uint32_t groupDwords = EstimateGroupRootDwords(group);
        if (groupDwords > kMaxRootDwords - rootDwords) {
            return scope.Fail(fmt::format(
                "pass exceeds the {} DWORD root signature budget", kMaxRootDwords));
        }
        rootDwords += groupDwords;
    }

    // 跨 group 的绑定名唯一 (未来 material 按名寻址的前提)。
    for (size_t gi = 0; gi < pass.BindingGroups.size(); ++gi) {
        for (size_t gj = gi + 1; gj < pass.BindingGroups.size(); ++gj) {
            for (const ShaderBindingDesc& a : pass.BindingGroups[gi].Bindings) {
                for (const ShaderBindingDesc& b : pass.BindingGroups[gj].Bindings) {
                    if (a.Name == b.Name) {
                        scope.SetGroup(pass.BindingGroups[gj].Group);
                        scope.SetBinding(b.Name);
                        scope.SetBindingIndex(b.Binding);
                        return scope.Fail(fmt::format(
                            "binding name '{}' is declared in both group {} and group {}",
                            a.Name,
                            pass.BindingGroups[gi].Group,
                            pass.BindingGroups[gj].Group));
                    }
                }
            }
        }
    }

    // ---- push constant ----
    scope.SetGroup(std::nullopt);
    scope.ClearBinding();
    if (pass.PushConstant.has_value()) {
        const ShaderPushConstantDesc& pc = pass.PushConstant.value();
        scope.SetBinding(pc.Name);
        scope.SetGroup(pc.Location.Group);
        scope.SetBindingIndex(pc.Location.Binding);
        if (pc.Name.empty()) {
            return scope.Fail("PushConstant Name must not be empty");
        }
        if (pc.Size == 0 || pc.Size % 4 != 0) {
            return scope.Fail(fmt::format("PushConstant Size must be non-zero and 4-byte aligned, got {}", pc.Size));
        }
        if (pc.Stages == render::ShaderStages{render::ShaderStage::UNKNOWN}) {
            return scope.Fail("PushConstant Stages must list at least one stage");
        }
        if ((pc.Stages & passStages) != pc.Stages) {
            return scope.Fail("PushConstant Stages reference a stage the pass does not declare");
        }
        const uint32_t pcDwords = pc.Size / 4;
        if (pcDwords > kMaxRootDwords - rootDwords) {
            return scope.Fail(fmt::format(
                "pass exceeds the {} DWORD root signature budget", kMaxRootDwords));
        }
        // D3D12 侧 push constant 与普通绑定共享 (space, register), 必须不撞。
        Nullable<const ShaderBindingDesc*> clash = pass.FindBinding(pc.Location.Group, pc.Location.Binding);
        if (clash.HasValue()) {
            return scope.Fail(fmt::format(
                "PushConstant location (group {}, binding {}) collides with binding '{}'",
                pc.Location.Group,
                pc.Location.Binding,
                clash.Unwrap()->Name));
        }
        for (const ShaderBindingGroupDesc& group : pass.BindingGroups) {
            if (group.Group != pc.Location.Group) {
                continue;
            }
            for (const ShaderBindingDesc& binding : group.Bindings) {
                if (binding.Name == pc.Name) {
                    return scope.Fail(fmt::format("PushConstant name '{}' collides with a binding name", pc.Name));
                }
            }
        }
    }

    // ---- vertex input ----
    scope.SetGroup(std::nullopt);
    scope.ClearBinding();
    if (pass.VertexInput.has_value()) {
        const ShaderVertexInputDesc& vi = pass.VertexInput.value();
        if (vi.Buffers.empty()) {
            return scope.Fail("VertexInput must declare at least one buffer");
        }
        if (vi.Attributes.empty()) {
            return scope.Fail("VertexInput must declare at least one attribute");
        }
        for (size_t i = 0; i < vi.Buffers.size(); ++i) {
            const ShaderVertexBufferDesc& buffer = vi.Buffers[i];
            if (buffer.ArrayStride == 0) {
                return scope.Fail(fmt::format("VertexInput buffer {} ArrayStride must be non-zero", buffer.Binding));
            }
            for (size_t j = 0; j < i; ++j) {
                if (vi.Buffers[j].Binding == buffer.Binding) {
                    return scope.Fail(fmt::format("duplicate VertexInput buffer binding {}", buffer.Binding));
                }
            }
        }
        vector<uint32_t> locations;
        locations.reserve(vi.Attributes.size());
        for (size_t i = 0; i < vi.Attributes.size(); ++i) {
            const ShaderVertexAttributeDesc& attribute = vi.Attributes[i];
            scope.SetBinding(attribute.Semantic);
            if (attribute.Semantic.empty()) {
                return scope.Fail("VertexInput attribute Semantic must not be empty");
            }
            if (attribute.Format == render::VertexFormat::UNKNOWN) {
                return scope.Fail("VertexInput attribute Format must be declared");
            }
            auto buffer = std::find_if(
                vi.Buffers.begin(),
                vi.Buffers.end(),
                [&](const ShaderVertexBufferDesc& b) noexcept { return b.Binding == attribute.BufferBinding; });
            if (buffer == vi.Buffers.end()) {
                return scope.Fail(fmt::format(
                    "VertexInput attribute references undeclared buffer binding {}",
                    attribute.BufferBinding));
            }
            const uint32_t size = render::GetVertexFormatSizeInBytes(attribute.Format);
            if (size == 0) {
                return scope.Fail("VertexInput attribute has unsupported Format");
            }
            if (attribute.Offset > buffer->ArrayStride - size ||
                attribute.Offset + size > buffer->ArrayStride) {
                return scope.Fail(fmt::format(
                    "VertexInput attribute at offset {} size {} exceeds buffer {} ArrayStride {}",
                    attribute.Offset,
                    size,
                    buffer->Binding,
                    buffer->ArrayStride));
            }
            const uint32_t location = attribute.Location.value_or(static_cast<uint32_t>(i));
            if (std::find(locations.begin(), locations.end(), location) != locations.end()) {
                return scope.Fail(fmt::format("duplicate VertexInput attribute location {}", location));
            }
            locations.push_back(location);
            for (size_t j = 0; j < i; ++j) {
                if (vi.Attributes[j].Semantic == attribute.Semantic &&
                    vi.Attributes[j].SemanticIndex == attribute.SemanticIndex) {
                    return scope.Fail(fmt::format(
                        "duplicate VertexInput semantic {}{}",
                        attribute.Semantic,
                        attribute.SemanticIndex));
                }
            }
        }
    }
    scope.ClearBinding();
    return true;
}

/// 找 keyword 属于哪个组。找不到返回 nullptr。
Nullable<const ShaderKeywordGroupDesc*> FindKeywordOwner(
    const ShaderAssetDesc& desc,
    std::string_view keyword) noexcept {
    for (const ShaderKeywordGroupDesc& group : desc.KeywordGroups) {
        if (std::ranges::find(group.Keywords, keyword) != group.Keywords.end()) {
            return &group;
        }
    }
    return nullptr;
}

/// 烘焙声明的结构合法性。
///
/// pass 非空表示这是 pass 自己写的规则, 此时 Expand 的组名还要属于该 pass 的组集合;
/// pass 为空表示这是资产级规则, 只按资产全域核对 —— 继承到某个 pass 后组名可能
/// 不适用, 那是 ExpandShaderBakeSet 静默投影的事, 不是错误。
bool ValidateBakeSet(
    const ShaderAssetDesc& desc,
    const ShaderBakeSetDesc& bake,
    Nullable<const ShaderPassDesc*> pass,
    ParseScope& scope) noexcept {
    for (size_t i = 0; i < bake.Rules.size(); ++i) {
        const ShaderBakeRuleDesc& rule = bake.Rules[i];
        const bool hasExpand = !rule.Expand.empty();
        const bool hasCombination = !rule.Combination.empty();
        if (hasExpand == hasCombination) {
            return scope.Fail(fmt::format(
                "BakeVariants Rules[{}] must declare exactly one of 'Expand' or 'Combination'",
                i));
        }

        for (size_t j = 0; j < rule.Expand.size(); ++j) {
            const string& name = rule.Expand[j];
            for (size_t k = 0; k < j; ++k) {
                if (rule.Expand[k] == name) {
                    return scope.Fail(fmt::format(
                        "BakeVariants Rules[{}] lists keyword group '{}' more than once",
                        i,
                        name));
                }
            }
            const auto it = std::ranges::find_if(desc.KeywordGroups, [&](const ShaderKeywordGroupDesc& g) {
                return g.Name == name;
            });
            if (it == desc.KeywordGroups.end()) {
                return scope.Fail(fmt::format(
                    "BakeVariants Rules[{}] expands unknown keyword group '{}'",
                    i,
                    name));
            }
            if (pass.HasValue()) {
                const ShaderPassDesc& p = *pass.Unwrap();
                if (!p.KeywordGroups.empty() &&
                    std::ranges::find(p.KeywordGroups, name) == p.KeywordGroups.end()) {
                    return scope.Fail(fmt::format(
                        "BakeVariants Rules[{}] expands keyword group '{}', "
                        "which this pass does not include",
                        i,
                        name));
                }
            }
        }

        // Combination 是作者点名的精确组合, 故未知 keyword 与同组多选始终是错误,
        // 与继承无关 —— 无论在哪个 pass 上, 这条组合都表达不出合法变体。
        for (size_t j = 0; j < rule.Combination.size(); ++j) {
            const string& keyword = rule.Combination[j];
            Nullable<const ShaderKeywordGroupDesc*> owner = FindKeywordOwner(desc, keyword);
            if (!owner.HasValue()) {
                return scope.Fail(fmt::format(
                    "BakeVariants Rules[{}] uses unknown keyword '{}'",
                    i,
                    keyword));
            }
            for (size_t k = 0; k < j; ++k) {
                if (rule.Combination[k] == keyword) {
                    return scope.Fail(fmt::format(
                        "BakeVariants Rules[{}] lists keyword '{}' more than once",
                        i,
                        keyword));
                }
                Nullable<const ShaderKeywordGroupDesc*> other =
                    FindKeywordOwner(desc, rule.Combination[k]);
                if (other.HasValue() && other.Unwrap() == owner.Unwrap()) {
                    return scope.Fail(fmt::format(
                        "BakeVariants Rules[{}] selects both '{}' and '{}' from the "
                        "mutually exclusive group '{}'",
                        i,
                        rule.Combination[k],
                        keyword,
                        owner.Unwrap()->Name));
                }
            }
        }
    }

    for (size_t i = 0; i < bake.Skip.size(); ++i) {
        const vector<string>& entry = bake.Skip[i];
        // 单个 keyword 的 skip 是表达错误: 想排除一个 keyword 应当把它所在的组从
        // Expand 里去掉, 而不是先展开再剔除。
        if (entry.size() < 2) {
            return scope.Fail(fmt::format(
                "BakeVariants Skip[{}] must list at least two keywords; "
                "to exclude a single keyword, drop its group from Expand instead",
                i));
        }
        for (const string& keyword : entry) {
            if (!FindKeywordOwner(desc, keyword).HasValue()) {
                return scope.Fail(fmt::format(
                    "BakeVariants Skip[{}] uses unknown keyword '{}'",
                    i,
                    keyword));
            }
        }
    }
    return true;
}

/// pass 与资产级 keyword 组的一致性。需要同时看 asset 与 pass, 故不在 ValidatePass 内。
bool ValidatePassKeywords(
    const ShaderAssetDesc& desc,
    const ShaderPassDesc& pass,
    ParseScope& scope) noexcept {
    scope.SetPass(pass.Name);

    // pass.Defines 里出现 keyword 意味着该 keyword 恒开, 变体维度是死的 ——
    // 这总是配置错误, 而非某种"强制开启"的表达方式。
    for (const string& define : pass.Defines) {
        if (define.empty()) {
            return scope.Fail("pass Defines must not contain an empty string");
        }
        // 允许 FOO=1 形式, 故只比对 '=' 之前的名字。
        const std::string_view name = std::string_view{define}.substr(0, define.find('='));
        for (const ShaderKeywordGroupDesc& group : desc.KeywordGroups) {
            if (std::ranges::find(group.Keywords, name) != group.Keywords.end()) {
                return scope.Fail(fmt::format(
                    "pass Defines contains '{}', which is a keyword of group '{}'; "
                    "an unconditional define would pin that variant dimension",
                    name,
                    group.Name));
            }
        }
    }

    const render::ShaderStages passStages = pass.GetStageMask();
    for (size_t i = 0; i < pass.KeywordGroups.size(); ++i) {
        const string& name = pass.KeywordGroups[i];
        for (size_t j = 0; j < i; ++j) {
            if (pass.KeywordGroups[j] == name) {
                return scope.Fail(fmt::format("pass lists keyword group '{}' more than once", name));
            }
        }
        const auto it = std::ranges::find_if(desc.KeywordGroups, [&](const ShaderKeywordGroupDesc& g) {
            return g.Name == name;
        });
        if (it == desc.KeywordGroups.end()) {
            return scope.Fail(fmt::format("pass references unknown keyword group '{}'", name));
        }
        // 组的 Stages 与本 pass 的 stage 无交集时该组不产生任何宏, 属声明了却不
        // 起作用。报错而非静默忽略: 静默会让作者误以为变体已生效。
        if ((it->Stages.value() & passStages.value()) == 0) {
            return scope.Fail(fmt::format(
                "keyword group '{}' targets stages that this pass does not declare",
                name));
        }
    }

    // 烘焙声明。asset 级的规则在这里【不】按 pass 核对组名 —— 继承是不对称的,
    // 未知组由 ExpandShaderBakeSet 静默投影掉 (见 ShaderAssetDesc::BakeVariants)。
    // 但 asset 级规则本身的结构合法性仍要校验, 那部分在 ValidateBakeSet 里按
    // 资产全域做, 由 ValidateAsset 调用一次。
    if (!ValidateBakeSet(desc, pass.BakeVariants, &pass, scope)) {
        return false;
    }
    return true;
}

bool ValidateAsset(const ShaderAssetDesc& desc, ParseScope& scope) noexcept {
    if (desc.Name.empty()) {
        return scope.Fail("asset Name must not be empty");
    }
    if (desc.Passes.empty()) {
        return scope.Fail("asset must declare at least one pass");
    }
    for (size_t i = 0; i < desc.KeywordGroups.size(); ++i) {
        const ShaderKeywordGroupDesc& group = desc.KeywordGroups[i];
        if (group.Name.empty()) {
            return scope.Fail("keyword group Name must not be empty");
        }
        if (group.Keywords.empty()) {
            return scope.Fail(fmt::format("keyword group '{}' must declare at least one keyword", group.Name));
        }
        // ShaderVariantKey 用 uint16_t 存组内下标, kShaderKeywordOff 是保留值。
        // 这是编码约束, 不是政策上限 —— keyword 总数与组数都不受限。
        if (group.Keywords.size() >= kShaderKeywordOff) {
            return scope.Fail(fmt::format(
                "keyword group '{}' declares {} keywords, which exceeds the encoding limit of {}",
                group.Name,
                group.Keywords.size(),
                kShaderKeywordOff - 1));
        }
        if (group.Stages == render::ShaderStages{render::ShaderStage::UNKNOWN}) {
            return scope.Fail(fmt::format("keyword group '{}' Stages must list at least one stage", group.Name));
        }
        for (size_t j = 0; j < i; ++j) {
            if (desc.KeywordGroups[j].Name == group.Name) {
                return scope.Fail(fmt::format("duplicate keyword group name '{}'", group.Name));
            }
        }
        for (size_t k = 0; k < group.Keywords.size(); ++k) {
            const string& keyword = group.Keywords[k];
            if (keyword.empty()) {
                return scope.Fail(fmt::format(
                    "keyword group '{}' must not list an empty keyword; use IsOptional to allow all-off",
                    group.Name));
            }
            for (size_t l = 0; l < k; ++l) {
                if (group.Keywords[l] == keyword) {
                    return scope.Fail(fmt::format("duplicate keyword '{}' in group '{}'", keyword, group.Name));
                }
            }
        }
        // keyword 与其他组的 keyword 不可重名 (否则宏投影歧义)。
        for (size_t j = 0; j < i; ++j) {
            for (const string& keyword : group.Keywords) {
                const vector<string>& other = desc.KeywordGroups[j].Keywords;
                if (std::find(other.begin(), other.end(), keyword) != other.end()) {
                    return scope.Fail(fmt::format(
                        "keyword '{}' appears in both group '{}' and group '{}'",
                        keyword,
                        desc.KeywordGroups[j].Name,
                        group.Name));
                }
            }
        }
    }
    // 资产级烘焙声明按资产全域核对, 不绑定任何 pass。
    if (!ValidateBakeSet(desc, desc.BakeVariants, nullptr, scope)) {
        return false;
    }
    for (size_t i = 0; i < desc.Passes.size(); ++i) {
        const ShaderPassDesc& pass = desc.Passes[i];
        if (pass.Name.empty()) {
            return scope.Fail("pass Name must not be empty");
        }
        for (size_t j = 0; j < i; ++j) {
            if (desc.Passes[j].Name == pass.Name) {
                scope.SetPass(pass.Name);
                return scope.Fail(fmt::format("duplicate pass name '{}'", pass.Name));
            }
        }
        if (pass.Source.empty() && desc.Source.empty()) {
            scope.SetPass(pass.Name);
            return scope.Fail("pass has no Source and the asset declares no default Source");
        }
        if (!ValidatePassKeywords(desc, pass, scope)) {
            return false;
        }
        if (!ValidatePass(pass, scope)) {
            return false;
        }
    }
    scope.SetPass({});
    return true;
}

// ============================ 反射映射 ============================
//
// 反射 -> RHI 的类型映射与 semantic 归一化在 shader_reflection_map.h, 与
// GenerateShaderAssetTemplate 共用: 生成器必须用与校验器完全相同的折叠规则,
// 否则生成出的模板会当场通不过校验。

/// 声明 ⊇ 反射: 逐条核对反射结果能在 manifest 里找到相容的声明。
bool MatchReflectedBindings(
    const ShaderPassDesc& pass,
    render::ShaderStage stage,
    std::span<const ReflectedBinding> reflected,
    ParseScope& scope) noexcept {
    for (const ReflectedBinding& item : reflected) {
        scope.SetGroup(item.Group);
        scope.SetBinding(item.Name);
        scope.SetBindingIndex(item.Binding);

        // push constant 在 DXIL 反射里表现为普通 cbuffer, 先按位置认领。
        if (pass.PushConstant.has_value()) {
            const ShaderPushConstantDesc& pc = pass.PushConstant.value();
            if (pc.Location.Group == item.Group && pc.Location.Binding == item.Binding) {
                if (item.Type != render::ShaderParameterBindingType::CBuffer) {
                    return scope.Fail(fmt::format(
                        "reflection reports non-cbuffer at push constant location (group {}, binding {})",
                        item.Group,
                        item.Binding));
                }
                if (item.Name != pc.Name) {
                    return scope.Fail(fmt::format(
                        "push constant name mismatch: manifest '{}' vs reflection '{}'",
                        pc.Name,
                        item.Name));
                }
                if (!pc.Stages.HasFlag(stage)) {
                    return scope.Fail(fmt::format(
                        "reflection uses push constant in stage {} which the manifest does not declare",
                        StageName(stage)));
                }
                continue;
            }
        }

        Nullable<const ShaderBindingDesc*> declaredOpt = pass.FindBinding(item.Group, item.Binding);
        if (!declaredOpt.HasValue()) {
            return scope.Fail(fmt::format(
                "reflection reports binding '{}' at (group {}, binding {}) that the manifest does not declare",
                item.Name,
                item.Group,
                item.Binding));
        }
        const ShaderBindingDesc& declared = *declaredOpt.Unwrap();
        if (declared.Name != item.Name) {
            return scope.Fail(fmt::format(
                "binding name mismatch at (group {}, binding {}): manifest '{}' vs reflection '{}'",
                item.Group,
                item.Binding,
                declared.Name,
                item.Name));
        }
        if (item.Type != render::ShaderParameterBindingType::UNKNOWN && declared.Type != item.Type) {
            return scope.Fail(fmt::format(
                "binding type mismatch for '{}': manifest {} vs reflection {}",
                declared.Name,
                EnumNameOr(declared.Type, "UNKNOWN"),
                EnumNameOr(item.Type, "UNKNOWN")));
        }
        // 反射 Count 为 0 表示 unbounded, manifest 的容量即为权威值。
        if (item.Count != 0 && item.Count != declared.Count) {
            return scope.Fail(fmt::format(
                "binding count mismatch for '{}': manifest {} vs reflection {}",
                declared.Name,
                declared.Count,
                item.Count));
        }
        if (!declared.Stages.HasFlag(stage)) {
            return scope.Fail(fmt::format(
                "reflection uses binding '{}' in stage {} which the manifest does not declare",
                declared.Name,
                StageName(stage)));
        }
    }
    scope.SetGroup(std::nullopt);
    scope.ClearBinding();
    return true;
}

// ============================ 哈希 ============================
//
// 自己实现 FNV-1a 而不用 radray::HashCode: 后者按 size_t 宽度分派 (32/64 位平台
// 结果不同), 而 artifact key 必须跨平台稳定 —— cook 机器与运行机器可能不同。

constexpr uint64_t kFnvOffset64 = 14695981039346656037ull;
constexpr uint64_t kFnvPrime64 = 1099511628211ull;

/// 第二通道用不同 offset basis, 得到独立的 64 位, 合成 128 位。
constexpr uint64_t kFnvOffset64Alt = 14695981039346656037ull ^ 0x9e3779b97f4a7c15ull;

struct HashAccum {
    uint64_t Low{kFnvOffset64};
    uint64_t High{kFnvOffset64Alt};

    void Byte(uint8_t value) noexcept {
        Low = (Low ^ value) * kFnvPrime64;
        High = (High ^ (value + 0x9eu)) * kFnvPrime64;
    }

    void Bytes(std::span<const byte> data) noexcept {
        for (byte b : data) {
            Byte(static_cast<uint8_t>(b));
        }
    }

    /// 长度前缀避免 ("ab","c") 与 ("a","bc") 撞值。
    void Text(std::string_view text) noexcept {
        U64(text.size());
        for (char c : text) {
            Byte(static_cast<uint8_t>(c));
        }
    }

    void U64(uint64_t value) noexcept {
        for (int i = 0; i < 8; ++i) {
            Byte(static_cast<uint8_t>((value >> (i * 8)) & 0xffu));
        }
    }

    void U32(uint32_t value) noexcept { U64(value); }

    void Hash(ShaderHash value) noexcept {
        U64(value.Low);
        U64(value.High);
    }

    ShaderHash Finish() const noexcept { return ShaderHash{Low, High}; }
};

/// blob 容器头部的 magic。
constexpr std::array<char, 8> kBlobMagic{'R', 'A', 'D', 'S', 'B', 'L', 'B', '1'};

// ============================ 源码身份扫描 ============================

/// 把注释替换成等长空白, 保持偏移不变以便逐行扫描。
/// 需要它是因为被注释掉的 #include 不应计入依赖。
string StripComments(std::string_view source) {
    enum class State : uint8_t {
        Normal,
        LineComment,
        BlockComment,
        StringLiteral,
    };
    State state = State::Normal;
    string result(source.size(), ' ');
    bool escaped = false;
    for (size_t i = 0; i < source.size(); ++i) {
        const char current = source[i];
        const char next = i + 1 < source.size() ? source[i + 1] : '\0';
        if (current == '\n' || current == '\r') {
            result[i] = current;
            if (state == State::LineComment) {
                state = State::Normal;
            }
            escaped = false;
            continue;
        }
        switch (state) {
            case State::Normal:
                if (current == '/' && next == '/') {
                    state = State::LineComment;
                    ++i;
                } else if (current == '/' && next == '*') {
                    state = State::BlockComment;
                    ++i;
                } else {
                    result[i] = current;
                    if (current == '"') {
                        state = State::StringLiteral;
                    }
                }
                break;
            case State::LineComment:
                break;
            case State::BlockComment:
                if (current == '*' && next == '/') {
                    state = State::Normal;
                    ++i;
                }
                break;
            case State::StringLiteral:
                result[i] = current;
                if (escaped) {
                    escaped = false;
                } else if (current == '\\') {
                    escaped = true;
                } else if (current == '"') {
                    state = State::Normal;
                }
                break;
        }
    }
    return result;
}

/// 扫出所有 #include 的目标。
///
/// 刻意【不】求解 #if / #ifdef: 被条件排除的 include 也计入依赖。这使身份变成
/// 过度近似 (改了一个当前用不到的头也会失效), 方向是安全的 —— 漏失效会让运行时
/// 加载到过期字节码, 而过度失效只是多编译一次。
bool ScanIncludes(std::string_view source, vector<string>& out, string& error) {
    const string stripped = StripComments(source);
    const std::string_view view{stripped};
    size_t lineBegin = 0;
    while (lineBegin <= view.size()) {
        const size_t lineEnd = view.find('\n', lineBegin);
        const std::string_view line = view.substr(
            lineBegin,
            lineEnd == std::string_view::npos ? view.size() - lineBegin : lineEnd - lineBegin);
        size_t offset = line.find_first_not_of(" \t");
        if (offset != std::string_view::npos && line[offset] == '#') {
            ++offset;
            while (offset < line.size() && (line[offset] == ' ' || line[offset] == '\t')) {
                ++offset;
            }
            constexpr std::string_view kKeyword = "include";
            if (line.substr(offset, kKeyword.size()) == kKeyword) {
                offset += kKeyword.size();
                // "#includefoo" 不是 include 指令。
                if (offset < line.size() && line[offset] != ' ' && line[offset] != '\t' &&
                    line[offset] != '"' && line[offset] != '<') {
                    if (lineEnd == std::string_view::npos) {
                        break;
                    }
                    lineBegin = lineEnd + 1;
                    continue;
                }
                while (offset < line.size() && (line[offset] == ' ' || line[offset] == '\t')) {
                    ++offset;
                }
                if (offset >= line.size() || (line[offset] != '"' && line[offset] != '<')) {
                    // 宏拼接的 include 无法静态解析, 拒绝而不是悄悄漏依赖。
                    error = "macro-based #include cannot participate in shader source identity";
                    return false;
                }
                const char close = line[offset] == '"' ? '"' : '>';
                const size_t end = line.find(close, offset + 1);
                if (end == std::string_view::npos || end == offset + 1) {
                    error = "malformed #include directive";
                    return false;
                }
                out.emplace_back(line.substr(offset + 1, end - offset - 1));
            }
        }
        if (lineEnd == std::string_view::npos) {
            break;
        }
        lineBegin = lineEnd + 1;
    }
    return true;
}

bool IsPathUnderRoot(const std::filesystem::path& root, const std::filesystem::path& path) {
    const std::filesystem::path relative = path.lexically_relative(root);
    if (relative.empty()) {
        return false;
    }
    return *relative.begin() != std::filesystem::path{".."};
}

// ============================ 产物读写辅助 ============================

/// 本次构建钉住的 DXC 版本。由 CMake 无条件注入 (与是否编入 JIT 无关) ——
/// 关 JIT 的发布包必须与 cook 机算出同一个 toolchain hash, 否则产物全部判为过期。
#if defined(RADRAY_DXC_VERSION)
constexpr std::string_view kDxcVersion = RADRAY_DXC_VERSION;
#else
#error "RADRAY_DXC_VERSION must be defined; the shader toolchain hash depends on it"
#endif

}  // namespace

// ============================ JSON 定制点 ============================

bool JsonSerializer<render::ShaderBindingLocation>::Write(
    JsonWriteContext& context,
    const render::ShaderBindingLocation& value) noexcept {
    using value_type = render::ShaderBindingLocation;
    return SerializeJsonObject(
        context,
        value,
        JsonMember{"Group", &value_type::Group},
        JsonMember{"Binding", &value_type::Binding});
}

bool JsonDeserializer<render::ShaderBindingLocation>::Read(
    const JsonValue& json,
    render::ShaderBindingLocation& value) noexcept {
    using value_type = render::ShaderBindingLocation;
    return DeserializeJsonObject(
        json,
        value,
        JsonMember{"Group", &value_type::Group},
        JsonMember{"Binding", &value_type::Binding});
}

bool JsonSerializer<render::SamplerDescriptor>::Write(
    JsonWriteContext& context,
    const render::SamplerDescriptor& value) noexcept {
    JsonObjectWriter object = context.BeginObject();
    return object.IsValid() &&
           object.Member("AddressS", value.AddressS) &&
           object.Member("AddressT", value.AddressT) &&
           object.Member("AddressR", value.AddressR) &&
           object.Member("MinFilter", value.MinFilter) &&
           object.Member("MagFilter", value.MagFilter) &&
           object.Member("MipmapFilter", value.MipmapFilter) &&
           object.Member("LodMin", value.LodMin) &&
           object.Member("LodMax", value.LodMax) &&
           object.OptionalMember("Compare", value.Compare) &&
           object.Member("AnisotropyClamp", value.AnisotropyClamp);
}

bool JsonDeserializer<render::SamplerDescriptor>::Read(
    const JsonValue& json,
    render::SamplerDescriptor& value) noexcept {
    JsonObjectReader object{json};
    render::SamplerDescriptor decoded{};
    if (!object.IsValid() ||
        !object.Member("AddressS", decoded.AddressS) ||
        !object.Member("AddressT", decoded.AddressT) ||
        !object.Member("AddressR", decoded.AddressR) ||
        !object.Member("MinFilter", decoded.MinFilter) ||
        !object.Member("MagFilter", decoded.MagFilter) ||
        !object.Member("MipmapFilter", decoded.MipmapFilter) ||
        !object.MemberIfPresent("LodMin", decoded.LodMin) ||
        !object.MemberIfPresent("LodMax", decoded.LodMax) ||
        !object.OptionalMember("Compare", decoded.Compare) ||
        !object.MemberIfPresent("AnisotropyClamp", decoded.AnisotropyClamp)) {
        return false;
    }
    value = decoded;
    return true;
}

bool JsonSerializer<ShaderBindingDesc>::Write(
    JsonWriteContext& context,
    const ShaderBindingDesc& value) noexcept {
    JsonObjectWriter object = context.BeginObject();
    return object.IsValid() &&
           object.Member("Name", value.Name) &&
           object.Member("Binding", value.Binding) &&
           object.Member("Type", value.Type) &&
           object.Member("Count", value.Count) &&
           object.Member("Stages", value.Stages) &&
           object.Member("Residency", value.Residency) &&
           object.OptionalMember("ImmutableSampler", value.ImmutableSampler);
}

bool JsonDeserializer<ShaderBindingDesc>::Read(
    const JsonValue& json,
    ShaderBindingDesc& value) noexcept {
    JsonObjectReader object{json};
    ShaderBindingDesc decoded{};
    if (!object.IsValid() ||
        !object.Member("Name", decoded.Name) ||
        !object.Member("Binding", decoded.Binding) ||
        !object.Member("Type", decoded.Type) ||
        !object.MemberIfPresent("Count", decoded.Count) ||
        !object.Member("Stages", decoded.Stages) ||
        !object.MemberIfPresent("Residency", decoded.Residency) ||
        !object.OptionalMember("ImmutableSampler", decoded.ImmutableSampler)) {
        return false;
    }
    value = std::move(decoded);
    return true;
}

bool JsonSerializer<ShaderBindingGroupDesc>::Write(
    JsonWriteContext& context,
    const ShaderBindingGroupDesc& value) noexcept {
    using value_type = ShaderBindingGroupDesc;
    return SerializeJsonObject(
        context,
        value,
        JsonMember{"Group", &value_type::Group},
        JsonMember{"Bindings", &value_type::Bindings});
}

bool JsonDeserializer<ShaderBindingGroupDesc>::Read(
    const JsonValue& json,
    ShaderBindingGroupDesc& value) noexcept {
    using value_type = ShaderBindingGroupDesc;
    return DeserializeJsonObject(
        json,
        value,
        JsonMember{"Group", &value_type::Group},
        JsonMember{"Bindings", &value_type::Bindings});
}

bool JsonSerializer<ShaderPushConstantDesc>::Write(
    JsonWriteContext& context,
    const ShaderPushConstantDesc& value) noexcept {
    using value_type = ShaderPushConstantDesc;
    return SerializeJsonObject(
        context,
        value,
        JsonMember{"Name", &value_type::Name},
        JsonMember{"Location", &value_type::Location},
        JsonMember{"Size", &value_type::Size},
        JsonMember{"Stages", &value_type::Stages});
}

bool JsonDeserializer<ShaderPushConstantDesc>::Read(
    const JsonValue& json,
    ShaderPushConstantDesc& value) noexcept {
    using value_type = ShaderPushConstantDesc;
    return DeserializeJsonObject(
        json,
        value,
        JsonMember{"Name", &value_type::Name},
        JsonMember{"Location", &value_type::Location},
        JsonMember{"Size", &value_type::Size},
        JsonMember{"Stages", &value_type::Stages});
}

bool JsonSerializer<ShaderVertexAttributeDesc>::Write(
    JsonWriteContext& context,
    const ShaderVertexAttributeDesc& value) noexcept {
    JsonObjectWriter object = context.BeginObject();
    return object.IsValid() &&
           object.Member("Semantic", value.Semantic) &&
           object.Member("SemanticIndex", value.SemanticIndex) &&
           object.Member("Format", value.Format) &&
           object.Member("BufferBinding", value.BufferBinding) &&
           object.Member("Offset", value.Offset) &&
           object.OptionalMember("Location", value.Location);
}

bool JsonDeserializer<ShaderVertexAttributeDesc>::Read(
    const JsonValue& json,
    ShaderVertexAttributeDesc& value) noexcept {
    JsonObjectReader object{json};
    ShaderVertexAttributeDesc decoded{};
    if (!object.IsValid() ||
        !object.Member("Semantic", decoded.Semantic) ||
        !object.MemberIfPresent("SemanticIndex", decoded.SemanticIndex) ||
        !object.Member("Format", decoded.Format) ||
        !object.MemberIfPresent("BufferBinding", decoded.BufferBinding) ||
        !object.MemberIfPresent("Offset", decoded.Offset) ||
        !object.OptionalMember("Location", decoded.Location)) {
        return false;
    }
    value = std::move(decoded);
    return true;
}

bool JsonSerializer<ShaderVertexBufferDesc>::Write(
    JsonWriteContext& context,
    const ShaderVertexBufferDesc& value) noexcept {
    using value_type = ShaderVertexBufferDesc;
    return SerializeJsonObject(
        context,
        value,
        JsonMember{"Binding", &value_type::Binding},
        JsonMember{"ArrayStride", &value_type::ArrayStride},
        JsonMember{"StepMode", &value_type::StepMode});
}

bool JsonDeserializer<ShaderVertexBufferDesc>::Read(
    const JsonValue& json,
    ShaderVertexBufferDesc& value) noexcept {
    JsonObjectReader object{json};
    ShaderVertexBufferDesc decoded{};
    if (!object.IsValid() ||
        !object.Member("Binding", decoded.Binding) ||
        !object.Member("ArrayStride", decoded.ArrayStride) ||
        !object.MemberIfPresent("StepMode", decoded.StepMode)) {
        return false;
    }
    value = decoded;
    return true;
}

bool JsonSerializer<ShaderVertexInputDesc>::Write(
    JsonWriteContext& context,
    const ShaderVertexInputDesc& value) noexcept {
    using value_type = ShaderVertexInputDesc;
    return SerializeJsonObject(
        context,
        value,
        JsonMember{"Buffers", &value_type::Buffers},
        JsonMember{"Attributes", &value_type::Attributes});
}

bool JsonDeserializer<ShaderVertexInputDesc>::Read(
    const JsonValue& json,
    ShaderVertexInputDesc& value) noexcept {
    using value_type = ShaderVertexInputDesc;
    return DeserializeJsonObject(
        json,
        value,
        JsonMember{"Buffers", &value_type::Buffers},
        JsonMember{"Attributes", &value_type::Attributes});
}

bool JsonSerializer<ShaderStageDesc>::Write(
    JsonWriteContext& context,
    const ShaderStageDesc& value) noexcept {
    using value_type = ShaderStageDesc;
    return SerializeJsonObject(
        context,
        value,
        JsonMember{"Stage", &value_type::Stage},
        JsonMember{"EntryPoint", &value_type::EntryPoint});
}

bool JsonDeserializer<ShaderStageDesc>::Read(
    const JsonValue& json,
    ShaderStageDesc& value) noexcept {
    using value_type = ShaderStageDesc;
    return DeserializeJsonObject(
        json,
        value,
        JsonMember{"Stage", &value_type::Stage},
        JsonMember{"EntryPoint", &value_type::EntryPoint});
}

bool JsonSerializer<ShaderKeywordGroupDesc>::Write(
    JsonWriteContext& context,
    const ShaderKeywordGroupDesc& value) noexcept {
    using value_type = ShaderKeywordGroupDesc;
    return SerializeJsonObject(
        context,
        value,
        JsonMember{"Name", &value_type::Name},
        JsonMember{"Keywords", &value_type::Keywords},
        JsonMember{"IsOptional", &value_type::IsOptional},
        JsonMember{"Stages", &value_type::Stages});
}

bool JsonDeserializer<ShaderKeywordGroupDesc>::Read(
    const JsonValue& json,
    ShaderKeywordGroupDesc& value) noexcept {
    JsonObjectReader object{json};
    ShaderKeywordGroupDesc decoded{};
    if (!object.IsValid() ||
        !object.Member("Name", decoded.Name) ||
        !object.MemberIfPresent("Keywords", decoded.Keywords) ||
        !object.MemberIfPresent("IsOptional", decoded.IsOptional) ||
        !object.MemberIfPresent("Stages", decoded.Stages)) {
        return false;
    }
    value = std::move(decoded);
    return true;
}

bool JsonSerializer<ShaderBakeRuleDesc>::Write(
    JsonWriteContext& context,
    const ShaderBakeRuleDesc& value) noexcept {
    JsonObjectWriter object = context.BeginObject();
    return object.IsValid() &&
           (value.Expand.empty() || object.Member("Expand", value.Expand)) &&
           (value.Combination.empty() || object.Member("Combination", value.Combination));
}

bool JsonDeserializer<ShaderBakeRuleDesc>::Read(
    const JsonValue& json,
    ShaderBakeRuleDesc& value) noexcept {
    JsonObjectReader object{json};
    ShaderBakeRuleDesc decoded{};
    if (!object.IsValid() ||
        !object.MemberIfPresent("Expand", decoded.Expand) ||
        !object.MemberIfPresent("Combination", decoded.Combination)) {
        return false;
    }
    value = std::move(decoded);
    return true;
}

bool JsonSerializer<ShaderBakeSetDesc>::Write(
    JsonWriteContext& context,
    const ShaderBakeSetDesc& value) noexcept {
    JsonObjectWriter object = context.BeginObject();
    return object.IsValid() &&
           (value.Rules.empty() || object.Member("Rules", value.Rules)) &&
           (value.Skip.empty() || object.Member("Skip", value.Skip));
}

bool JsonDeserializer<ShaderBakeSetDesc>::Read(
    const JsonValue& json,
    ShaderBakeSetDesc& value) noexcept {
    JsonObjectReader object{json};
    ShaderBakeSetDesc decoded{};
    if (!object.IsValid() ||
        !object.MemberIfPresent("Rules", decoded.Rules) ||
        !object.MemberIfPresent("Skip", decoded.Skip)) {
        return false;
    }
    value = std::move(decoded);
    return true;
}

bool JsonSerializer<ShaderPassDesc>::Write(
    JsonWriteContext& context,
    const ShaderPassDesc& value) noexcept {
    JsonObjectWriter object = context.BeginObject();
    if (!object.IsValid() ||
        !object.Member("Name", value.Name) ||
        (!value.Source.empty() && !object.Member("Source", value.Source)) ||
        !object.Member("Stages", value.Stages) ||
        !object.Member("ShaderModel", value.ShaderModel) ||
        !object.Member("IsOptimize", value.IsOptimize) ||
        !object.Member("EnableUnbounded", value.EnableUnbounded) ||
        (!value.Defines.empty() && !object.Member("Defines", value.Defines)) ||
        (!value.KeywordGroups.empty() && !object.Member("KeywordGroups", value.KeywordGroups)) ||
        ((!value.BakeVariants.IsEmpty() || !value.BakeVariants.Skip.empty()) &&
         !object.Member("BakeVariants", value.BakeVariants)) ||
        !object.OptionalMember("PushConstant", value.PushConstant) ||
        (!value.BindingGroups.empty() && !object.Member("BindingGroups", value.BindingGroups)) ||
        !object.OptionalMember("VertexInput", value.VertexInput)) {
        return false;
    }
    return true;
}

bool JsonDeserializer<ShaderPassDesc>::Read(
    const JsonValue& json,
    ShaderPassDesc& value) noexcept {
    JsonObjectReader object{json};
    ShaderPassDesc decoded{};
    if (!object.IsValid() ||
        !object.Member("Name", decoded.Name) ||
        !object.MemberIfPresent("Source", decoded.Source) ||
        !object.Member("Stages", decoded.Stages) ||
        !object.MemberIfPresent("ShaderModel", decoded.ShaderModel) ||
        !object.MemberIfPresent("Defines", decoded.Defines) ||
        !object.MemberIfPresent("KeywordGroups", decoded.KeywordGroups) ||
        !object.MemberIfPresent("IsOptimize", decoded.IsOptimize) ||
        !object.MemberIfPresent("EnableUnbounded", decoded.EnableUnbounded) ||
        !object.MemberIfPresent("BakeVariants", decoded.BakeVariants) ||
        !object.MemberIfPresent("BindingGroups", decoded.BindingGroups) ||
        !object.OptionalMember("PushConstant", decoded.PushConstant) ||
        !object.OptionalMember("VertexInput", decoded.VertexInput)) {
        return false;
    }
    value = std::move(decoded);
    return true;
}

// 字段列表在此唯一实现, 模板序列化器 (shader_asset_template.cpp) 借它往同一个
// object 里额外插 "_TODO", 从而不会因为各写一份而漏字段。
bool WriteShaderAssetMembers(JsonObjectWriter& object, const ShaderAssetDesc& value) noexcept {
    return object.IsValid() &&
           object.Member("FormatVersion", kShaderAssetFormatVersion) &&
           object.Member("Name", value.Name) &&
           (value.Source.empty() || object.Member("Source", value.Source)) &&
           (value.KeywordGroups.empty() || object.Member("KeywordGroups", value.KeywordGroups)) &&
           ((value.BakeVariants.IsEmpty() && value.BakeVariants.Skip.empty()) ||
            object.Member("BakeVariants", value.BakeVariants)) &&
           object.Member("Passes", value.Passes);
}

bool JsonSerializer<ShaderAssetDesc>::Write(
    JsonWriteContext& context,
    const ShaderAssetDesc& value) noexcept {
    JsonObjectWriter object = context.BeginObject();
    return WriteShaderAssetMembers(object, value);
}

bool JsonDeserializer<ShaderAssetDesc>::Read(
    const JsonValue& json,
    ShaderAssetDesc& value) noexcept {
    JsonObjectReader object{json};
    uint32_t formatVersion = 0;
    ShaderAssetDesc decoded{};
    if (!object.IsValid() ||
        !object.Member("FormatVersion", formatVersion) ||
        formatVersion != kShaderAssetFormatVersion ||
        !object.Member("Name", decoded.Name) ||
        !object.MemberIfPresent("Source", decoded.Source) ||
        !object.MemberIfPresent("KeywordGroups", decoded.KeywordGroups) ||
        !object.MemberIfPresent("BakeVariants", decoded.BakeVariants) ||
        !object.Member("Passes", decoded.Passes)) {
        return false;
    }
    value = std::move(decoded);
    return true;
}

bool JsonSerializer<ShaderHash>::Write(
    JsonWriteContext& context,
    const ShaderHash& value) noexcept {
    return context.String(value.ToHex());
}

bool JsonDeserializer<ShaderHash>::Read(
    const JsonValue& json,
    ShaderHash& value) noexcept {
    if (!json.IsString()) {
        return false;
    }
    const std::optional<ShaderHash> decoded = ShaderHash::FromHex(json.AsString());
    if (!decoded.has_value()) {
        return false;
    }
    value = decoded.value();
    return true;
}

bool JsonSerializer<ShaderArtifactEntry>::Write(
    JsonWriteContext& context,
    const ShaderArtifactEntry& value) noexcept {
    JsonObjectWriter object = context.BeginObject();
    return object.IsValid() &&
           object.Member("Key", value.Key) &&
           object.Member("PassName", value.PassName) &&
           object.Member("Source", value.Source) &&
           object.Member("Stage", value.Stage) &&
           object.Member("EntryPoint", value.EntryPoint) &&
           object.Member("Category", value.Category) &&
           object.Member("BlobPath", value.BlobPath) &&
           object.Member("BytecodeHash", value.BytecodeHash) &&
           object.Member("BytecodeSize", value.BytecodeSize) &&
           (value.Keywords.empty() || object.Member("Keywords", value.Keywords));
}

bool JsonDeserializer<ShaderArtifactEntry>::Read(
    const JsonValue& json,
    ShaderArtifactEntry& value) noexcept {
    JsonObjectReader object{json};
    ShaderArtifactEntry decoded{};
    if (!object.IsValid() ||
        !object.Member("Key", decoded.Key) ||
        !object.Member("PassName", decoded.PassName) ||
        !object.Member("Source", decoded.Source) ||
        !object.Member("Stage", decoded.Stage) ||
        !object.Member("EntryPoint", decoded.EntryPoint) ||
        !object.Member("Category", decoded.Category) ||
        !object.Member("BlobPath", decoded.BlobPath) ||
        !object.Member("BytecodeHash", decoded.BytecodeHash) ||
        !object.Member("BytecodeSize", decoded.BytecodeSize) ||
        !object.MemberIfPresent("Keywords", decoded.Keywords)) {
        return false;
    }
    value = std::move(decoded);
    return true;
}

bool JsonSerializer<ShaderArtifactSource>::Write(
    JsonWriteContext& context,
    const ShaderArtifactSource& value) noexcept {
    using value_type = ShaderArtifactSource;
    return SerializeJsonObject(
        context,
        value,
        JsonMember{"Path", &value_type::Path},
        JsonMember{"Identity", &value_type::Identity});
}

bool JsonDeserializer<ShaderArtifactSource>::Read(
    const JsonValue& json,
    ShaderArtifactSource& value) noexcept {
    using value_type = ShaderArtifactSource;
    return DeserializeJsonObject(
        json,
        value,
        JsonMember{"Path", &value_type::Path},
        JsonMember{"Identity", &value_type::Identity});
}

bool JsonSerializer<ShaderArtifactIndex>::Write(
    JsonWriteContext& context,
    const ShaderArtifactIndex& value) noexcept {
    JsonObjectWriter object = context.BeginObject();
    return object.IsValid() &&
           object.Member("FormatVersion", kShaderArtifactFormatVersion) &&
           object.Member("AssetName", value.AssetName) &&
           object.Member("ToolchainHash", value.ToolchainHash) &&
           object.Member("Sources", value.Sources) &&
           object.Member("Entries", value.Entries);
}

bool JsonDeserializer<ShaderArtifactIndex>::Read(
    const JsonValue& json,
    ShaderArtifactIndex& value) noexcept {
    JsonObjectReader object{json};
    ShaderArtifactIndex decoded{};
    if (!object.IsValid() ||
        !object.Member("FormatVersion", decoded.FormatVersion) ||
        decoded.FormatVersion != kShaderArtifactFormatVersion ||
        !object.Member("AssetName", decoded.AssetName) ||
        !object.Member("ToolchainHash", decoded.ToolchainHash) ||
        !object.Member("Sources", decoded.Sources) ||
        !object.Member("Entries", decoded.Entries)) {
        return false;
    }
    value = std::move(decoded);
    return true;
}

// ============================ 数据类型成员 ============================

string ShaderAssetDiagnostic::ToString() const {
    string text = Message.empty() ? string{"unknown shader asset error"} : Message;
    string context;
    if (!PassName.empty()) {
        context += fmt::format(" pass='{}'", PassName);
    }
    if (Group.has_value()) {
        context += fmt::format(" group={}", Group.value());
    }
    if (Binding.has_value()) {
        context += fmt::format(" binding={}", Binding.value());
    }
    if (!BindingName.empty()) {
        context += fmt::format(" name='{}'", BindingName);
    }
    if (Stage.has_value()) {
        context += fmt::format(" stage={}", StageName(Stage.value()));
    }
    if (!context.empty()) {
        text += " [";
        text += context.substr(1);
        text += "]";
    }
    return text;
}

render::ShaderStages ShaderPassDesc::GetStageMask() const noexcept {
    render::ShaderStages mask{render::ShaderStage::UNKNOWN};
    for (const ShaderStageDesc& stage : Stages) {
        mask |= stage.Stage;
    }
    return mask;
}

std::optional<std::string_view> ShaderPassDesc::FindEntryPoint(render::ShaderStage stage) const noexcept {
    for (const ShaderStageDesc& item : Stages) {
        if (item.Stage == stage) {
            return std::string_view{item.EntryPoint};
        }
    }
    return std::nullopt;
}

Nullable<const ShaderBindingGroupDesc*> ShaderPassDesc::FindGroup(uint32_t group) const noexcept {
    for (const ShaderBindingGroupDesc& item : BindingGroups) {
        if (item.Group == group) {
            return &item;
        }
    }
    return nullptr;
}

Nullable<const ShaderBindingDesc*> ShaderPassDesc::FindBinding(uint32_t group, uint32_t binding) const noexcept {
    Nullable<const ShaderBindingGroupDesc*> groupPtr = FindGroup(group);
    if (!groupPtr.HasValue()) {
        return nullptr;
    }
    for (const ShaderBindingDesc& item : groupPtr.Unwrap()->Bindings) {
        // 数组绑定占据 [Binding, Binding + Count) 整段。
        if (binding >= item.Binding && binding - item.Binding < item.Count) {
            return &item;
        }
    }
    return nullptr;
}

Nullable<const ShaderPassDesc*> ShaderAssetDesc::FindPass(std::string_view name) const noexcept {
    for (const ShaderPassDesc& pass : Passes) {
        if (pass.Name == name) {
            return &pass;
        }
    }
    return nullptr;
}
string ShaderHash::ToHex() const {
    return fmt::format("{:016x}{:016x}", High, Low);
}

std::optional<ShaderHash> ShaderHash::FromHex(std::string_view hex) noexcept {
    if (hex.size() != 32) {
        return std::nullopt;
    }
    uint64_t parts[2]{0, 0};
    for (size_t i = 0; i < 32; ++i) {
        const char c = hex[i];
        uint64_t digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<uint64_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<uint64_t>(c - 'a') + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<uint64_t>(c - 'A') + 10;
        } else {
            return std::nullopt;
        }
        parts[i / 16] = (parts[i / 16] << 4) | digit;
    }
    return ShaderHash{parts[1], parts[0]};
}

Nullable<const ShaderArtifactEntry*> ShaderArtifactIndex::Find(ShaderHash key) const noexcept {
    for (const ShaderArtifactEntry& entry : Entries) {
        if (entry.Key == key) {
            return &entry;
        }
    }
    return nullptr;
}

std::optional<ShaderHash> ShaderArtifactIndex::FindSourceIdentity(std::string_view path) const noexcept {
    for (const ShaderArtifactSource& source : Sources) {
        if (source.Path == path) {
            return source.Identity;
        }
    }
    return std::nullopt;
}

// ============================ 变体域 ============================

std::optional<ShaderVariantDomain> ShaderVariantDomain::Build(
    const ShaderAssetDesc& asset,
    const ShaderPassDesc& pass,
    ShaderAssetDiagnostic& outDiag) noexcept {
    ShaderVariantDomain domain;
    domain._passDefines = pass.Defines;

    // pass.KeywordGroups 留空 = 取全部组。非空则按 pass 的声明顺序取, 使槽位序
    // 由 pass 决定而非 asset —— 同一 asset 的两个 pass 因此可以有不同的槽位布局,
    // 这没问题: ShaderVariantKey 只在单个 domain 内有意义。
    if (pass.KeywordGroups.empty()) {
        domain._groups.reserve(asset.KeywordGroups.size());
        for (const ShaderKeywordGroupDesc& group : asset.KeywordGroups) {
            domain._groups.push_back(Group{group.Name, group.Keywords, group.Stages, group.IsOptional});
        }
    } else {
        domain._groups.reserve(pass.KeywordGroups.size());
        for (const string& name : pass.KeywordGroups) {
            const auto it = std::ranges::find_if(asset.KeywordGroups, [&](const ShaderKeywordGroupDesc& g) {
                return g.Name == name;
            });
            if (it == asset.KeywordGroups.end()) {
                outDiag = ShaderAssetDiagnostic{};
                outDiag.Message = fmt::format("pass references unknown keyword group '{}'", name);
                outDiag.PassName = pass.Name;
                return std::nullopt;
            }
            domain._groups.push_back(Group{it->Name, it->Keywords, it->Stages, it->IsOptional});
        }
    }

    for (uint32_t groupIndex = 0; groupIndex < domain._groups.size(); ++groupIndex) {
        const Group& group = domain._groups[groupIndex];
        if (group.Keywords.size() >= kShaderKeywordOff) {
            outDiag = ShaderAssetDiagnostic{};
            outDiag.Message = fmt::format(
                "keyword group '{}' declares {} keywords, which exceeds the encoding limit of {}",
                group.Name,
                group.Keywords.size(),
                kShaderKeywordOff - 1);
            outDiag.PassName = pass.Name;
            return std::nullopt;
        }
        for (size_t k = 0; k < group.Keywords.size(); ++k) {
            domain._lookup.push_back(LookupEntry{
                group.Keywords[k],
                groupIndex,
                static_cast<uint16_t>(k)});
        }
    }
    std::ranges::sort(domain._lookup, [](const LookupEntry& lhs, const LookupEntry& rhs) {
        return lhs.Keyword < rhs.Keyword;
    });
    return domain;
}

std::optional<std::pair<uint32_t, uint16_t>> ShaderVariantDomain::FindKeyword(
    std::string_view keyword) const noexcept {
    const auto it = std::ranges::lower_bound(_lookup, keyword, {}, &LookupEntry::Keyword);
    if (it == _lookup.end() || it->Keyword != keyword) {
        return std::nullopt;
    }
    return std::pair<uint32_t, uint16_t>{it->GroupIndex, it->KeywordIndex};
}

ShaderVariantKey ShaderVariantDomain::DefaultVariant() const noexcept {
    ShaderVariantKey key;
    key.Selection.reserve(_groups.size());
    for (const Group& group : _groups) {
        // 必选组没有"关"这个取值, 只能取首个 keyword 作为默认。
        key.Selection.push_back(group.IsOptional ? kShaderKeywordOff : uint16_t{0});
    }
    return key;
}

bool ShaderVariantDomain::IsValid(const ShaderVariantKey& key) const noexcept {
    if (key.Selection.size() != _groups.size()) {
        return false;
    }
    for (size_t i = 0; i < _groups.size(); ++i) {
        const uint16_t selection = key.Selection[i];
        if (selection == kShaderKeywordOff) {
            // 注意: 必选组的 Off 在【投影后】是合法的规范形式 (见 ProjectToStage),
            // 故这里不能按 IsOptional 拒绝, 否则投影结果会被判为无效。
            continue;
        }
        if (selection >= _groups[i].Keywords.size()) {
            return false;
        }
    }
    return true;
}

std::optional<ShaderVariantKey> ShaderVariantDomain::Resolve(
    std::span<const std::string_view> keywords,
    ShaderAssetDiagnostic& outDiag) const noexcept {
    ShaderVariantKey key;
    key.Selection.assign(_groups.size(), kShaderKeywordOff);
    for (const std::string_view keyword : keywords) {
        const auto found = FindKeyword(keyword);
        if (!found.has_value()) {
            outDiag = ShaderAssetDiagnostic{};
            outDiag.Message = fmt::format("keyword '{}' is not declared by this shader", keyword);
            return std::nullopt;
        }
        const auto [groupIndex, keywordIndex] = found.value();
        uint16_t& slot = key.Selection[groupIndex];
        if (slot != kShaderKeywordOff && slot != keywordIndex) {
            outDiag = ShaderAssetDiagnostic{};
            outDiag.Message = fmt::format(
                "keywords '{}' and '{}' both belong to the mutually exclusive group '{}'",
                _groups[groupIndex].Keywords[slot],
                keyword,
                _groups[groupIndex].Name);
            return std::nullopt;
        }
        slot = keywordIndex;
    }
    for (size_t i = 0; i < _groups.size(); ++i) {
        if (!_groups[i].IsOptional && key.Selection[i] == kShaderKeywordOff) {
            outDiag = ShaderAssetDiagnostic{};
            outDiag.Message = fmt::format(
                "keyword group '{}' is not optional; one of its keywords must be selected",
                _groups[i].Name);
            return std::nullopt;
        }
    }
    return key;
}

std::optional<ShaderVariantKey> ShaderVariantDomain::WithKeyword(
    const ShaderVariantKey& key,
    std::string_view keyword,
    bool enabled) const noexcept {
    if (!IsValid(key)) {
        return std::nullopt;
    }
    const auto found = FindKeyword(keyword);
    if (!found.has_value()) {
        return std::nullopt;
    }
    const auto [groupIndex, keywordIndex] = found.value();
    ShaderVariantKey result = key;
    if (enabled) {
        // 组内互斥: 直接覆盖槽位即顶掉同组其他选择。
        result.Selection[groupIndex] = keywordIndex;
        return result;
    }
    if (result.Selection[groupIndex] != keywordIndex) {
        // 本就没选中它, 关闭是 no-op。
        return result;
    }
    if (!_groups[groupIndex].IsOptional) {
        return std::nullopt;
    }
    result.Selection[groupIndex] = kShaderKeywordOff;
    return result;
}

ShaderVariantKey ShaderVariantDomain::ProjectToStage(
    const ShaderVariantKey& key,
    render::ShaderStage stage) const noexcept {
    ShaderVariantKey result = key;
    if (result.Selection.size() != _groups.size()) {
        return result;
    }
    for (size_t i = 0; i < _groups.size(); ++i) {
        if (!_groups[i].Stages.HasFlag(stage)) {
            result.Selection[i] = kShaderKeywordOff;
        }
    }
    return result;
}

vector<string> ShaderVariantDomain::CollectDefines(
    const ShaderVariantKey& key,
    render::ShaderStage stage) const noexcept {
    vector<string> defines = _passDefines;
    if (key.Selection.size() != _groups.size()) {
        return defines;
    }
    for (size_t i = 0; i < _groups.size(); ++i) {
        const uint16_t selection = key.Selection[i];
        if (selection == kShaderKeywordOff || selection >= _groups[i].Keywords.size()) {
            continue;
        }
        if (!_groups[i].Stages.HasFlag(stage)) {
            continue;
        }
        defines.push_back(_groups[i].Keywords[selection]);
    }
    return defines;
}

vector<string> ShaderVariantDomain::DescribeKeywords(
    const ShaderVariantKey& key,
    render::ShaderStage stage) const noexcept {
    vector<string> names;
    if (key.Selection.size() != _groups.size()) {
        return names;
    }
    for (size_t i = 0; i < _groups.size(); ++i) {
        const uint16_t selection = key.Selection[i];
        if (selection == kShaderKeywordOff || selection >= _groups[i].Keywords.size()) {
            continue;
        }
        if (!_groups[i].Stages.HasFlag(stage)) {
            continue;
        }
        names.push_back(_groups[i].Keywords[selection]);
    }
    std::ranges::sort(names);
    return names;
}

// ============================ 烘焙集展开 ============================

const ShaderBakeSetDesc& GetEffectiveBakeSet(
    const ShaderAssetDesc& asset,
    const ShaderPassDesc& pass) noexcept {
    return pass.BakeVariants.IsEmpty() ? asset.BakeVariants : pass.BakeVariants;
}

std::string_view GetEffectiveSource(
    const ShaderAssetDesc& asset,
    const ShaderPassDesc& pass) noexcept {
    return pass.Source.empty() ? std::string_view{asset.Source} : std::string_view{pass.Source};
}

ShaderPassDesc MakeResolvablePass(
    const ShaderAssetDesc& asset,
    const ShaderPassDesc& pass) {
    ShaderPassDesc result = pass;
    result.Source = string{GetEffectiveSource(asset, pass)};
    return result;
}

std::optional<vector<ShaderVariantKey>> ExpandShaderBakeSet(
    const ShaderVariantDomain& domain,
    const ShaderBakeSetDesc& bake,
    bool isInherited,
    ShaderAssetDiagnostic& outDiag) noexcept {
    const std::span<const ShaderVariantDomain::Group> groups = domain.Groups();

    /// 该条 Skip 的【全部】keyword 是否都被选中。本域不含的 keyword 视为未选中,
    /// 于是继承来的 Skip 在裁剪过的 pass 上自然失效。
    const auto matchesSkip = [&](const ShaderVariantKey& variant, const vector<string>& entry) {
        for (const string& keyword : entry) {
            const auto found = domain.FindKeyword(keyword);
            if (!found.has_value()) {
                return false;
            }
            const auto [groupIndex, keywordIndex] = found.value();
            if (variant.Selection[groupIndex] != keywordIndex) {
                return false;
            }
        }
        return true;
    };

    // Expand 的积与显式 Combination 分开累积, 因为 Skip 只作用于前者。
    vector<ShaderVariantKey> expanded;
    vector<ShaderVariantKey> explicitKeys;

    for (const ShaderBakeRuleDesc& rule : bake.Rules) {
        if (!rule.Combination.empty()) {
            vector<std::string_view> keywords;
            keywords.reserve(rule.Combination.size());
            for (const string& keyword : rule.Combination) {
                keywords.emplace_back(keyword);
            }
            std::optional<ShaderVariantKey> key = domain.Resolve(keywords, outDiag);
            if (!key.has_value()) {
                if (isInherited) {
                    // 继承的组合可能引用本 pass 裁掉的 keyword, 整条跳过。
                    outDiag = ShaderAssetDiagnostic{};
                    continue;
                }
                return std::nullopt;
            }
            explicitKeys.push_back(std::move(key.value()));
            continue;
        }

        // Expand: 组名 -> 槽位下标。
        vector<uint32_t> slots;
        slots.reserve(rule.Expand.size());
        for (const string& name : rule.Expand) {
            const auto it = std::ranges::find_if(groups, [&](const ShaderVariantDomain::Group& g) {
                return g.Name == name;
            });
            if (it == groups.end()) {
                if (isInherited) {
                    // 继承下来的规则遇到本 pass 没有的组: 静默投影掉该维度。
                    continue;
                }
                outDiag = ShaderAssetDiagnostic{};
                outDiag.Message = fmt::format("bake rule expands unknown keyword group '{}'", name);
                return std::nullopt;
            }
            slots.push_back(static_cast<uint32_t>(std::distance(groups.begin(), it)));
        }

        // 逐维展开。起点是默认变体, 因为未列出的组按定义取默认值。
        vector<ShaderVariantKey> product;
        product.push_back(domain.DefaultVariant());
        for (const uint32_t slot : slots) {
            const ShaderVariantDomain::Group& group = groups[slot];
            vector<ShaderVariantKey> next;
            next.reserve(product.size() * (group.Keywords.size() + (group.IsOptional ? 1 : 0)));
            for (const ShaderVariantKey& base : product) {
                // 该组的取值集合: 全部 keyword, 可选组再加上"关"。
                if (group.IsOptional) {
                    ShaderVariantKey off = base;
                    off.Selection[slot] = kShaderKeywordOff;
                    next.push_back(std::move(off));
                }
                for (size_t k = 0; k < group.Keywords.size(); ++k) {
                    ShaderVariantKey on = base;
                    on.Selection[slot] = static_cast<uint16_t>(k);
                    next.push_back(std::move(on));
                }
            }
            product = std::move(next);
        }
        for (ShaderVariantKey& key : product) {
            const bool skipped = std::ranges::any_of(bake.Skip, [&](const vector<string>& entry) {
                return matchesSkip(key, entry);
            });
            if (!skipped) {
                expanded.push_back(std::move(key));
            }
        }
    }

    vector<ShaderVariantKey> variants;
    variants.reserve(expanded.size() + explicitKeys.size() + 1);
    // 默认变体总在结果里, 且不受 Skip 影响: 一份 shader 至少要能在不开任何
    // keyword 时工作, 这也使"空 BakeVariants"与"只烘默认"成为同一件事。
    variants.push_back(domain.DefaultVariant());
    variants.insert(
        variants.end(),
        std::make_move_iterator(expanded.begin()),
        std::make_move_iterator(expanded.end()));
    // 显式点名的组合不受 Skip 影响 —— 作者要的就该烘。
    variants.insert(
        variants.end(),
        std::make_move_iterator(explicitKeys.begin()),
        std::make_move_iterator(explicitKeys.end()));

    std::ranges::sort(variants);
    variants.erase(std::unique(variants.begin(), variants.end()), variants.end());
    return variants;
}

// ============================ 功能类成员 ============================

ShaderResolver::ShaderResolver(
    ShaderResolveConfig config,
    Nullable<render::Dxc*> dxc) noexcept
    : _config(std::move(config)),
      _dxc(dxc),
      _toolchainHash(GetShaderToolchainHash()) {}

bool ShaderResolver::CanJit() const noexcept {
#if defined(RADRAY_ENABLE_SHADER_JIT)
    return _config.AllowJit && _dxc.HasValue() && _dxc.Get()->IsValid();
#else
    return false;
#endif
}

std::optional<ShaderHash> ShaderResolver::GetSourceIdentity(
    std::string_view sourcePath,
    ShaderAssetDiagnostic& outDiag) noexcept {
    // 缓存命中前先复核时间戳: 任一依赖变动就必须重算, 否则 Strict 的
    // "改 shader 立刻生效" 在长命 resolver 上失效。
    auto stampsAreCurrent = [this](const SourceIdentityCache& item) noexcept {
        for (const auto& [relative, stamp] : item.Stamps) {
            std::error_code error;
            const auto now = std::filesystem::last_write_time(
                _config.ShaderRoot / std::filesystem::path{relative},
                error);
            if (error || now != stamp) {
                return false;
            }
        }
        return true;
    };

    for (size_t i = 0; i < _sourceIdentities.size(); ++i) {
        if (_sourceIdentities[i].SourcePath != sourcePath) {
            continue;
        }
        if (stampsAreCurrent(_sourceIdentities[i])) {
            return _sourceIdentities[i].Hash;
        }
        _sourceIdentities.erase(_sourceIdentities.begin() + static_cast<ptrdiff_t>(i));
        break;
    }

    auto identity = ComputeShaderSourceIdentity(_config.ShaderRoot, sourcePath, outDiag);
    if (!identity.has_value()) {
        return std::nullopt;
    }

    SourceIdentityCache cached;
    cached.SourcePath = string{sourcePath};
    cached.Hash = identity->Hash;
    cached.Stamps.reserve(identity->Dependencies.size());
    for (const string& dependency : identity->Dependencies) {
        std::error_code error;
        const auto stamp = std::filesystem::last_write_time(
            _config.ShaderRoot / std::filesystem::path{dependency},
            error);
        if (error) {
            // 拿不到时间戳就不缓存: 宁可每次重算, 也不能缓存一个无法失效的条目。
            return identity->Hash;
        }
        cached.Stamps.emplace_back(dependency, stamp);
    }
    _sourceIdentities.push_back(std::move(cached));
    return identity->Hash;
}

Nullable<const ShaderArtifactIndex*> ShaderResolver::GetIndex() noexcept {
    if (_indexLoaded) {
        return _index.has_value() ? &_index.value() : nullptr;
    }
    _indexLoaded = true;
    if (_config.ManifestPath.empty()) {
        return nullptr;
    }
    const std::filesystem::path indexPath =
        GetShaderArtifactDirectory(_config.ManifestPath) / "index.json";
    std::error_code error;
    if (!std::filesystem::is_regular_file(indexPath, error) || error) {
        // 没有产物目录是正常状态 (纯 JIT 工作流), 不报错。
        return nullptr;
    }
    ShaderAssetDiagnostic diag;
    auto index = LoadShaderArtifactIndex(indexPath, diag);
    if (!index.has_value()) {
        // index 存在但坏了是真错误, 记日志后退化为纯 JIT。
        RADRAY_ERR_LOG("shader artifact index '{}' is unusable: {}", indexPath.string(), diag.Message);
        return nullptr;
    }
    _index = std::move(index);
    return &_index.value();
}

std::optional<ShaderBytecode> ShaderResolver::LoadFromArtifact(
    ShaderHash key,
    render::ShaderStage stage,
    ShaderAssetDiagnostic& outDiag) noexcept {
    Nullable<const ShaderArtifactIndex*> index = GetIndex();
    if (index == nullptr) {
        return std::nullopt;
    }
    Nullable<const ShaderArtifactEntry*> entry = index.Get()->Find(key);
    if (entry == nullptr) {
        return std::nullopt;
    }
    const ShaderArtifactEntry& found = *entry.Get();
    const std::filesystem::path blobPath =
        GetShaderArtifactDirectory(_config.ManifestPath) /
        std::filesystem::path{found.BlobPath};

    ShaderAssetDiagnostic blobDiag;
    auto blob = ReadShaderArtifactBlob(blobPath, blobDiag);
    if (!blob.has_value()) {
        // index 指向的 blob 坏了: 不静默回退, 因为这是产物损坏而非未命中。
        outDiag.Message = blobDiag.Message;
        return std::nullopt;
    }
    if (blob->Key != key) {
        outDiag.Message = fmt::format(
            "shader blob '{}' key {} does not match the index key {}",
            found.BlobPath,
            blob->Key.ToHex(),
            key.ToHex());
        return std::nullopt;
    }
    if (blob->Stage != stage) {
        outDiag.Message = fmt::format(
            "shader blob '{}' stage {} does not match the requested stage {}",
            found.BlobPath,
            blob->Stage,
            stage);
        return std::nullopt;
    }

    ShaderBytecode result;
    result.Data = std::move(blob->Bytecode);
    result.Category = blob->Category;
    result.Stage = blob->Stage;
    result.Source = ShaderBytecodeSource::Artifact;
    result.Key = key;
    return result;
}

std::optional<ShaderBytecode> ShaderResolver::CompileWithJit(
    [[maybe_unused]] const ShaderPassDesc& pass,
    [[maybe_unused]] render::ShaderStage stage,
    [[maybe_unused]] render::ShaderBlobCategory category,
    [[maybe_unused]] std::span<const string> defines,
    [[maybe_unused]] std::string_view sourcePath,
    [[maybe_unused]] ShaderHash key,
    ShaderAssetDiagnostic& outDiag) noexcept {
#if defined(RADRAY_ENABLE_SHADER_JIT)
    std::optional<std::string_view> entry = pass.FindEntryPoint(stage);
    if (!entry.has_value()) {
        outDiag.Message = fmt::format("pass does not declare an entry point for stage {}", stage);
        return std::nullopt;
    }
    if (category != render::ShaderBlobCategory::DXIL &&
        category != render::ShaderBlobCategory::SPIRV) {
        outDiag.Message = fmt::format("shader JIT cannot produce category {}", category);
        return std::nullopt;
    }

    vector<std::string_view> defineViews;
    defineViews.reserve(defines.size());
    for (const string& define : defines) {
        defineViews.emplace_back(define);
    }
    const string rootString = _config.ShaderRoot.string();
    const std::array<std::string_view, 1> includes{rootString};

    const render::DxcCompileOptions options{
        .EntryPoint = entry.value(),
        .Stage = stage,
        .SM = pass.ShaderModel,
        .Defines = defineViews,
        .Includes = includes,
        .IsOptimize = pass.IsOptimize,
        .IsSpirv = category == render::ShaderBlobCategory::SPIRV,
        .EnableUnbounded = pass.EnableUnbounded};

    const std::filesystem::path sourceFile =
        _config.ShaderRoot / std::filesystem::path{sourcePath};
    // CanJit() 已确认非空 (Resolve 在调用本函数前必查)。
    auto output = _dxc.Get()->CompileFile(sourceFile, options);
    if (!output.has_value()) {
        outDiag.Message = fmt::format("failed to compile '{}'", sourcePath);
        return std::nullopt;
    }
    if (output->Data.empty()) {
        outDiag.Message = fmt::format("compiling '{}' produced no bytecode", sourcePath);
        return std::nullopt;
    }
    if (output->Category != category) {
        outDiag.Message = fmt::format(
            "compiling '{}' produced category {} but {} was requested",
            sourcePath,
            output->Category,
            category);
        return std::nullopt;
    }

    ShaderBytecode result;
    result.Data = std::move(output->Data);
    result.Category = output->Category;
    result.Stage = stage;
    result.Source = ShaderBytecodeSource::Jit;
    result.Key = key;
    return result;
#else
    outDiag.Message = "shader JIT is not available in this build";
    return std::nullopt;
#endif
}

std::optional<ShaderBytecode> ShaderResolver::Resolve(
    const ShaderPassDesc& pass,
    render::ShaderStage stage,
    render::ShaderBlobCategory category,
    std::span<const string> defines,
    ShaderAssetDiagnostic& outDiag) noexcept {
    outDiag.PassName = pass.Name;
    outDiag.Stage = stage;

    if (!pass.FindEntryPoint(stage).has_value()) {
        outDiag.Message = fmt::format("pass does not declare an entry point for stage {}", stage);
        return std::nullopt;
    }
    const std::string_view sourcePath = pass.Source;
    if (sourcePath.empty()) {
        // 资产级 Source 的继承由调用方完成 (manifest 允许 pass.Source 留空)。
        outDiag.Message = "pass source path is empty; inherit ShaderAssetDesc::Source first";
        return std::nullopt;
    }

    // 源码身份两种模式下都尽力计算: Strict 用它判过期, Lenient 用它决定是否告警。
    // 发布包里源文件可能根本没有部署, 故 Lenient 下算不出来【不是】错误 —— 那时用
    // index 自称的身份作为 key 输入, 直接按逻辑 key 命中。
    ShaderHash sourceIdentity{};
    bool haveSourceIdentity = false;
    {
        ShaderAssetDiagnostic identityDiag;
        auto identity = GetSourceIdentity(sourcePath, identityDiag);
        if (identity.has_value()) {
            sourceIdentity = identity.value();
            haveSourceIdentity = true;
        } else if (_config.Staleness == ShaderArtifactStaleness::Strict && !CanJit()) {
            // Strict 且无法 JIT: 算不出身份就无从判断产物是否可用, 只能失败。
            outDiag.Message = identityDiag.Message;
            return std::nullopt;
        }
        // 其余情况落到下面: Strict + 能 JIT 走 JIT; Lenient 用 index 的身份。
    }

    Nullable<const ShaderArtifactIndex*> index = GetIndex();
    if (index != nullptr) {
        const ShaderArtifactIndex& idx = *index.Get();
        const bool toolchainMatches = idx.ToolchainHash == _toolchainHash;
        // 按【本 pass 的源文件】取 cook 时身份: 一个资产内各 pass 的 Source 可以不同,
        // 而 key 是按各自源文件算的。
        const std::optional<ShaderHash> cookedIdentity = idx.FindSourceIdentity(sourcePath);
        // Lenient 下用 index 自称的源码身份组 key, 从而绕过源码比对。
        const std::optional<ShaderHash> keyIdentity =
            _config.Staleness == ShaderArtifactStaleness::Strict
                ? (haveSourceIdentity ? std::optional{sourceIdentity} : std::nullopt)
                : cookedIdentity;
        const bool identityMatches =
            _config.Staleness == ShaderArtifactStaleness::Lenient
                ? cookedIdentity.has_value()
                : (haveSourceIdentity && cookedIdentity == sourceIdentity);

        if (toolchainMatches && identityMatches && keyIdentity.has_value()) {
            auto key = ComputeShaderArtifactKey(
                pass,
                stage,
                category,
                defines,
                keyIdentity.value(),
                _toolchainHash);
            if (key.has_value()) {
                ShaderAssetDiagnostic artifactDiag;
                auto bytecode = LoadFromArtifact(key.value(), stage, artifactDiag);
                if (bytecode.has_value()) {
                    if (_config.Staleness == ShaderArtifactStaleness::Lenient &&
                        haveSourceIdentity && cookedIdentity != sourceIdentity) {
                        RADRAY_WARN_LOG(
                            "shader artifact for pass '{}' stage {} is stale but accepted (Lenient)",
                            pass.Name,
                            stage);
                    }
                    return bytecode;
                }
                if (!artifactDiag.Message.empty()) {
                    // 产物损坏: 记日志, 但仍允许 JIT 兜底。
                    RADRAY_ERR_LOG("{}", artifactDiag.Message);
                }
            }
        } else if (!CanJit()) {
            outDiag.Message = fmt::format(
                "shader artifact index is unusable for source '{}' "
                "(toolchain match: {}, source match: {}) and JIT is unavailable",
                sourcePath,
                toolchainMatches,
                identityMatches);
            return std::nullopt;
        }
    }

    if (!CanJit()) {
        outDiag.Message = fmt::format(
            "no AOT artifact for pass '{}' stage {} and JIT is unavailable",
            pass.Name,
            stage);
        return std::nullopt;
    }

    // JIT 路径的 key 仍用真实源码身份 (Strict 下已算出); 算不出时留零值,
    // 因为 JIT 结果不落盘, key 只作诊断用途。
    auto key = ComputeShaderArtifactKey(
        pass,
        stage,
        category,
        defines,
        sourceIdentity,
        _toolchainHash);
    return CompileWithJit(
        pass,
        stage,
        category,
        defines,
        sourcePath,
        key.value_or(ShaderHash{}),
        outDiag);
}

// ============================ manifest 读写 ============================

std::optional<ShaderAssetDesc> ParseShaderAssetDesc(
    std::string_view json,
    ShaderAssetDiagnostic& outDiag) noexcept {
    outDiag = ShaderAssetDiagnostic{};
    ParseScope scope{outDiag};

    std::optional<JsonDocument> document = JsonDocument::Parse(json);
    if (!document.has_value()) {
        scope.Fail("shader asset JSON parse failed");
        return std::nullopt;
    }
    JsonValue root = document->Root();
    if (!root.IsObject()) {
        scope.Fail("shader asset root must be an object");
        return std::nullopt;
    }
    JsonValue versionJson = root["FormatVersion"];
    uint32_t formatVersion = 0;
    if (!versionJson.IsValid() || !DeserializeJsonValue(versionJson, formatVersion)) {
        scope.Fail("shader asset is missing 'FormatVersion'");
        return std::nullopt;
    }
    if (formatVersion != kShaderAssetFormatVersion) {
        scope.Fail(fmt::format(
            "shader asset FormatVersion {} != expected {}",
            formatVersion,
            kShaderAssetFormatVersion));
        return std::nullopt;
    }

    ShaderAssetDesc desc{};
    if (!DeserializeJsonValue(root, desc)) {
        // 保留最有用的一条旧诊断上下文：绑定枚举拼写错误应指出 pass / binding
        // 与原始字符串。其余 schema 错误由统一的类型解码失败诊断覆盖。
        const JsonValue passes = root["Passes"];
        if (passes.IsArray()) {
            for (size_t passIndex = 0; passIndex < passes.Size(); ++passIndex) {
                const JsonValue pass = passes.At(passIndex);
                scope.SetPass(pass["Name"].AsString());
                const JsonValue groups = pass["BindingGroups"];
                if (!groups.IsArray()) {
                    continue;
                }
                for (size_t groupIndex = 0; groupIndex < groups.Size(); ++groupIndex) {
                    const JsonValue group = groups.At(groupIndex);
                    if (group["Group"].IsNumber()) {
                        scope.SetGroup(static_cast<uint32_t>(group["Group"].AsUint()));
                    }
                    const JsonValue bindings = group["Bindings"];
                    if (!bindings.IsArray()) {
                        continue;
                    }
                    for (size_t bindingIndex = 0; bindingIndex < bindings.Size(); ++bindingIndex) {
                        const JsonValue binding = bindings.At(bindingIndex);
                        scope.SetBinding(binding["Name"].AsString());
                        if (binding["Binding"].IsNumber()) {
                            scope.SetBindingIndex(static_cast<uint32_t>(binding["Binding"].AsUint()));
                        }
                        const JsonValue type = binding["Type"];
                        if (type.IsString() &&
                            !EnumCast<render::ShaderParameterBindingType>(
                                 type.AsString())
                                 .has_value()) {
                            scope.Fail(fmt::format(
                                "field 'Type' has unknown value '{}'",
                                type.AsString()));
                            return std::nullopt;
                        }
                    }
                }
            }
        }
        scope.SetPass({});
        scope.SetGroup(std::nullopt);
        scope.ClearBinding();
        scope.SetStage(std::nullopt);
        scope.Fail("shader asset JSON does not match the declared schema");
        return std::nullopt;
    }

    if (!ValidateAsset(desc, scope)) {
        return std::nullopt;
    }
    return desc;
}

std::optional<ShaderAssetDesc> LoadShaderAssetDesc(
    const std::filesystem::path& path,
    ShaderAssetDiagnostic& outDiag) noexcept {
    outDiag = ShaderAssetDiagnostic{};

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        outDiag.Message = fmt::format("shader asset file does not exist: {}", path.generic_string());
        return std::nullopt;
    }
    const uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        outDiag.Message = fmt::format("cannot query shader asset file size: {}", path.generic_string());
        return std::nullopt;
    }

    string text;
    text.resize(static_cast<size_t>(size));
    std::FILE* file = nullptr;
#if defined(_WIN32)
    if (::_wfopen_s(&file, path.c_str(), L"rb") != 0) {
        file = nullptr;
    }
#else
    file = std::fopen(path.c_str(), "rb");
#endif
    if (file == nullptr) {
        outDiag.Message = fmt::format("cannot open shader asset file: {}", path.generic_string());
        return std::nullopt;
    }
    const size_t read = text.empty() ? 0 : std::fread(text.data(), 1, text.size(), file);
    std::fclose(file);
    if (read != text.size()) {
        outDiag.Message = fmt::format("truncated read of shader asset file: {}", path.generic_string());
        return std::nullopt;
    }
    return ParseShaderAssetDesc(text, outDiag);
}

std::optional<string> SerializeShaderAssetDesc(const ShaderAssetDesc& desc, bool pretty) noexcept {
    return SerializeJson(desc, pretty);
}

// ============================ 反射核对 ============================

bool ValidateShaderReflection(
    const ShaderPassDesc& pass,
    render::ShaderStage stage,
    const render::HlslShaderDesc& reflection,
    ShaderAssetDiagnostic& outDiag) noexcept {
    outDiag = ShaderAssetDiagnostic{};
    ParseScope scope{outDiag};
    scope.SetPass(pass.Name);
    scope.SetStage(stage);

    if (!pass.GetStageMask().HasFlag(stage)) {
        return scope.Fail(fmt::format("pass does not declare stage {}", StageName(stage)));
    }

    vector<ReflectedBinding> reflected;
    reflected.reserve(reflection.BoundResources.size());
    for (const render::HlslInputBindDesc& bind : reflection.BoundResources) {
        std::optional<ReflectedBinding> item = MakeReflectedBinding(bind);
        if (!item.has_value()) {
            scope.SetBinding(bind.Name);
            scope.SetGroup(bind.Space);
            scope.SetBindingIndex(bind.BindPoint);
            return scope.Fail(fmt::format(
                "reflection reports resource '{}' with a type that has no RHI binding equivalent",
                bind.Name));
        }
        reflected.push_back(item.value());
    }
    if (!MatchReflectedBindings(pass, stage, reflected, scope)) {
        return false;
    }

    // push constant 大小核对: DXIL 侧 push constant 表现为普通 cbuffer。
    //
    // 方向与本函数其余部分一致 (声明 ⊇ 反射): 反射用掉的字节数不得超出 manifest 声明。
    // D3D 反射的 cbuffer Size 已按 16 字节向上对齐, 而 manifest 只要求 4 字节对齐, 故
    // 比较前把声明值也对齐到 16 —— 否则任何 4/8/12 字节的 push constant 都会误报。
    if (pass.PushConstant.has_value()) {
        const ShaderPushConstantDesc& pc = pass.PushConstant.value();
        Nullable<const render::HlslShaderBufferDesc*> cbuffer = reflection.FindCBufferByName(pc.Name);
        if (cbuffer.HasValue()) {
            const uint32_t declared = static_cast<uint32_t>(Align(pc.Size, 16));
            if (cbuffer.Unwrap()->Size > declared) {
                scope.SetBinding(pc.Name);
                scope.SetGroup(pc.Location.Group);
                scope.SetBindingIndex(pc.Location.Binding);
                return scope.Fail(fmt::format(
                    "push constant size mismatch: reflection {} exceeds the manifest {} (16-byte aligned to {})",
                    cbuffer.Unwrap()->Size,
                    pc.Size,
                    declared));
            }
        }
    }

    // vertex input 核对: 只在 VS 且 manifest 声明了 VertexInput 时进行。
    if (stage == render::ShaderStage::Vertex && pass.VertexInput.has_value()) {
        scope.SetGroup(std::nullopt);
        scope.SetBindingIndex(std::nullopt);
        for (const render::HlslSignatureParameterDesc& input : reflection.InputParameters) {
            std::string_view baseName{};
            uint32_t nameIndex = 0;
            SplitSemantic(input.SemanticName, baseName, nameIndex);
            if (IsSystemSemantic(baseName)) {
                continue;
            }
            const uint32_t index = EffectiveSemanticIndex(input.SemanticName, input.SemanticIndex);
            const bool found = std::any_of(
                pass.VertexInput->Attributes.begin(),
                pass.VertexInput->Attributes.end(),
                [&](const ShaderVertexAttributeDesc& attribute) noexcept {
                    std::string_view declaredBase{};
                    uint32_t declaredIndex = 0;
                    SplitSemantic(attribute.Semantic, declaredBase, declaredIndex);
                    const uint32_t effective =
                        EffectiveSemanticIndex(attribute.Semantic, attribute.SemanticIndex);
                    return EqualsIgnoreCase(declaredBase, baseName) && effective == index;
                });
            if (!found) {
                scope.SetBinding(input.SemanticName);
                return scope.Fail(fmt::format(
                    "reflection reports vertex input semantic {}{} that the manifest does not declare",
                    baseName,
                    index));
            }
        }
    }
    return true;
}

bool ValidateShaderReflection(
    const ShaderPassDesc& pass,
    render::ShaderStage stage,
    const render::SpirvShaderDesc& reflection,
    ShaderAssetDiagnostic& outDiag) noexcept {
    outDiag = ShaderAssetDiagnostic{};
    ParseScope scope{outDiag};
    scope.SetPass(pass.Name);
    scope.SetStage(stage);

    if (!pass.GetStageMask().HasFlag(stage)) {
        return scope.Fail(fmt::format("pass does not declare stage {}", StageName(stage)));
    }

    vector<ReflectedBinding> reflected;
    reflected.reserve(reflection.ResourceBindings.size());
    for (const render::SpirvResourceBinding& bind : reflection.ResourceBindings) {
        if (bind.Kind == render::SpirvResourceKind::PushConstant) {
            continue;  // push constant 单独核对。
        }
        std::optional<ReflectedBinding> item = MakeReflectedBinding(bind);
        if (!item.has_value()) {
            scope.SetBinding(bind.Name);
            scope.SetGroup(bind.Set);
            scope.SetBindingIndex(bind.Binding);
            return scope.Fail(fmt::format(
                "reflection reports resource '{}' with a type that has no RHI binding equivalent",
                bind.Name));
        }
        reflected.push_back(item.value());
    }
    if (!MatchReflectedBindings(pass, stage, reflected, scope)) {
        return false;
    }

    // push constant: SPIRV 反射直接给出 range。
    if (!reflection.ConstantRanges.empty()) {
        if (!pass.PushConstant.has_value()) {
            scope.SetBinding(reflection.ConstantRanges.front().Name);
            return scope.Fail("reflection reports a push constant range but the manifest declares none");
        }
        if (reflection.ConstantRanges.size() > 1) {
            return scope.Fail(fmt::format(
                "reflection reports {} push constant ranges; at most one is supported",
                reflection.ConstantRanges.size()));
        }
        const render::SpirvPushConstantRange& range = reflection.ConstantRanges.front();
        const ShaderPushConstantDesc& pc = pass.PushConstant.value();
        scope.SetBinding(pc.Name);
        scope.SetGroup(pc.Location.Group);
        scope.SetBindingIndex(pc.Location.Binding);
        if (range.Offset + range.Size > pc.Size) {
            return scope.Fail(fmt::format(
                "push constant range [{}, {}) exceeds the manifest size {}",
                range.Offset,
                range.Offset + range.Size,
                pc.Size));
        }
        if (!pc.Stages.HasFlag(stage)) {
            return scope.Fail(fmt::format(
                "reflection uses the push constant in stage {} which the manifest does not declare",
                StageName(stage)));
        }
    }

    if (stage == render::ShaderStage::Vertex && pass.VertexInput.has_value()) {
        scope.SetGroup(std::nullopt);
        scope.SetBindingIndex(std::nullopt);
        for (const render::SpirvStageIo& input : reflection.StageInputs) {
            if (input.BuiltIn.has_value()) {
                continue;
            }
            const bool found = std::any_of(
                pass.VertexInput->Attributes.begin(),
                pass.VertexInput->Attributes.end(),
                [&](const ShaderVertexAttributeDesc& attribute) noexcept {
                    const size_t index = static_cast<size_t>(
                        &attribute - pass.VertexInput->Attributes.data());
                    const uint32_t location =
                        attribute.Location.value_or(static_cast<uint32_t>(index));
                    return location == input.Location;
                });
            if (!found) {
                scope.SetBinding(input.HlslSemantic.empty() ? input.Name : input.HlslSemantic);
                return scope.Fail(fmt::format(
                    "reflection reports vertex input at location {} that the manifest does not declare",
                    input.Location));
            }
        }
    }
    return true;
}

// ============================ 哈希与身份 ============================

ShaderHash HashShaderBytes(std::span<const byte> data) noexcept {
    HashAccum accum;
    accum.U64(data.size());
    accum.Bytes(data);
    return accum.Finish();
}

std::optional<ShaderSourceIdentity> ComputeShaderSourceIdentity(
    const std::filesystem::path& shaderRoot,
    std::string_view sourcePath,
    ShaderAssetDiagnostic& outDiag) noexcept {
    std::error_code error;
    const std::filesystem::path root = std::filesystem::weakly_canonical(shaderRoot, error);
    if (error || !std::filesystem::is_directory(root, error) || error) {
        outDiag.Message = fmt::format("shader root '{}' is unavailable", shaderRoot.string());
        return std::nullopt;
    }

    struct SourceFile {
        string IdentityPath;
        vector<byte> Content;
    };
    vector<SourceFile> files;
    vector<string> pending;
    pending.emplace_back(sourcePath);

    while (!pending.empty()) {
        const string requested = std::move(pending.back());
        pending.pop_back();

        const std::filesystem::path absolute = std::filesystem::weakly_canonical(
            root / std::filesystem::path{requested},
            error);
        if (error || !std::filesystem::is_regular_file(absolute, error) || error) {
            outDiag.Message = fmt::format("shader source '{}' is missing", requested);
            return std::nullopt;
        }
        if (!IsPathUnderRoot(root, absolute)) {
            outDiag.Message = fmt::format("shader source '{}' escapes the shader root", requested);
            return std::nullopt;
        }
        const string identityPath = absolute.lexically_relative(root).generic_string();
        if (std::ranges::any_of(files, [&](const SourceFile& item) {
                return item.IdentityPath == identityPath;
            })) {
            continue;
        }

        auto bytes = ReadBinaryFile(absolute);
        if (!bytes.has_value()) {
            outDiag.Message = fmt::format("failed to read shader source '{}'", identityPath);
            return std::nullopt;
        }
        const std::string_view text{
            reinterpret_cast<const char*>(bytes->data()),
            bytes->size()};
        vector<string> includes;
        string scanError;
        if (!ScanIncludes(text, includes, scanError)) {
            outDiag.Message = fmt::format("{} in '{}'", scanError, identityPath);
            return std::nullopt;
        }
        // shaderlib 是唯一 include 根 (见 AGENTS.md), 故 include 目标一律按 root 解析,
        // 不做相对当前文件的解析 —— 与 DXC 的 -I 行为一致。
        for (string& include : includes) {
            pending.emplace_back(std::move(include));
        }
        files.emplace_back(SourceFile{identityPath, std::move(bytes.value())});
    }

    // 按路径排序, 使哈希不受遍历顺序影响。
    std::ranges::sort(files, [](const SourceFile& lhs, const SourceFile& rhs) {
        return lhs.IdentityPath < rhs.IdentityPath;
    });

    ShaderSourceIdentity identity;
    HashAccum accum;
    accum.U32(kShaderArtifactFormatVersion);
    accum.U64(files.size());
    identity.Dependencies.reserve(files.size());
    for (const SourceFile& file : files) {
        accum.Text(file.IdentityPath);
        accum.U64(file.Content.size());
        accum.Bytes(file.Content);
        identity.Dependencies.emplace_back(file.IdentityPath);
    }
    identity.Hash = accum.Finish();
    return identity;
}

ShaderHash ComputeShaderArtifactKey(const ShaderArtifactKeyParams& params) noexcept {
    HashAccum accum;
    accum.U32(kShaderArtifactFormatVersion);
    accum.Hash(params.SourceIdentity);
    accum.Text(params.PassName);
    accum.U32(static_cast<uint32_t>(params.Stage));
    accum.Text(params.EntryPoint);
    accum.U32(static_cast<uint32_t>(params.ShaderModel));
    accum.U32(static_cast<uint32_t>(params.Category));
    accum.Byte(params.IsOptimize ? 1u : 0u);
    accum.Byte(params.EnableUnbounded ? 1u : 0u);
    accum.Hash(params.ToolchainHash);

    // 宏排序去重后入哈希: -DA -DB 与 -DB -DA 编出相同字节码, 必须得到同一个 key。
    vector<string> defines{params.Defines.begin(), params.Defines.end()};
    std::ranges::sort(defines);
    defines.erase(std::unique(defines.begin(), defines.end()), defines.end());
    accum.U64(defines.size());
    for (const string& define : defines) {
        accum.Text(define);
    }
    return accum.Finish();
}

std::optional<ShaderHash> ComputeShaderArtifactKey(
    const ShaderPassDesc& pass,
    render::ShaderStage stage,
    render::ShaderBlobCategory category,
    std::span<const string> defines,
    ShaderHash sourceIdentity,
    ShaderHash toolchainHash) noexcept {
    std::optional<std::string_view> entry = pass.FindEntryPoint(stage);
    if (!entry.has_value()) {
        return std::nullopt;
    }
    return ComputeShaderArtifactKey(ShaderArtifactKeyParams{
        .SourceIdentity = sourceIdentity,
        .PassName = pass.Name,
        .Stage = stage,
        .EntryPoint = entry.value(),
        .ShaderModel = pass.ShaderModel,
        .Category = category,
        .Defines = defines,
        .IsOptimize = pass.IsOptimize,
        .EnableUnbounded = pass.EnableUnbounded,
        .ToolchainHash = toolchainHash});
}

ShaderHash GetShaderToolchainHash() noexcept {
    // 用 artifact 层的字节哈希, 保证与 key 计算同一套算法。
    string material = fmt::format(
        "radray-shader-toolchain|dxc={}|artifact={}",
        kDxcVersion,
        kShaderArtifactFormatVersion);
    return HashShaderBytes(std::span{
        reinterpret_cast<const byte*>(material.data()),
        material.size()});
}

// ============================ 产物路径 ============================

std::filesystem::path GetShaderArtifactDirectory(const std::filesystem::path& manifestPath) {
    std::filesystem::path result = manifestPath;
    // "forward_pass.shader.json" 有两级后缀, 逐级剥到无后缀为止。
    while (result.has_extension()) {
        result.replace_extension();
    }
    return result;
}

string MakeShaderArtifactBlobPath(render::ShaderBlobCategory category, ShaderHash key) {
    const std::string_view dir = EnumNameOr(category, "unknown");
    string lower{dir};
    std::ranges::transform(lower, lower.begin(), [](char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    });
    return fmt::format("{}/{}.bin", lower, key.ToHex());
}

// ============================ 产物读写 ============================

bool WriteShaderArtifactBlob(
    const std::filesystem::path& path,
    const ShaderArtifactEntry& entry,
    std::span<const byte> bytecode) noexcept {
    if (bytecode.empty()) {
        return false;
    }
    BinaryWriter writer{bytecode.size() + 64};
    for (char c : kBlobMagic) {
        writer.U8(static_cast<uint8_t>(c));
    }
    writer.U32(kShaderArtifactFormatVersion);
    writer.U64(entry.Key.Low);
    writer.U64(entry.Key.High);
    writer.U32(static_cast<uint32_t>(entry.Stage));
    writer.I32(static_cast<int32_t>(entry.Category));
    const ShaderHash contentHash = HashShaderBytes(bytecode);
    writer.U64(contentHash.Low);
    writer.U64(contentHash.High);
    writer.SizedBytes(bytecode);
    return WriteBinaryFile(path, writer.GetData());
}

std::optional<ShaderArtifactBlob> ReadShaderArtifactBlob(
    const std::filesystem::path& path,
    ShaderAssetDiagnostic& outDiag) noexcept {
    auto bytes = ReadBinaryFile(path);
    if (!bytes.has_value()) {
        outDiag.Message = fmt::format("failed to read shader blob '{}'", path.string());
        return std::nullopt;
    }
    BinaryReader reader{bytes.value()};
    for (char expected : kBlobMagic) {
        uint8_t actual = 0;
        if (!reader.U8(actual) || actual != static_cast<uint8_t>(expected)) {
            outDiag.Message = fmt::format("shader blob '{}' has a bad magic", path.string());
            return std::nullopt;
        }
    }
    uint32_t version = 0;
    if (!reader.U32(version)) {
        outDiag.Message = fmt::format("shader blob '{}' is truncated", path.string());
        return std::nullopt;
    }
    if (version != kShaderArtifactFormatVersion) {
        outDiag.Message = fmt::format(
            "shader blob '{}' format version {} != expected {}",
            path.string(),
            version,
            kShaderArtifactFormatVersion);
        return std::nullopt;
    }

    ShaderArtifactBlob blob;
    uint32_t stage = 0;
    int32_t category = 0;
    ShaderHash storedHash{};
    std::span<const byte> payload;
    if (!reader.U64(blob.Key.Low) || !reader.U64(blob.Key.High) ||
        !reader.U32(stage) || !reader.I32(category) ||
        !reader.U64(storedHash.Low) || !reader.U64(storedHash.High) ||
        !reader.SizedBytes(payload)) {
        outDiag.Message = fmt::format("shader blob '{}' is truncated", path.string());
        return std::nullopt;
    }
    if (payload.empty()) {
        outDiag.Message = fmt::format("shader blob '{}' has empty bytecode", path.string());
        return std::nullopt;
    }
    if (HashShaderBytes(payload) != storedHash) {
        outDiag.Message = fmt::format("shader blob '{}' failed its content hash check", path.string());
        return std::nullopt;
    }
    blob.Stage = static_cast<render::ShaderStage>(stage);
    blob.Category = static_cast<render::ShaderBlobCategory>(category);
    // 复制到独立 vector: std::allocator 的对齐满足 Vulkan 对 SPIR-V 的 4 字节要求
    // (vkCreateShaderModule 侧会把指针 bit_cast 成 const uint32_t*)。
    blob.Bytecode.assign(payload.begin(), payload.end());
    return blob;
}

std::optional<ShaderArtifactIndex> ParseShaderArtifactIndex(
    std::string_view json,
    ShaderAssetDiagnostic& outDiag) noexcept {
    outDiag = ShaderAssetDiagnostic{};
    auto doc = JsonDocument::Parse(json);
    if (!doc.has_value()) {
        outDiag.Message = "shader artifact index is not valid JSON";
        return std::nullopt;
    }
    JsonValue root = doc->Root();
    if (!root.IsObject()) {
        outDiag.Message = "shader artifact index root must be an object";
        return std::nullopt;
    }

    JsonValue version = root["FormatVersion"];
    uint32_t formatVersion = 0;
    if (!version.IsValid() || !DeserializeJsonValue(version, formatVersion)) {
        outDiag.Message = "shader artifact index is missing 'FormatVersion'";
        return std::nullopt;
    }
    if (formatVersion != kShaderArtifactFormatVersion) {
        outDiag.Message = fmt::format(
            "shader artifact index FormatVersion {} != expected {}",
            formatVersion,
            kShaderArtifactFormatVersion);
        return std::nullopt;
    }

    ShaderArtifactIndex index{};
    if (!DeserializeJsonValue(root, index)) {
        outDiag.Message =
            "shader artifact index has malformed fields (Sources / Identity / "
            "ToolchainHash / Entries / Stage / Category / Keywords)";
        return std::nullopt;
    }

    if (index.Sources.empty()) {
        outDiag.Message = "shader artifact index 'Sources' must be a non-empty array";
        return std::nullopt;
    }
    for (size_t i = 0; i < index.Sources.size(); ++i) {
        const ShaderArtifactSource& source = index.Sources[i];
        if (source.Path.empty()) {
            outDiag.Message = fmt::format(
                "shader artifact Sources[{}] has a missing Path",
                i);
            return std::nullopt;
        }
        for (size_t j = 0; j < i; ++j) {
            if (index.Sources[j].Path == source.Path) {
                outDiag.Message = fmt::format(
                    "shader artifact index has a duplicate source '{}'",
                    source.Path);
                return std::nullopt;
            }
        }
    }

    for (size_t i = 0; i < index.Entries.size(); ++i) {
        const ShaderArtifactEntry& entry = index.Entries[i];
        if (entry.PassName.empty() || entry.EntryPoint.empty() || entry.BlobPath.empty()) {
            outDiag.Message = fmt::format(
                "shader artifact Entries[{}] is missing PassName / EntryPoint / BlobPath",
                i);
            return std::nullopt;
        }
        if (!index.FindSourceIdentity(entry.Source).has_value()) {
            // entry 的 key 是按该源文件的身份算的, 身份缺失说明 index 自相矛盾。
            outDiag.Message = fmt::format(
                "shader artifact Entries[{}] references source '{}' that 'Sources' does not record",
                i,
                entry.Source);
            return std::nullopt;
        }
        if (entry.BytecodeSize == 0) {
            outDiag.Message = fmt::format("shader artifact Entries[{}] has zero BytecodeSize", i);
            return std::nullopt;
        }
        for (size_t j = 0; j < i; ++j) {
            if (index.Entries[j].Key == entry.Key) {
                outDiag.Message = fmt::format(
                    "shader artifact index has a duplicate key {}",
                    entry.Key.ToHex());
                return std::nullopt;
            }
        }
    }
    return index;
}

std::optional<ShaderArtifactIndex> LoadShaderArtifactIndex(
    const std::filesystem::path& path,
    ShaderAssetDiagnostic& outDiag) noexcept {
    auto text = ReadTextFile(path);
    if (!text.has_value()) {
        outDiag.Message = fmt::format("failed to read shader artifact index '{}'", path.string());
        return std::nullopt;
    }
    return ParseShaderArtifactIndex(text.value(), outDiag);
}

std::optional<string> SerializeShaderArtifactIndex(
    const ShaderArtifactIndex& index,
    bool pretty) noexcept {
    return SerializeJson(index, pretty);
}

// ============================ AOT 烘焙 ============================
//
// 本节的内部辅助无法并入文件顶部那个匿名 namespace: 它们依赖 render::Dxc,
// 只在 RADRAY_ENABLE_SHADER_JIT 下才有定义。

#if defined(RADRAY_ENABLE_SHADER_JIT)

namespace {

ShaderAssetDiagnostic MakeDiag(
    string message,
    std::string_view passName,
    std::optional<render::ShaderStage> stage) {
    ShaderAssetDiagnostic diag;
    diag.Message = std::move(message);
    diag.PassName = string{passName};
    diag.Stage = stage;
    return diag;
}

/// 烘焙单个 (变体, stage) 所需的全部输入。字段全是借用, 生命周期由调用方保证。
struct CookStageInput {
    const ShaderPassDesc& Pass;
    const ShaderStageDesc& StageDesc;
    const ShaderVariantDomain& Domain;
    const ShaderVariantKey& Variant;
    render::ShaderBlobCategory Category{render::ShaderBlobCategory::DXIL};
    ShaderHash SourceIdentity{};
    std::string_view Source{};
    const std::filesystem::path& SourceFile;
    const std::filesystem::path& ArtifactDir;
    std::span<const std::string_view> Includes{};
};

/// 用反射核对 manifest。方向为声明 ⊇ 反射。
bool ValidateCompiled(
    render::Dxc& dxc,
    const ShaderPassDesc& pass,
    render::ShaderStage stage,
    render::ShaderBlobCategory category,
    const render::DxcOutput& output,
    ShaderAssetDiagnostic& outDiag) {
    if (category == render::ShaderBlobCategory::DXIL) {
        if (output.Refl.empty()) {
            outDiag = MakeDiag("DXIL output has no reflection blob", pass.Name, stage);
            return false;
        }
        auto reflection = dxc.GetShaderDescFromOutput(output.Refl);
        if (!reflection.has_value()) {
            outDiag = MakeDiag("failed to parse the DXIL reflection blob", pass.Name, stage);
            return false;
        }
        return ValidateShaderReflection(pass, stage, reflection.value(), outDiag);
    }
    if (category == render::ShaderBlobCategory::SPIRV) {
#if defined(RADRAY_ENABLE_SPIRV_CROSS)
        std::optional<std::string_view> entry = pass.FindEntryPoint(stage);
        if (!entry.has_value()) {
            outDiag = MakeDiag("pass does not declare an entry point", pass.Name, stage);
            return false;
        }
        // ReflectSpirv 不是 noexcept。按仓库异常政策不在此捕获: 反射失败属于
        // 工具链或输入的不变量破坏, 应当在 noexcept 边界终止而非降级为 false。
        auto reflection = render::ReflectSpirv(render::SpirvBytecodeView{
            .Data = output.Data,
            .EntryPointName = entry.value(),
            .Stage = stage});
        if (!reflection.has_value()) {
            outDiag = MakeDiag("failed to reflect the SPIR-V module", pass.Name, stage);
            return false;
        }
        return ValidateShaderReflection(pass, stage, reflection.value(), outDiag);
#else
        outDiag = MakeDiag(
            "SPIR-V reflection validation requires spirv-cross",
            pass.Name,
            stage);
        return false;
#endif
    }
    outDiag = MakeDiag(
        fmt::format("cannot validate reflection for category {}", category),
        pass.Name,
        stage);
    return false;
}

/// 烘焙一个 (变体, stage)。成功或"跳过"都返回 true; 只有硬错误返回 false,
/// 此时诊断已入 result。
bool CookStage(
    render::Dxc& dxc,
    const CookStageInput& input,
    const ShaderCookOptions& options,
    ShaderCookResult& result) noexcept {
    const ShaderPassDesc& pass = input.Pass;
    const render::ShaderStage stage = input.StageDesc.Stage;

    // stage 投影: 与本 stage 无关的 keyword 不进宏集合, 于是多个只在其他 stage 上
    // 不同的变体会算出同一个 key, 在下面被去重。
    const vector<string> defines = input.Domain.CollectDefines(input.Variant, stage);

    auto key = ComputeShaderArtifactKey(
        pass,
        stage,
        input.Category,
        defines,
        input.SourceIdentity,
        result.Index.ToolchainHash);
    if (!key.has_value()) {
        result.Diagnostics.push_back(
            MakeDiag("failed to compute an artifact key", pass.Name, stage));
        return false;
    }

    // 内容寻址带来的去重: 不同 pass / 不同变体投影到同一 key 时只留一份。
    if (result.Index.Find(key.value()).HasValue()) {
        ++result.Stats.Deduplicated;
        return true;
    }

    const vector<string> keywords = input.Domain.DescribeKeywords(input.Variant, stage);
    const string blobPath = MakeShaderArtifactBlobPath(input.Category, key.value());
    const std::filesystem::path absoluteBlob =
        input.ArtifactDir / std::filesystem::path{blobPath};

    ShaderArtifactEntry entry;
    entry.Key = key.value();
    entry.PassName = pass.Name;
    entry.Source = string{input.Source};
    entry.Stage = stage;
    entry.EntryPoint = input.StageDesc.EntryPoint;
    entry.Category = input.Category;
    entry.BlobPath = blobPath;
    entry.Keywords = keywords;

    // 增量: blob 已存在且自验通过则跳过编译。
    if (options.Incremental) {
        ShaderAssetDiagnostic probeDiag;
        auto existing = ReadShaderArtifactBlob(absoluteBlob, probeDiag);
        if (existing.has_value() && existing->Key == key.value()) {
            entry.BytecodeHash = HashShaderBytes(existing->Bytecode);
            entry.BytecodeSize = static_cast<uint32_t>(existing->Bytecode.size());
            result.Index.Entries.push_back(std::move(entry));
            ++result.Stats.Reused;
            return true;
        }
    }

    vector<std::string_view> defineViews;
    defineViews.reserve(defines.size());
    for (const string& define : defines) {
        defineViews.emplace_back(define);
    }
    const render::DxcCompileOptions compileOptions{
        .EntryPoint = input.StageDesc.EntryPoint,
        .Stage = stage,
        .SM = pass.ShaderModel,
        .Defines = defineViews,
        .Includes = input.Includes,
        .IsOptimize = pass.IsOptimize,
        .IsSpirv = input.Category == render::ShaderBlobCategory::SPIRV,
        .EnableUnbounded = pass.EnableUnbounded};

    auto output = dxc.CompileFile(input.SourceFile, compileOptions);
    if (!output.has_value() || output->Data.empty()) {
        result.Diagnostics.push_back(MakeDiag(
            keywords.empty()
                ? fmt::format("failed to compile '{}'", input.Source)
                : fmt::format(
                      "failed to compile '{}' with keywords [{}]",
                      input.Source,
                      fmt::join(keywords, ", ")),
            pass.Name,
            stage));
        return false;
    }
    if (output->Category != input.Category) {
        result.Diagnostics.push_back(MakeDiag(
            fmt::format(
                "compiling '{}' produced category {} but {} was requested",
                input.Source,
                output->Category,
                input.Category),
            pass.Name,
            stage));
        return false;
    }

    if (options.ValidateReflection) {
        ShaderAssetDiagnostic reflectionDiag;
        if (!ValidateCompiled(dxc, pass, stage, input.Category, output.value(), reflectionDiag)) {
            result.Diagnostics.push_back(std::move(reflectionDiag));
            return false;
        }
    }

    entry.BytecodeHash = HashShaderBytes(output->Data);
    entry.BytecodeSize = static_cast<uint32_t>(output->Data.size());
    if (!WriteShaderArtifactBlob(absoluteBlob, entry, output->Data)) {
        result.Diagnostics.push_back(MakeDiag(
            fmt::format("failed to write '{}'", blobPath),
            pass.Name,
            stage));
        return false;
    }
    result.Index.Entries.push_back(std::move(entry));
    ++result.Stats.Compiled;
    return true;
}

}  // namespace

ShaderCookResult CookShaderAsset(
    render::Dxc& dxc,
    const ShaderAssetDesc& asset,
    const ShaderCookOptions& options) noexcept {
    ShaderCookResult result;
    result.Index.AssetName = asset.Name;
    result.Index.ToolchainHash = GetShaderToolchainHash();

    if (options.Categories.empty()) {
        result.Diagnostics.push_back(MakeDiag("no target categories requested", {}, std::nullopt));
        return result;
    }

    const std::filesystem::path artifactDir = GetShaderArtifactDirectory(options.ManifestPath);

    // 每个源文件各自记一份身份。key 是按 pass 自己的源文件算的, 故 index 必须能按
    // 源文件回查 —— 合并成一个哈希会让多源资产在运行时永远查不中。
    vector<string> sourcePaths;
    for (const ShaderPassDesc& pass : asset.Passes) {
        const string source{GetEffectiveSource(asset, pass)};
        if (std::ranges::find(sourcePaths, source) == sourcePaths.end()) {
            sourcePaths.push_back(source);
        }
    }
    for (const string& source : sourcePaths) {
        ShaderAssetDiagnostic diag;
        auto identity = ComputeShaderSourceIdentity(options.ShaderRoot, source, diag);
        if (!identity.has_value()) {
            result.Diagnostics.push_back(std::move(diag));
            return result;
        }
        result.Index.Sources.push_back(ShaderArtifactSource{source, identity->Hash});
    }

    const string rootString = options.ShaderRoot.string();
    const std::array<std::string_view, 1> includes{rootString};

    for (const ShaderPassDesc& pass : asset.Passes) {
        const string source{GetEffectiveSource(asset, pass)};
        const std::optional<ShaderHash> cookedIdentity = result.Index.FindSourceIdentity(source);
        if (!cookedIdentity.has_value()) {
            result.Diagnostics.push_back(
                MakeDiag(fmt::format("no source identity for '{}'", source), pass.Name, std::nullopt));
            return result;
        }
        const ShaderHash sourceIdentity = cookedIdentity.value();
        const std::filesystem::path sourceFile =
            options.ShaderRoot / std::filesystem::path{source};

        ShaderAssetDiagnostic domainDiag;
        std::optional<ShaderVariantDomain> domain =
            ShaderVariantDomain::Build(asset, pass, domainDiag);
        if (!domain.has_value()) {
            result.Diagnostics.push_back(std::move(domainDiag));
            return result;
        }
        // pass 没有自己的烘焙声明时用的是资产级的, 那份规则可能引用本 pass 裁掉的
        // 组或 keyword, 按继承语义静默投影。
        const bool isInherited = pass.BakeVariants.IsEmpty();
        std::optional<vector<ShaderVariantKey>> variants = ExpandShaderBakeSet(
            domain.value(),
            GetEffectiveBakeSet(asset, pass),
            isInherited,
            domainDiag);
        if (!variants.has_value()) {
            result.Diagnostics.push_back(std::move(domainDiag));
            return result;
        }

        for (render::ShaderBlobCategory category : options.Categories) {
            if (category != render::ShaderBlobCategory::DXIL &&
                category != render::ShaderBlobCategory::SPIRV) {
                result.Diagnostics.push_back(MakeDiag(
                    fmt::format("shader cooking cannot produce category {}", category),
                    pass.Name,
                    std::nullopt));
                return result;
            }

            for (const ShaderVariantKey& variant : variants.value()) {
                for (const ShaderStageDesc& stageDesc : pass.Stages) {
                    const CookStageInput input{
                        .Pass = pass,
                        .StageDesc = stageDesc,
                        .Domain = domain.value(),
                        .Variant = variant,
                        .Category = category,
                        .SourceIdentity = sourceIdentity,
                        .Source = source,
                        .SourceFile = sourceFile,
                        .ArtifactDir = artifactDir,
                        .Includes = includes};
                    if (!CookStage(dxc, input, options, result)) {
                        return result;
                    }
                }
            }
        }
    }

    auto json = SerializeShaderArtifactIndex(result.Index);
    if (!json.has_value()) {
        result.Diagnostics.push_back(
            MakeDiag("failed to serialize the artifact index", {}, std::nullopt));
        return result;
    }
    if (!WriteTextFile(artifactDir / "index.json", json.value())) {
        result.Diagnostics.push_back(
            MakeDiag("failed to write the artifact index", {}, std::nullopt));
        return result;
    }
    return result;
}

ShaderCookResult CookShaderAssetFile(
    render::Dxc& dxc,
    const ShaderCookOptions& options) noexcept {
    ShaderCookResult result;
    ShaderAssetDiagnostic diag;
    auto asset = LoadShaderAssetDesc(options.ManifestPath, diag);
    if (!asset.has_value()) {
        result.Diagnostics.push_back(std::move(diag));
        return result;
    }
    return CookShaderAsset(dxc, asset.value(), options);
}

#endif

// ============================ 格式化 ============================

std::string_view format_as(ShaderBindingResidency v) noexcept {
    return EnumNameOr(v, "UNKNOWN");
}

std::string_view format_as(ShaderBytecodeSource v) noexcept {
    return EnumNameOr(v, "UNKNOWN");
}

std::string_view format_as(ShaderArtifactStaleness v) noexcept {
    return EnumNameOr(v, "UNKNOWN");
}

}  // namespace radray
