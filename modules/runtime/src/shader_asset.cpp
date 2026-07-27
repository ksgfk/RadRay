#include <radray/runtime/shader_asset.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <type_traits>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <radray/basic_math.h>
#include <radray/binary_io.h>
#include <radray/file.h>
#include <radray/json.h>
#include <radray/logger.h>
#include <radray/render/dxc.h>

#if defined(RADRAY_ENABLE_SHADER_JIT) && defined(RADRAY_ENABLE_SPIRV_CROSS)
#include <radray/render/spvc.h>
#endif

namespace radray {

// 文件组织: 先是全部内部实现细节 (单个匿名 namespace), 然后按
// 数据类型成员 -> 功能类成员 -> 自由函数 的顺序给出公开定义,
// 与 shader_asset.h 的声明顺序一致。

namespace {

// ============================ 字符串 <-> 枚举 ============================
//
// 名称与 C++ 枚举标识符保持一致, 便于人写人读。
// 刻意不复用 render::format_as: 那些是日志友好名 (例如 VertexFormat::UINT8X2 会打成
// "byte2"), 不适合做稳定的序列化 key。

template <class E>
struct EnumEntry {
    std::string_view Name;
    E Value;
};

template <class E, size_t N>
std::optional<E> LookupEnum(const std::array<EnumEntry<E>, N>& table, std::string_view name) noexcept {
    for (const EnumEntry<E>& entry : table) {
        if (entry.Name == name) {
            return entry.Value;
        }
    }
    return std::nullopt;
}

template <class E, size_t N>
std::string_view NameOfEnum(const std::array<EnumEntry<E>, N>& table, E value) noexcept {
    for (const EnumEntry<E>& entry : table) {
        if (entry.Value == value) {
            return entry.Name;
        }
    }
    return {};
}

constexpr std::array kResidencyTable{
    EnumEntry<ShaderBindingResidency>{"DescriptorTable", ShaderBindingResidency::DescriptorTable},
    EnumEntry<ShaderBindingResidency>{"RootDescriptor", ShaderBindingResidency::RootDescriptor},
};

constexpr std::array kBindingTypeTable{
    EnumEntry<render::ShaderParameterBindingType>{"CBuffer", render::ShaderParameterBindingType::CBuffer},
    EnumEntry<render::ShaderParameterBindingType>{"Buffer", render::ShaderParameterBindingType::Buffer},
    EnumEntry<render::ShaderParameterBindingType>{"RWBuffer", render::ShaderParameterBindingType::RWBuffer},
    EnumEntry<render::ShaderParameterBindingType>{"TexelBuffer", render::ShaderParameterBindingType::TexelBuffer},
    EnumEntry<render::ShaderParameterBindingType>{"RWTexelBuffer", render::ShaderParameterBindingType::RWTexelBuffer},
    EnumEntry<render::ShaderParameterBindingType>{"Texture", render::ShaderParameterBindingType::Texture},
    EnumEntry<render::ShaderParameterBindingType>{"RWTexture", render::ShaderParameterBindingType::RWTexture},
    EnumEntry<render::ShaderParameterBindingType>{"Sampler", render::ShaderParameterBindingType::Sampler},
};

constexpr std::array kShaderStageTable{
    EnumEntry<render::ShaderStage>{"Vertex", render::ShaderStage::Vertex},
    EnumEntry<render::ShaderStage>{"Pixel", render::ShaderStage::Pixel},
    EnumEntry<render::ShaderStage>{"Compute", render::ShaderStage::Compute},
};

/// artifact index.json 也是人可读可 diff 的产物清单, 与 manifest 共用同一套字符串表。
constexpr std::array kBlobCategoryTable{
    EnumEntry<render::ShaderBlobCategory>{"DXIL", render::ShaderBlobCategory::DXIL},
    EnumEntry<render::ShaderBlobCategory>{"SPIRV", render::ShaderBlobCategory::SPIRV},
    EnumEntry<render::ShaderBlobCategory>{"MSL", render::ShaderBlobCategory::MSL},
    EnumEntry<render::ShaderBlobCategory>{"METALLIB", render::ShaderBlobCategory::METALLIB},
};

/// 诊断文本用的 stage 名。与序列化表同源, 保证 JSON 里的名字和报错里的名字一致。
std::string_view StageName(render::ShaderStage stage) noexcept {
    const std::string_view name = NameOfEnum(kShaderStageTable, stage);
    return name.empty() ? std::string_view{"UNKNOWN"} : name;
}

constexpr std::array kShaderModelTable{
    EnumEntry<render::HlslShaderModel>{"SM60", render::HlslShaderModel::SM60},
    EnumEntry<render::HlslShaderModel>{"SM61", render::HlslShaderModel::SM61},
    EnumEntry<render::HlslShaderModel>{"SM62", render::HlslShaderModel::SM62},
    EnumEntry<render::HlslShaderModel>{"SM63", render::HlslShaderModel::SM63},
    EnumEntry<render::HlslShaderModel>{"SM64", render::HlslShaderModel::SM64},
    EnumEntry<render::HlslShaderModel>{"SM65", render::HlslShaderModel::SM65},
    EnumEntry<render::HlslShaderModel>{"SM66", render::HlslShaderModel::SM66},
};

constexpr std::array kVertexStepModeTable{
    EnumEntry<render::VertexStepMode>{"Vertex", render::VertexStepMode::Vertex},
    EnumEntry<render::VertexStepMode>{"Instance", render::VertexStepMode::Instance},
};

constexpr std::array kVertexFormatTable{
    EnumEntry<render::VertexFormat>{"UINT8X2", render::VertexFormat::UINT8X2},
    EnumEntry<render::VertexFormat>{"UINT8X4", render::VertexFormat::UINT8X4},
    EnumEntry<render::VertexFormat>{"SINT8X2", render::VertexFormat::SINT8X2},
    EnumEntry<render::VertexFormat>{"SINT8X4", render::VertexFormat::SINT8X4},
    EnumEntry<render::VertexFormat>{"UNORM8X2", render::VertexFormat::UNORM8X2},
    EnumEntry<render::VertexFormat>{"UNORM8X4", render::VertexFormat::UNORM8X4},
    EnumEntry<render::VertexFormat>{"SNORM8X2", render::VertexFormat::SNORM8X2},
    EnumEntry<render::VertexFormat>{"SNORM8X4", render::VertexFormat::SNORM8X4},
    EnumEntry<render::VertexFormat>{"UINT16X2", render::VertexFormat::UINT16X2},
    EnumEntry<render::VertexFormat>{"UINT16X4", render::VertexFormat::UINT16X4},
    EnumEntry<render::VertexFormat>{"SINT16X2", render::VertexFormat::SINT16X2},
    EnumEntry<render::VertexFormat>{"SINT16X4", render::VertexFormat::SINT16X4},
    EnumEntry<render::VertexFormat>{"UNORM16X2", render::VertexFormat::UNORM16X2},
    EnumEntry<render::VertexFormat>{"UNORM16X4", render::VertexFormat::UNORM16X4},
    EnumEntry<render::VertexFormat>{"SNORM16X2", render::VertexFormat::SNORM16X2},
    EnumEntry<render::VertexFormat>{"SNORM16X4", render::VertexFormat::SNORM16X4},
    EnumEntry<render::VertexFormat>{"FLOAT16X2", render::VertexFormat::FLOAT16X2},
    EnumEntry<render::VertexFormat>{"FLOAT16X4", render::VertexFormat::FLOAT16X4},
    EnumEntry<render::VertexFormat>{"UINT32", render::VertexFormat::UINT32},
    EnumEntry<render::VertexFormat>{"UINT32X2", render::VertexFormat::UINT32X2},
    EnumEntry<render::VertexFormat>{"UINT32X3", render::VertexFormat::UINT32X3},
    EnumEntry<render::VertexFormat>{"UINT32X4", render::VertexFormat::UINT32X4},
    EnumEntry<render::VertexFormat>{"SINT32", render::VertexFormat::SINT32},
    EnumEntry<render::VertexFormat>{"SINT32X2", render::VertexFormat::SINT32X2},
    EnumEntry<render::VertexFormat>{"SINT32X3", render::VertexFormat::SINT32X3},
    EnumEntry<render::VertexFormat>{"SINT32X4", render::VertexFormat::SINT32X4},
    EnumEntry<render::VertexFormat>{"FLOAT32", render::VertexFormat::FLOAT32},
    EnumEntry<render::VertexFormat>{"FLOAT32X2", render::VertexFormat::FLOAT32X2},
    EnumEntry<render::VertexFormat>{"FLOAT32X3", render::VertexFormat::FLOAT32X3},
    EnumEntry<render::VertexFormat>{"FLOAT32X4", render::VertexFormat::FLOAT32X4},
};

constexpr std::array kAddressModeTable{
    EnumEntry<render::AddressMode>{"ClampToEdge", render::AddressMode::ClampToEdge},
    EnumEntry<render::AddressMode>{"Repeat", render::AddressMode::Repeat},
    EnumEntry<render::AddressMode>{"Mirror", render::AddressMode::Mirror},
};

constexpr std::array kFilterModeTable{
    EnumEntry<render::FilterMode>{"Nearest", render::FilterMode::Nearest},
    EnumEntry<render::FilterMode>{"Linear", render::FilterMode::Linear},
};

constexpr std::array kCompareFunctionTable{
    EnumEntry<render::CompareFunction>{"Never", render::CompareFunction::Never},
    EnumEntry<render::CompareFunction>{"Less", render::CompareFunction::Less},
    EnumEntry<render::CompareFunction>{"Equal", render::CompareFunction::Equal},
    EnumEntry<render::CompareFunction>{"LessEqual", render::CompareFunction::LessEqual},
    EnumEntry<render::CompareFunction>{"Greater", render::CompareFunction::Greater},
    EnumEntry<render::CompareFunction>{"NotEqual", render::CompareFunction::NotEqual},
    EnumEntry<render::CompareFunction>{"GreaterEqual", render::CompareFunction::GreaterEqual},
    EnumEntry<render::CompareFunction>{"Always", render::CompareFunction::Always},
};

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

bool ReadRequiredString(const JsonValue& obj, std::string_view key, ParseScope& scope, string& out) noexcept {
    JsonValue value = obj[key];
    if (!value.IsString()) {
        return scope.Fail(fmt::format("missing or non-string field '{}'", key));
    }
    out = string{value.AsString()};
    if (out.empty()) {
        return scope.Fail(fmt::format("field '{}' must not be empty", key));
    }
    return true;
}

bool ReadOptionalString(const JsonValue& obj, std::string_view key, ParseScope& scope, string& out) noexcept {
    if (!obj.Has(key)) {
        return true;
    }
    JsonValue value = obj[key];
    if (!value.IsString()) {
        return scope.Fail(fmt::format("field '{}' must be a string", key));
    }
    out = string{value.AsString()};
    return true;
}

bool ReadRequiredUint32(const JsonValue& obj, std::string_view key, ParseScope& scope, uint32_t& out) noexcept {
    JsonValue value = obj[key];
    if (!value.IsNumber()) {
        return scope.Fail(fmt::format("missing or non-numeric field '{}'", key));
    }
    const uint64_t raw = value.AsUint();
    if (raw > std::numeric_limits<uint32_t>::max()) {
        return scope.Fail(fmt::format("field '{}' exceeds uint32 range: {}", key, raw));
    }
    out = static_cast<uint32_t>(raw);
    return true;
}

bool ReadOptionalUint32(const JsonValue& obj, std::string_view key, ParseScope& scope, uint32_t& out) noexcept {
    if (!obj.Has(key)) {
        return true;
    }
    return ReadRequiredUint32(obj, key, scope, out);
}

bool ReadOptionalBool(const JsonValue& obj, std::string_view key, ParseScope& scope, bool& out) noexcept {
    if (!obj.Has(key)) {
        return true;
    }
    JsonValue value = obj[key];
    if (!value.IsBool()) {
        return scope.Fail(fmt::format("field '{}' must be a bool", key));
    }
    out = value.AsBool();
    return true;
}

bool ReadOptionalFloat(const JsonValue& obj, std::string_view key, ParseScope& scope, float& out) noexcept {
    if (!obj.Has(key)) {
        return true;
    }
    JsonValue value = obj[key];
    if (!value.IsNumber()) {
        return scope.Fail(fmt::format("field '{}' must be a number", key));
    }
    out = static_cast<float>(value.AsDouble());
    return true;
}

template <class E, size_t N>
bool ReadRequiredEnum(
    const JsonValue& obj,
    std::string_view key,
    const std::array<EnumEntry<E>, N>& table,
    ParseScope& scope,
    E& out) noexcept {
    JsonValue value = obj[key];
    if (!value.IsString()) {
        return scope.Fail(fmt::format("missing or non-string enum field '{}'", key));
    }
    const std::string_view name = value.AsString();
    std::optional<E> parsed = LookupEnum(table, name);
    if (!parsed.has_value()) {
        return scope.Fail(fmt::format("field '{}' has unknown value '{}'", key, name));
    }
    out = parsed.value();
    return true;
}

template <class E, size_t N>
bool ReadOptionalEnum(
    const JsonValue& obj,
    std::string_view key,
    const std::array<EnumEntry<E>, N>& table,
    ParseScope& scope,
    E& out) noexcept {
    if (!obj.Has(key)) {
        return true;
    }
    return ReadRequiredEnum(obj, key, table, scope, out);
}

/// ShaderStages 序列化为字符串数组, 与 EnumFlags 语义一致。
bool ReadRequiredStages(
    const JsonValue& obj,
    std::string_view key,
    ParseScope& scope,
    render::ShaderStages& out) noexcept {
    JsonValue value = obj[key];
    if (!value.IsArray()) {
        return scope.Fail(fmt::format("missing or non-array field '{}'", key));
    }
    const size_t count = value.Size();
    if (count == 0) {
        return scope.Fail(fmt::format("field '{}' must list at least one shader stage", key));
    }
    render::ShaderStages stages{render::ShaderStage::UNKNOWN};
    for (size_t i = 0; i < count; ++i) {
        JsonValue element = value.At(i);
        if (!element.IsString()) {
            return scope.Fail(fmt::format("field '{}'[{}] must be a string", key, i));
        }
        const std::string_view name = element.AsString();
        std::optional<render::ShaderStage> stage = LookupEnum(kShaderStageTable, name);
        if (!stage.has_value()) {
            return scope.Fail(fmt::format("field '{}'[{}] has unknown shader stage '{}'", key, i, name));
        }
        stages |= stage.value();
    }
    out = stages;
    return true;
}

bool ReadOptionalStages(
    const JsonValue& obj,
    std::string_view key,
    ParseScope& scope,
    render::ShaderStages& out) noexcept {
    if (!obj.Has(key)) {
        return true;
    }
    return ReadRequiredStages(obj, key, scope, out);
}

bool ReadStringArray(
    const JsonValue& obj,
    std::string_view key,
    ParseScope& scope,
    bool allowEmptyElements,
    vector<string>& out) noexcept {
    if (!obj.Has(key)) {
        return true;
    }
    JsonValue value = obj[key];
    if (!value.IsArray()) {
        return scope.Fail(fmt::format("field '{}' must be an array", key));
    }
    const size_t count = value.Size();
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        JsonValue element = value.At(i);
        if (!element.IsString()) {
            return scope.Fail(fmt::format("field '{}'[{}] must be a string", key, i));
        }
        string text{element.AsString()};
        if (!allowEmptyElements && text.empty()) {
            return scope.Fail(fmt::format("field '{}'[{}] must not be empty", key, i));
        }
        out.push_back(std::move(text));
    }
    return true;
}

bool ReadSampler(const JsonValue& obj, ParseScope& scope, render::SamplerDescriptor& out) noexcept {
    if (!obj.IsObject()) {
        return scope.Fail("ImmutableSampler must be an object");
    }
    render::SamplerDescriptor sampler{};
    if (!ReadRequiredEnum(obj, "AddressS", kAddressModeTable, scope, sampler.AddressS) ||
        !ReadRequiredEnum(obj, "AddressT", kAddressModeTable, scope, sampler.AddressT) ||
        !ReadRequiredEnum(obj, "AddressR", kAddressModeTable, scope, sampler.AddressR) ||
        !ReadRequiredEnum(obj, "MinFilter", kFilterModeTable, scope, sampler.MinFilter) ||
        !ReadRequiredEnum(obj, "MagFilter", kFilterModeTable, scope, sampler.MagFilter) ||
        !ReadRequiredEnum(obj, "MipmapFilter", kFilterModeTable, scope, sampler.MipmapFilter) ||
        !ReadOptionalFloat(obj, "LodMin", scope, sampler.LodMin) ||
        !ReadOptionalFloat(obj, "LodMax", scope, sampler.LodMax) ||
        !ReadOptionalUint32(obj, "AnisotropyClamp", scope, sampler.AnisotropyClamp)) {
        return false;
    }
    if (obj.Has("Compare")) {
        render::CompareFunction compare{};
        if (!ReadRequiredEnum(obj, "Compare", kCompareFunctionTable, scope, compare)) {
            return false;
        }
        sampler.Compare = compare;
    }
    if (sampler.LodMax < sampler.LodMin) {
        return scope.Fail(fmt::format(
            "ImmutableSampler LodMax {} is less than LodMin {}",
            sampler.LodMax,
            sampler.LodMin));
    }
    out = sampler;
    return true;
}

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

/// manifest 声明的 Type + Residency 折叠为 RHI 的 ShaderParameterBindingType。
/// RHI 把"类型"与"驻留"合并进一个枚举 (Dynamic* 变体), manifest 保持两者正交。
render::ShaderParameterBindingType ResolveBindingType(const ShaderBindingDesc& binding) noexcept {
    if (binding.Residency != ShaderBindingResidency::RootDescriptor) {
        return binding.Type;
    }
    switch (binding.Type) {
        case render::ShaderParameterBindingType::CBuffer:
            return render::ShaderParameterBindingType::DynamicCBuffer;
        case render::ShaderParameterBindingType::Buffer:
            return render::ShaderParameterBindingType::DynamicBuffer;
        case render::ShaderParameterBindingType::RWBuffer:
            return render::ShaderParameterBindingType::DynamicRWBuffer;
        default:
            return binding.Type;
    }
}

bool ValidateBinding(const ShaderBindingDesc& binding, ParseScope& scope) noexcept {
    scope.SetBinding(binding.Name);
    scope.SetBindingIndex(binding.Binding);

    if (binding.Type == render::ShaderParameterBindingType::UNKNOWN) {
        return scope.Fail("binding Type must be declared");
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
                NameOfEnum(kBindingTypeTable, binding.Type)));
        }
        if (binding.Count != 1) {
            return scope.Fail(fmt::format("RootDescriptor residency requires Count 1, got {}", binding.Count));
        }
    }
    if (binding.ImmutableSampler.has_value()) {
        if (binding.Type != render::ShaderParameterBindingType::Sampler) {
            return scope.Fail("ImmutableSampler requires Sampler type");
        }
        if (binding.Count != 1) {
            return scope.Fail(fmt::format("ImmutableSampler requires Count 1, got {}", binding.Count));
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

// ============================ JSON -> 结构 ============================

bool ReadBinding(const JsonValue& obj, ParseScope& scope, ShaderBindingDesc& out) noexcept {
    if (!obj.IsObject()) {
        return scope.Fail("binding must be an object");
    }
    if (!ReadRequiredString(obj, "Name", scope, out.Name)) {
        return false;
    }
    scope.SetBinding(out.Name);
    if (!ReadRequiredUint32(obj, "Binding", scope, out.Binding)) {
        return false;
    }
    scope.SetBindingIndex(out.Binding);
    if (!ReadRequiredEnum(obj, "Type", kBindingTypeTable, scope, out.Type) ||
        !ReadOptionalUint32(obj, "Count", scope, out.Count) ||
        !ReadRequiredStages(obj, "Stages", scope, out.Stages) ||
        !ReadOptionalEnum(obj, "Residency", kResidencyTable, scope, out.Residency)) {
        return false;
    }
    if (obj.Has("ImmutableSampler")) {
        render::SamplerDescriptor sampler{};
        if (!ReadSampler(obj["ImmutableSampler"], scope, sampler)) {
            return false;
        }
        out.ImmutableSampler = sampler;
    }
    return true;
}

bool ReadBindingGroup(const JsonValue& obj, ParseScope& scope, ShaderBindingGroupDesc& out) noexcept {
    if (!obj.IsObject()) {
        return scope.Fail("BindingGroups element must be an object");
    }
    if (!ReadRequiredUint32(obj, "Group", scope, out.Group)) {
        return false;
    }
    scope.SetGroup(out.Group);
    JsonValue bindings = obj["Bindings"];
    if (!bindings.IsArray()) {
        return scope.Fail("binding group must have a 'Bindings' array");
    }
    const size_t count = bindings.Size();
    out.Bindings.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        ShaderBindingDesc binding{};
        if (!ReadBinding(bindings.At(i), scope, binding)) {
            return false;
        }
        out.Bindings.push_back(std::move(binding));
    }
    scope.ClearBinding();
    return true;
}

bool ReadPushConstant(const JsonValue& obj, ParseScope& scope, ShaderPushConstantDesc& out) noexcept {
    if (!obj.IsObject()) {
        return scope.Fail("PushConstant must be an object");
    }
    if (!ReadRequiredString(obj, "Name", scope, out.Name)) {
        return false;
    }
    scope.SetBinding(out.Name);
    JsonValue location = obj["Location"];
    if (!location.IsObject()) {
        return scope.Fail("PushConstant must have a 'Location' object");
    }
    if (!ReadRequiredUint32(location, "Group", scope, out.Location.Group) ||
        !ReadRequiredUint32(location, "Binding", scope, out.Location.Binding) ||
        !ReadRequiredUint32(obj, "Size", scope, out.Size) ||
        !ReadRequiredStages(obj, "Stages", scope, out.Stages)) {
        return false;
    }
    scope.ClearBinding();
    return true;
}

bool ReadVertexInput(const JsonValue& obj, ParseScope& scope, ShaderVertexInputDesc& out) noexcept {
    if (!obj.IsObject()) {
        return scope.Fail("VertexInput must be an object");
    }
    JsonValue buffers = obj["Buffers"];
    if (!buffers.IsArray()) {
        return scope.Fail("VertexInput must have a 'Buffers' array");
    }
    out.Buffers.reserve(buffers.Size());
    for (size_t i = 0; i < buffers.Size(); ++i) {
        JsonValue element = buffers.At(i);
        if (!element.IsObject()) {
            return scope.Fail(fmt::format("VertexInput Buffers[{}] must be an object", i));
        }
        ShaderVertexBufferDesc buffer{};
        if (!ReadRequiredUint32(element, "Binding", scope, buffer.Binding) ||
            !ReadRequiredUint32(element, "ArrayStride", scope, buffer.ArrayStride) ||
            !ReadOptionalEnum(element, "StepMode", kVertexStepModeTable, scope, buffer.StepMode)) {
            return false;
        }
        out.Buffers.push_back(buffer);
    }
    JsonValue attributes = obj["Attributes"];
    if (!attributes.IsArray()) {
        return scope.Fail("VertexInput must have an 'Attributes' array");
    }
    out.Attributes.reserve(attributes.Size());
    for (size_t i = 0; i < attributes.Size(); ++i) {
        JsonValue element = attributes.At(i);
        if (!element.IsObject()) {
            return scope.Fail(fmt::format("VertexInput Attributes[{}] must be an object", i));
        }
        ShaderVertexAttributeDesc attribute{};
        if (!ReadRequiredString(element, "Semantic", scope, attribute.Semantic)) {
            return false;
        }
        scope.SetBinding(attribute.Semantic);
        if (!ReadOptionalUint32(element, "SemanticIndex", scope, attribute.SemanticIndex) ||
            !ReadRequiredEnum(element, "Format", kVertexFormatTable, scope, attribute.Format) ||
            !ReadOptionalUint32(element, "BufferBinding", scope, attribute.BufferBinding) ||
            !ReadOptionalUint32(element, "Offset", scope, attribute.Offset)) {
            return false;
        }
        if (element.Has("Location")) {
            uint32_t location = 0;
            if (!ReadRequiredUint32(element, "Location", scope, location)) {
                return false;
            }
            attribute.Location = location;
        }
        out.Attributes.push_back(std::move(attribute));
    }
    scope.ClearBinding();
    return true;
}

bool ReadBakeSet(const JsonValue& obj, ParseScope& scope, ShaderBakeSetDesc& out) noexcept;

bool ReadPass(const JsonValue& obj, ParseScope& scope, ShaderPassDesc& out) noexcept {
    if (!obj.IsObject()) {
        return scope.Fail("Passes element must be an object");
    }
    if (!ReadRequiredString(obj, "Name", scope, out.Name)) {
        return false;
    }
    scope.SetPass(out.Name);
    if (!ReadOptionalString(obj, "Source", scope, out.Source)) {
        return false;
    }

    JsonValue stages = obj["Stages"];
    if (!stages.IsArray() || stages.Size() == 0) {
        return scope.Fail("pass must have a non-empty 'Stages' array");
    }
    out.Stages.reserve(stages.Size());
    for (size_t i = 0; i < stages.Size(); ++i) {
        JsonValue element = stages.At(i);
        if (!element.IsObject()) {
            return scope.Fail(fmt::format("pass Stages[{}] must be an object", i));
        }
        ShaderStageDesc stage{};
        if (!ReadRequiredEnum(element, "Stage", kShaderStageTable, scope, stage.Stage)) {
            return false;
        }
        scope.SetStage(stage.Stage);
        if (!ReadRequiredString(element, "EntryPoint", scope, stage.EntryPoint)) {
            return false;
        }
        scope.SetStage(std::nullopt);
        out.Stages.push_back(std::move(stage));
    }

    if (!ReadOptionalEnum(obj, "ShaderModel", kShaderModelTable, scope, out.ShaderModel) ||
        !ReadStringArray(obj, "Defines", scope, false, out.Defines) ||
        !ReadStringArray(obj, "KeywordGroups", scope, false, out.KeywordGroups) ||
        !ReadOptionalBool(obj, "IsOptimize", scope, out.IsOptimize) ||
        !ReadOptionalBool(obj, "EnableUnbounded", scope, out.EnableUnbounded)) {
        return false;
    }

    if (obj.Has("BindingGroups")) {
        JsonValue groups = obj["BindingGroups"];
        if (!groups.IsArray()) {
            return scope.Fail("'BindingGroups' must be an array");
        }
        out.BindingGroups.reserve(groups.Size());
        for (size_t i = 0; i < groups.Size(); ++i) {
            ShaderBindingGroupDesc group{};
            if (!ReadBindingGroup(groups.At(i), scope, group)) {
                return false;
            }
            out.BindingGroups.push_back(std::move(group));
        }
        scope.SetGroup(std::nullopt);
    }

    if (obj.Has("PushConstant")) {
        ShaderPushConstantDesc pc{};
        if (!ReadPushConstant(obj["PushConstant"], scope, pc)) {
            return false;
        }
        out.PushConstant = std::move(pc);
    }

    if (obj.Has("VertexInput")) {
        ShaderVertexInputDesc vi{};
        if (!ReadVertexInput(obj["VertexInput"], scope, vi)) {
            return false;
        }
        out.VertexInput = std::move(vi);
    }

    if (obj.Has("BakeVariants")) {
        if (!ReadBakeSet(obj["BakeVariants"], scope, out.BakeVariants)) {
            return false;
        }
    }
    return true;
}

bool ReadBakeSet(const JsonValue& obj, ParseScope& scope, ShaderBakeSetDesc& out) noexcept {
    if (!obj.IsObject()) {
        return scope.Fail("'BakeVariants' must be an object");
    }
    if (obj.Has("Rules")) {
        JsonValue rules = obj["Rules"];
        if (!rules.IsArray()) {
            return scope.Fail("BakeVariants 'Rules' must be an array");
        }
        out.Rules.reserve(rules.Size());
        for (size_t i = 0; i < rules.Size(); ++i) {
            JsonValue element = rules.At(i);
            if (!element.IsObject()) {
                return scope.Fail(fmt::format("BakeVariants Rules[{}] must be an object", i));
            }
            ShaderBakeRuleDesc rule{};
            if (!ReadStringArray(element, "Expand", scope, false, rule.Expand) ||
                !ReadStringArray(element, "Combination", scope, false, rule.Combination)) {
                return false;
            }
            out.Rules.push_back(std::move(rule));
        }
    }
    if (obj.Has("Skip")) {
        JsonValue skip = obj["Skip"];
        if (!skip.IsArray()) {
            return scope.Fail("BakeVariants 'Skip' must be an array");
        }
        out.Skip.reserve(skip.Size());
        for (size_t i = 0; i < skip.Size(); ++i) {
            JsonValue element = skip.At(i);
            if (!element.IsArray()) {
                return scope.Fail(fmt::format("BakeVariants Skip[{}] must be an array of keywords", i));
            }
            vector<string> keywords;
            keywords.reserve(element.Size());
            for (size_t j = 0; j < element.Size(); ++j) {
                JsonValue keyword = element.At(j);
                if (!keyword.IsString()) {
                    return scope.Fail(fmt::format("BakeVariants Skip[{}][{}] must be a string", i, j));
                }
                string text{keyword.AsString()};
                if (text.empty()) {
                    return scope.Fail(fmt::format("BakeVariants Skip[{}][{}] must not be empty", i, j));
                }
                keywords.push_back(std::move(text));
            }
            out.Skip.push_back(std::move(keywords));
        }
    }
    return true;
}

bool ReadKeywordGroup(const JsonValue& obj, ParseScope& scope, ShaderKeywordGroupDesc& out) noexcept {
    if (!obj.IsObject()) {
        return scope.Fail("KeywordGroups element must be an object");
    }
    if (!ReadRequiredString(obj, "Name", scope, out.Name) ||
        !ReadStringArray(obj, "Keywords", scope, true, out.Keywords) ||
        !ReadOptionalBool(obj, "IsOptional", scope, out.IsOptional) ||
        !ReadOptionalStages(obj, "Stages", scope, out.Stages)) {
        return false;
    }
    return true;
}

// ============================ 反射映射 ============================

/// DXIL 的 D3D_SHADER_INPUT_TYPE -> RHI 绑定类型。
/// 注意 D3D 不区分 Buffer / TexelBuffer 的采样语义, 只能给出反射侧可判定的部分,
/// 因此 Texture 与 TexelBuffer、Buffer 与 RWBuffer 的细分交给 IsBufferDimension。
std::optional<render::ShaderParameterBindingType> MapHlslBindingType(
    const render::HlslInputBindDesc& bind) noexcept {
    switch (bind.Type) {
        case render::HlslShaderInputType::CBUFFER:
            return render::ShaderParameterBindingType::CBuffer;
        case render::HlslShaderInputType::TBUFFER:
            return render::ShaderParameterBindingType::TexelBuffer;
        case render::HlslShaderInputType::SAMPLER:
            return render::ShaderParameterBindingType::Sampler;
        case render::HlslShaderInputType::TEXTURE:
            return render::IsBufferDimension(bind.Dimension)
                       ? render::ShaderParameterBindingType::TexelBuffer
                       : render::ShaderParameterBindingType::Texture;
        case render::HlslShaderInputType::STRUCTURED:
        case render::HlslShaderInputType::BYTEADDRESS:
            return render::ShaderParameterBindingType::Buffer;
        case render::HlslShaderInputType::UAV_RWSTRUCTURED:
        case render::HlslShaderInputType::UAV_RWBYTEADDRESS:
        case render::HlslShaderInputType::UAV_APPEND_STRUCTURED:
        case render::HlslShaderInputType::UAV_CONSUME_STRUCTURED:
        case render::HlslShaderInputType::UAV_RWSTRUCTURED_WITH_COUNTER:
            return render::ShaderParameterBindingType::RWBuffer;
        case render::HlslShaderInputType::UAV_RWTYPED:
            return render::IsBufferDimension(bind.Dimension)
                       ? render::ShaderParameterBindingType::RWTexelBuffer
                       : render::ShaderParameterBindingType::RWTexture;
        default:
            return std::nullopt;
    }
}

std::optional<render::ShaderParameterBindingType> MapSpirvBindingType(
    const render::SpirvResourceBinding& bind) noexcept {
    switch (bind.Kind) {
        case render::SpirvResourceKind::UniformBuffer:
            return render::ShaderParameterBindingType::CBuffer;
        case render::SpirvResourceKind::StorageBuffer:
            return bind.WriteOnly || !bind.ReadOnly
                       ? render::ShaderParameterBindingType::RWBuffer
                       : render::ShaderParameterBindingType::Buffer;
        case render::SpirvResourceKind::SeparateSampler:
            return render::ShaderParameterBindingType::Sampler;
        case render::SpirvResourceKind::SeparateImage:
        case render::SpirvResourceKind::SampledImage:
            return bind.ImageInfo.has_value() && bind.ImageInfo->Dim == render::SpirvImageDim::Buffer
                       ? render::ShaderParameterBindingType::TexelBuffer
                       : render::ShaderParameterBindingType::Texture;
        case render::SpirvResourceKind::StorageImage:
            return bind.ImageInfo.has_value() && bind.ImageInfo->Dim == render::SpirvImageDim::Buffer
                       ? render::ShaderParameterBindingType::RWTexelBuffer
                       : render::ShaderParameterBindingType::RWTexture;
        default:
            return std::nullopt;
    }
}

/// 反射侧的一条绑定, 归一化后与 manifest 比对。
struct ReflectedBinding {
    std::string_view Name;
    uint32_t Group{0};
    uint32_t Binding{0};
    render::ShaderParameterBindingType Type{render::ShaderParameterBindingType::UNKNOWN};
    /// 0 表示 unbounded。
    uint32_t Count{1};
};

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
                NameOfEnum(kBindingTypeTable, declared.Type),
                NameOfEnum(kBindingTypeTable, item.Type)));
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

/// 去掉 HLSL semantic 名尾部的数字, 得到基名与索引 (DXC 会把 POSITION0 拆成
/// SemanticName="POSITION" + SemanticIndex=0, 但不同来源不一致, 统一归一化)。
void SplitSemantic(std::string_view raw, std::string_view& baseName, uint32_t& index) noexcept {
    size_t end = raw.size();
    while (end > 0 && raw[end - 1] >= '0' && raw[end - 1] <= '9') {
        --end;
    }
    baseName = raw.substr(0, end);
    index = 0;
    if (end < raw.size()) {
        uint64_t parsed = 0;
        for (size_t i = end; i < raw.size(); ++i) {
            parsed = parsed * 10 + static_cast<uint64_t>(raw[i] - '0');
            if (parsed > std::numeric_limits<uint32_t>::max()) {
                parsed = std::numeric_limits<uint32_t>::max();
                break;
            }
        }
        index = static_cast<uint32_t>(parsed);
    }
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        const char ca = (a[i] >= 'a' && a[i] <= 'z') ? static_cast<char>(a[i] - 32) : a[i];
        const char cb = (b[i] >= 'a' && b[i] <= 'z') ? static_cast<char>(b[i] - 32) : b[i];
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

/// 系统值语义不由 vertex buffer 提供, 不参与 VertexInput 比对。
bool IsSystemSemantic(std::string_view baseName) noexcept {
    return baseName.size() >= 3 &&
           (baseName[0] == 'S' || baseName[0] == 's') &&
           (baseName[1] == 'V' || baseName[1] == 'v') &&
           baseName[2] == '_';
}

// ============================ 结构 -> JSON ============================

/// 把 ShaderStages 写成字符串数组。
bool WriteStages(JsonRef parent, std::string_view key, render::ShaderStages stages) noexcept {
    JsonRef array = parent.AddArray(key);
    if (!array.IsValid()) {
        return false;
    }
    for (const EnumEntry<render::ShaderStage>& entry : kShaderStageTable) {
        if (stages.HasFlag(entry.Value)) {
            array.AppendString(entry.Name);
        }
    }
    return true;
}

void WriteSampler(JsonRef parent, const render::SamplerDescriptor& sampler) noexcept {
    JsonRef obj = parent.AddObject("ImmutableSampler");
    if (!obj.IsValid()) {
        return;
    }
    obj.AddString("AddressS", NameOfEnum(kAddressModeTable, sampler.AddressS));
    obj.AddString("AddressT", NameOfEnum(kAddressModeTable, sampler.AddressT));
    obj.AddString("AddressR", NameOfEnum(kAddressModeTable, sampler.AddressR));
    obj.AddString("MinFilter", NameOfEnum(kFilterModeTable, sampler.MinFilter));
    obj.AddString("MagFilter", NameOfEnum(kFilterModeTable, sampler.MagFilter));
    obj.AddString("MipmapFilter", NameOfEnum(kFilterModeTable, sampler.MipmapFilter));
    obj.AddDouble("LodMin", sampler.LodMin);
    obj.AddDouble("LodMax", sampler.LodMax);
    if (sampler.Compare.has_value()) {
        obj.AddString("Compare", NameOfEnum(kCompareFunctionTable, sampler.Compare.value()));
    }
    obj.AddUint("AnisotropyClamp", sampler.AnisotropyClamp);
}

void WriteBakeSet(JsonRef parent, const ShaderBakeSetDesc& bake) noexcept {
    JsonRef obj = parent.AddObject("BakeVariants");
    if (!obj.IsValid()) {
        return;
    }
    if (!bake.Rules.empty()) {
        JsonRef rules = obj.AddArray("Rules");
        for (const ShaderBakeRuleDesc& rule : bake.Rules) {
            JsonRef item = rules.AppendObject();
            if (!rule.Expand.empty()) {
                JsonRef expand = item.AddArray("Expand");
                for (const string& name : rule.Expand) {
                    expand.AppendString(name);
                }
            }
            if (!rule.Combination.empty()) {
                JsonRef combination = item.AddArray("Combination");
                for (const string& keyword : rule.Combination) {
                    combination.AppendString(keyword);
                }
            }
        }
    }
    if (!bake.Skip.empty()) {
        JsonRef skip = obj.AddArray("Skip");
        for (const vector<string>& entry : bake.Skip) {
            JsonRef item = skip.AppendArray();
            for (const string& keyword : entry) {
                item.AppendString(keyword);
            }
        }
    }
}

void WritePass(JsonRef array, const ShaderPassDesc& pass) noexcept {
    JsonRef obj = array.AppendObject();
    if (!obj.IsValid()) {
        return;
    }
    obj.AddString("Name", pass.Name);
    if (!pass.Source.empty()) {
        obj.AddString("Source", pass.Source);
    }
    JsonRef stages = obj.AddArray("Stages");
    for (const ShaderStageDesc& stage : pass.Stages) {
        JsonRef item = stages.AppendObject();
        item.AddString("Stage", NameOfEnum(kShaderStageTable, stage.Stage));
        item.AddString("EntryPoint", stage.EntryPoint);
    }
    obj.AddString("ShaderModel", NameOfEnum(kShaderModelTable, pass.ShaderModel));
    obj.AddBool("IsOptimize", pass.IsOptimize);
    obj.AddBool("EnableUnbounded", pass.EnableUnbounded);
    if (!pass.Defines.empty()) {
        JsonRef defines = obj.AddArray("Defines");
        for (const string& define : pass.Defines) {
            defines.AppendString(define);
        }
    }
    if (!pass.KeywordGroups.empty()) {
        JsonRef groups = obj.AddArray("KeywordGroups");
        for (const string& name : pass.KeywordGroups) {
            groups.AppendString(name);
        }
    }
    if (!pass.BakeVariants.IsEmpty() || !pass.BakeVariants.Skip.empty()) {
        WriteBakeSet(obj, pass.BakeVariants);
    }
    if (pass.PushConstant.has_value()) {
        const ShaderPushConstantDesc& pc = pass.PushConstant.value();
        JsonRef pcObj = obj.AddObject("PushConstant");
        pcObj.AddString("Name", pc.Name);
        JsonRef location = pcObj.AddObject("Location");
        location.AddUint("Group", pc.Location.Group);
        location.AddUint("Binding", pc.Location.Binding);
        pcObj.AddUint("Size", pc.Size);
        WriteStages(pcObj, "Stages", pc.Stages);
    }
    if (!pass.BindingGroups.empty()) {
        JsonRef groups = obj.AddArray("BindingGroups");
        for (const ShaderBindingGroupDesc& group : pass.BindingGroups) {
            JsonRef groupObj = groups.AppendObject();
            groupObj.AddUint("Group", group.Group);
            JsonRef bindings = groupObj.AddArray("Bindings");
            for (const ShaderBindingDesc& binding : group.Bindings) {
                JsonRef bindingObj = bindings.AppendObject();
                bindingObj.AddString("Name", binding.Name);
                bindingObj.AddUint("Binding", binding.Binding);
                bindingObj.AddString("Type", NameOfEnum(kBindingTypeTable, binding.Type));
                bindingObj.AddUint("Count", binding.Count);
                WriteStages(bindingObj, "Stages", binding.Stages);
                bindingObj.AddString("Residency", NameOfEnum(kResidencyTable, binding.Residency));
                if (binding.ImmutableSampler.has_value()) {
                    WriteSampler(bindingObj, binding.ImmutableSampler.value());
                }
            }
        }
    }
    if (pass.VertexInput.has_value()) {
        const ShaderVertexInputDesc& vi = pass.VertexInput.value();
        JsonRef viObj = obj.AddObject("VertexInput");
        JsonRef buffers = viObj.AddArray("Buffers");
        for (const ShaderVertexBufferDesc& buffer : vi.Buffers) {
            JsonRef item = buffers.AppendObject();
            item.AddUint("Binding", buffer.Binding);
            item.AddUint("ArrayStride", buffer.ArrayStride);
            item.AddString("StepMode", NameOfEnum(kVertexStepModeTable, buffer.StepMode));
        }
        JsonRef attributes = viObj.AddArray("Attributes");
        for (const ShaderVertexAttributeDesc& attribute : vi.Attributes) {
            JsonRef item = attributes.AppendObject();
            item.AddString("Semantic", attribute.Semantic);
            item.AddUint("SemanticIndex", attribute.SemanticIndex);
            item.AddString("Format", NameOfEnum(kVertexFormatTable, attribute.Format));
            item.AddUint("BufferBinding", attribute.BufferBinding);
            item.AddUint("Offset", attribute.Offset);
            if (attribute.Location.has_value()) {
                item.AddUint("Location", attribute.Location.value());
            }
        }
    }
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

std::optional<ShaderHash> ReadHash(const JsonValue& obj, std::string_view key) noexcept {
    JsonValue value = obj[key];
    if (!value.IsString()) {
        return std::nullopt;
    }
    return ShaderHash::FromHex(value.AsString());
}

/// 本次构建钉住的 DXC 版本。由 CMake 无条件注入 (与是否编入 JIT 无关) ——
/// 关 JIT 的发布包必须与 cook 机算出同一个 toolchain hash, 否则产物全部判为过期。
#if defined(RADRAY_DXC_VERSION)
constexpr std::string_view kDxcVersion = RADRAY_DXC_VERSION;
#else
#error "RADRAY_DXC_VERSION must be defined; the shader toolchain hash depends on it"
#endif

}  // namespace

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

render::ShaderDescriptor ShaderBytecode::MakeDescriptor() const noexcept {
    return render::ShaderDescriptor{
        .Source = Data,
        .Category = Category,
        .Stages = Stage};
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

render::PipelineLayoutDescriptor ShaderPipelineLayoutStorage::Get() const noexcept {
    render::PipelineLayoutDescriptor desc{};
    desc.ParameterSets = _sets;
    desc.PushConstant = _pushConstant;
    return desc;
}

render::VertexInputState ShaderVertexInputStorage::Get() const noexcept {
    render::VertexInputState state{};
    state.Buffers = _buffers;
    state.Attributes = _attributes;
    return state;
}

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
    JsonValue version = root["FormatVersion"];
    if (!version.IsNumber()) {
        scope.Fail("shader asset is missing 'FormatVersion'");
        return std::nullopt;
    }
    if (version.AsUint() != kShaderAssetFormatVersion) {
        scope.Fail(fmt::format(
            "shader asset FormatVersion {} != expected {}",
            version.AsUint(),
            kShaderAssetFormatVersion));
        return std::nullopt;
    }

    ShaderAssetDesc desc{};
    if (!ReadRequiredString(root, "Name", scope, desc.Name) ||
        !ReadOptionalString(root, "Source", scope, desc.Source)) {
        return std::nullopt;
    }

    if (root.Has("KeywordGroups")) {
        JsonValue groups = root["KeywordGroups"];
        if (!groups.IsArray()) {
            scope.Fail("'KeywordGroups' must be an array");
            return std::nullopt;
        }
        desc.KeywordGroups.reserve(groups.Size());
        for (size_t i = 0; i < groups.Size(); ++i) {
            ShaderKeywordGroupDesc group{};
            if (!ReadKeywordGroup(groups.At(i), scope, group)) {
                return std::nullopt;
            }
            desc.KeywordGroups.push_back(std::move(group));
        }
    }

    if (root.Has("BakeVariants")) {
        if (!ReadBakeSet(root["BakeVariants"], scope, desc.BakeVariants)) {
            return std::nullopt;
        }
    }

    JsonValue passes = root["Passes"];
    if (!passes.IsArray()) {
        scope.Fail("shader asset must have a 'Passes' array");
        return std::nullopt;
    }
    desc.Passes.reserve(passes.Size());
    for (size_t i = 0; i < passes.Size(); ++i) {
        ShaderPassDesc pass{};
        if (!ReadPass(passes.At(i), scope, pass)) {
            return std::nullopt;
        }
        desc.Passes.push_back(std::move(pass));
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
    JsonWriter writer;
    if (!writer.IsValid()) {
        return std::nullopt;
    }
    JsonRef root = writer.RootObject();
    if (!root.IsValid()) {
        return std::nullopt;
    }
    root.AddUint("FormatVersion", kShaderAssetFormatVersion);
    root.AddString("Name", desc.Name);
    if (!desc.Source.empty()) {
        root.AddString("Source", desc.Source);
    }
    if (!desc.KeywordGroups.empty()) {
        JsonRef groups = root.AddArray("KeywordGroups");
        for (const ShaderKeywordGroupDesc& group : desc.KeywordGroups) {
            JsonRef obj = groups.AppendObject();
            obj.AddString("Name", group.Name);
            JsonRef keywords = obj.AddArray("Keywords");
            for (const string& keyword : group.Keywords) {
                keywords.AppendString(keyword);
            }
            obj.AddBool("IsOptional", group.IsOptional);
            WriteStages(obj, "Stages", group.Stages);
        }
    }
    if (!desc.BakeVariants.IsEmpty() || !desc.BakeVariants.Skip.empty()) {
        WriteBakeSet(root, desc.BakeVariants);
    }
    JsonRef passes = root.AddArray("Passes");
    if (!passes.IsValid()) {
        return std::nullopt;
    }
    for (const ShaderPassDesc& pass : desc.Passes) {
        WritePass(passes, pass);
    }
    return writer.Write(pretty);
}

// ============================ layout 构建 ============================

ShaderPipelineLayoutStorage BuildPipelineLayoutStorage(const ShaderPassDesc& pass) {
    ShaderPipelineLayoutStorage storage;
    storage._entries.reserve(pass.BindingGroups.size());
    storage._sets.reserve(pass.BindingGroups.size());

    for (const ShaderBindingGroupDesc& group : pass.BindingGroups) {
        auto entries = make_unique<vector<render::ShaderParameterSetLayoutEntryDescriptor>>();
        entries->reserve(group.Bindings.size());
        for (const ShaderBindingDesc& binding : group.Bindings) {
            render::ShaderParameterSetLayoutEntryDescriptor entry{};
            entry.Binding = binding.Binding;
            entry.Type = ResolveBindingType(binding);
            entry.Count = binding.Count;
            entry.Stages = binding.Stages;
            entry.ImmutableSampler = binding.ImmutableSampler;
            entries->push_back(entry);
        }
        render::ShaderParameterSetLayoutDescriptor set{};
        set.GroupIndex = group.Group;
        set.Entries = *entries;
        storage._entries.push_back(std::move(entries));
        storage._sets.push_back(set);
    }

    if (pass.PushConstant.has_value()) {
        const ShaderPushConstantDesc& pc = pass.PushConstant.value();
        render::PushConstantDescriptor descriptor{};
        descriptor.Location = pc.Location;
        descriptor.Size = pc.Size;
        descriptor.Stages = pc.Stages;
        storage._pushConstant = descriptor;
    }
    return storage;
}

ShaderVertexInputStorage BuildVertexInputStorage(const ShaderVertexInputDesc& desc) {
    ShaderVertexInputStorage storage;
    storage._buffers.reserve(desc.Buffers.size());
    for (const ShaderVertexBufferDesc& buffer : desc.Buffers) {
        render::VertexBufferLayout layout{};
        layout.Binding = buffer.Binding;
        layout.ArrayStride = buffer.ArrayStride;
        layout.StepMode = buffer.StepMode;
        storage._buffers.push_back(layout);
    }
    storage._semantics.reserve(desc.Attributes.size());
    storage._attributes.reserve(desc.Attributes.size());
    for (size_t i = 0; i < desc.Attributes.size(); ++i) {
        const ShaderVertexAttributeDesc& source = desc.Attributes[i];
        auto semantic = make_unique<string>(source.Semantic);
        render::VertexAttribute attribute{};
        attribute.BufferBinding = source.BufferBinding;
        attribute.Offset = source.Offset;
        attribute.Semantic = *semantic;
        attribute.SemanticIndex = source.SemanticIndex;
        attribute.Format = source.Format;
        attribute.Location = source.Location.value_or(static_cast<uint32_t>(i));
        storage._semantics.push_back(std::move(semantic));
        storage._attributes.push_back(attribute);
    }
    return storage;
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
        std::optional<render::ShaderParameterBindingType> type = MapHlslBindingType(bind);
        if (!type.has_value()) {
            scope.SetBinding(bind.Name);
            scope.SetGroup(bind.Space);
            scope.SetBindingIndex(bind.BindPoint);
            return scope.Fail(fmt::format(
                "reflection reports resource '{}' with a type that has no RHI binding equivalent",
                bind.Name));
        }
        reflected.push_back(ReflectedBinding{
            bind.Name,
            bind.Space,
            bind.BindPoint,
            type.value(),
            bind.IsUnboundArray() ? 0u : bind.BindCount});
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
            // DXC 通常已把索引拆到 SemanticIndex; 名字尾部还带数字时取名字里的。
            const uint32_t index = nameIndex != 0 ? nameIndex : input.SemanticIndex;
            const bool found = std::any_of(
                pass.VertexInput->Attributes.begin(),
                pass.VertexInput->Attributes.end(),
                [&](const ShaderVertexAttributeDesc& attribute) noexcept {
                    std::string_view declaredBase{};
                    uint32_t declaredIndex = 0;
                    SplitSemantic(attribute.Semantic, declaredBase, declaredIndex);
                    const uint32_t effective =
                        declaredIndex != 0 ? declaredIndex : attribute.SemanticIndex;
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
        std::optional<render::ShaderParameterBindingType> type = MapSpirvBindingType(bind);
        if (!type.has_value()) {
            scope.SetBinding(bind.Name);
            scope.SetGroup(bind.Set);
            scope.SetBindingIndex(bind.Binding);
            return scope.Fail(fmt::format(
                "reflection reports resource '{}' with a type that has no RHI binding equivalent",
                bind.Name));
        }
        reflected.push_back(ReflectedBinding{
            bind.Name,
            bind.Set,
            bind.Binding,
            type.value(),
            bind.IsUnboundedArray ? 0u : (bind.ArraySize == 0 ? 1u : bind.ArraySize)});
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
    std::string_view dir = NameOfEnum(kBlobCategoryTable, category);
    if (dir.empty()) {
        dir = "unknown";
    }
    string lower{dir};
    std::ranges::transform(lower, lower.begin(), [](char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    });
    return fmt::format("{}/{}.bin", lower, key.ToHex());
}

render::ShaderBlobCategory GetShaderBlobCategoryForBackend(render::RenderBackend backend) noexcept {
    switch (backend) {
        case render::RenderBackend::D3D12: return render::ShaderBlobCategory::DXIL;
        case render::RenderBackend::Vulkan: return render::ShaderBlobCategory::SPIRV;
        case render::RenderBackend::Metal: return render::ShaderBlobCategory::MSL;
        default: return render::ShaderBlobCategory::DXIL;
    }
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

    ShaderArtifactIndex index;
    JsonValue version = root["FormatVersion"];
    if (!version.IsNumber()) {
        outDiag.Message = "shader artifact index is missing 'FormatVersion'";
        return std::nullopt;
    }
    index.FormatVersion = static_cast<uint32_t>(version.AsUint());
    if (index.FormatVersion != kShaderArtifactFormatVersion) {
        outDiag.Message = fmt::format(
            "shader artifact index FormatVersion {} != expected {}",
            index.FormatVersion,
            kShaderArtifactFormatVersion);
        return std::nullopt;
    }
    index.AssetName = string{root["AssetName"].AsString()};

    JsonValue sources = root["Sources"];
    if (!sources.IsArray() || sources.Size() == 0) {
        outDiag.Message = "shader artifact index 'Sources' must be a non-empty array";
        return std::nullopt;
    }
    index.Sources.reserve(sources.Size());
    for (size_t i = 0; i < sources.Size(); ++i) {
        JsonValue element = sources.At(i);
        if (!element.IsObject()) {
            outDiag.Message = fmt::format("shader artifact Sources[{}] must be an object", i);
            return std::nullopt;
        }
        ShaderArtifactSource source;
        source.Path = string{element["Path"].AsString()};
        auto identity = ReadHash(element, "Identity");
        if (source.Path.empty() || !identity.has_value()) {
            outDiag.Message = fmt::format(
                "shader artifact Sources[{}] has a missing Path or malformed Identity",
                i);
            return std::nullopt;
        }
        source.Identity = identity.value();
        if (index.FindSourceIdentity(source.Path).has_value()) {
            outDiag.Message = fmt::format(
                "shader artifact index has a duplicate source '{}'",
                source.Path);
            return std::nullopt;
        }
        index.Sources.push_back(std::move(source));
    }

    auto toolchain = ReadHash(root, "ToolchainHash");
    if (!toolchain.has_value()) {
        outDiag.Message = "shader artifact index has a missing or malformed 'ToolchainHash'";
        return std::nullopt;
    }
    index.ToolchainHash = toolchain.value();

    JsonValue entries = root["Entries"];
    if (!entries.IsArray()) {
        outDiag.Message = "shader artifact index 'Entries' must be an array";
        return std::nullopt;
    }
    index.Entries.reserve(entries.Size());
    for (size_t i = 0; i < entries.Size(); ++i) {
        JsonValue element = entries.At(i);
        if (!element.IsObject()) {
            outDiag.Message = fmt::format("shader artifact Entries[{}] must be an object", i);
            return std::nullopt;
        }
        ShaderArtifactEntry entry;
        auto key = ReadHash(element, "Key");
        auto bytecodeHash = ReadHash(element, "BytecodeHash");
        if (!key.has_value() || !bytecodeHash.has_value()) {
            outDiag.Message = fmt::format("shader artifact Entries[{}] has a malformed hash", i);
            return std::nullopt;
        }
        entry.Key = key.value();
        entry.BytecodeHash = bytecodeHash.value();
        entry.PassName = string{element["PassName"].AsString()};
        entry.Source = string{element["Source"].AsString()};
        entry.EntryPoint = string{element["EntryPoint"].AsString()};
        entry.BlobPath = string{element["BlobPath"].AsString()};
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
        auto stage = LookupEnum(kShaderStageTable, element["Stage"].AsString());
        auto category = LookupEnum(kBlobCategoryTable, element["Category"].AsString());
        if (!stage.has_value() || !category.has_value()) {
            outDiag.Message = fmt::format(
                "shader artifact Entries[{}] has an unknown Stage or Category",
                i);
            return std::nullopt;
        }
        entry.Stage = stage.value();
        entry.Category = category.value();
        entry.BytecodeSize = static_cast<uint32_t>(element["BytecodeSize"].AsUint());
        if (entry.BytecodeSize == 0) {
            outDiag.Message = fmt::format("shader artifact Entries[{}] has zero BytecodeSize", i);
            return std::nullopt;
        }
        // Keywords 是可选的可读身份, 不参与查找。缺失 (旧产物) 与空数组等价。
        if (element.Has("Keywords")) {
            JsonValue keywords = element["Keywords"];
            if (!keywords.IsArray()) {
                outDiag.Message = fmt::format(
                    "shader artifact Entries[{}] 'Keywords' must be an array",
                    i);
                return std::nullopt;
            }
            entry.Keywords.reserve(keywords.Size());
            for (size_t j = 0; j < keywords.Size(); ++j) {
                JsonValue keyword = keywords.At(j);
                if (!keyword.IsString()) {
                    outDiag.Message = fmt::format(
                        "shader artifact Entries[{}] Keywords[{}] must be a string",
                        i,
                        j);
                    return std::nullopt;
                }
                entry.Keywords.push_back(string{keyword.AsString()});
            }
        }
        for (const ShaderArtifactEntry& existing : index.Entries) {
            if (existing.Key == entry.Key) {
                outDiag.Message = fmt::format(
                    "shader artifact index has a duplicate key {}",
                    entry.Key.ToHex());
                return std::nullopt;
            }
        }
        index.Entries.push_back(std::move(entry));
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
    JsonWriter writer;
    if (!writer.IsValid()) {
        return std::nullopt;
    }
    JsonRef root = writer.RootObject();
    if (!root.IsValid()) {
        return std::nullopt;
    }
    root.AddUint("FormatVersion", kShaderArtifactFormatVersion);
    root.AddString("AssetName", index.AssetName);
    const string toolchain = index.ToolchainHash.ToHex();
    root.AddString("ToolchainHash", toolchain);

    JsonRef sources = root.AddArray("Sources");
    for (const ShaderArtifactSource& source : index.Sources) {
        JsonRef obj = sources.AppendObject();
        if (!obj.IsValid()) {
            return std::nullopt;
        }
        obj.AddString("Path", source.Path);
        obj.AddString("Identity", source.Identity.ToHex());
    }

    JsonRef entries = root.AddArray("Entries");
    // ToHex 返回临时 string, 而 AddString 会复制 value, 故无需延长生命周期。
    for (const ShaderArtifactEntry& entry : index.Entries) {
        JsonRef obj = entries.AppendObject();
        if (!obj.IsValid()) {
            return std::nullopt;
        }
        obj.AddString("Key", entry.Key.ToHex());
        obj.AddString("PassName", entry.PassName);
        obj.AddString("Source", entry.Source);
        obj.AddString("Stage", NameOfEnum(kShaderStageTable, entry.Stage));
        obj.AddString("EntryPoint", entry.EntryPoint);
        obj.AddString("Category", NameOfEnum(kBlobCategoryTable, entry.Category));
        obj.AddString("BlobPath", entry.BlobPath);
        obj.AddString("BytecodeHash", entry.BytecodeHash.ToHex());
        obj.AddUint("BytecodeSize", entry.BytecodeSize);
        if (!entry.Keywords.empty()) {
            JsonRef keywords = obj.AddArray("Keywords");
            for (const string& keyword : entry.Keywords) {
                keywords.AppendString(keyword);
            }
        }
    }
    return writer.Write(pretty);
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
        const string source = pass.Source.empty() ? asset.Source : pass.Source;
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
        const string source = pass.Source.empty() ? asset.Source : pass.Source;
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
    const std::string_view name = NameOfEnum(kResidencyTable, v);
    return name.empty() ? std::string_view{"UNKNOWN"} : name;
}

std::string_view format_as(ShaderBytecodeSource v) noexcept {
    switch (v) {
        case ShaderBytecodeSource::Artifact: return "Artifact";
        case ShaderBytecodeSource::Jit: return "Jit";
    }
    return "UNKNOWN";
}

std::string_view format_as(ShaderArtifactStaleness v) noexcept {
    switch (v) {
        case ShaderArtifactStaleness::Strict: return "Strict";
        case ShaderArtifactStaleness::Lenient: return "Lenient";
    }
    return "UNKNOWN";
}

}  // namespace radray
