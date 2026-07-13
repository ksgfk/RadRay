#pragma once

#include <optional>

#include <radray/runtime/shader_asset.h>
#include <radray/runtime/shader_variant_library.h>
#include <radray/types.h>

namespace radray {

enum class ShaderBindingFrequency : uint8_t {
    Material,
    Pass,
    View,
    Object,
};

enum class ShaderBindingEntryKind : uint8_t {
    ConstantBuffer,
    Texture,
    Sampler,
    StaticSampler,
};

struct ShaderBindingSource {
    ShaderParameterScope Scope{ShaderParameterScope::Material};
    string ProviderName;
    bool Explicit{false};
};

struct ShaderConstantFieldPlan {
    string Name;
    uint32_t Offset{0};
    uint32_t Size{0};
    ShaderBindingSource Source{};
};

struct ShaderBindingEntryPlan {
    string Name;
    uint32_t Group{0};
    uint32_t Binding{0};
    uint32_t Count{1};
    uint32_t ByteSize{0};
    render::ShaderParameterKind ParameterKind{render::ShaderParameterKind::UNKNOWN};
    render::ResourceBindType ResourceType{render::ResourceBindType::UNKNOWN};
    ShaderBindingEntryKind Kind{ShaderBindingEntryKind::Texture};
    ShaderBindingFrequency Frequency{ShaderBindingFrequency::Material};
    bool HasDynamicOffset{false};
    ShaderBindingSource Source{};
    vector<ShaderConstantFieldPlan> Fields;
};

struct ShaderBindingGroupPlan {
    uint32_t Group{0};
    ShaderBindingFrequency Frequency{ShaderBindingFrequency::Material};
    vector<uint32_t> EntryIndices;
    vector<uint32_t> DynamicEntryIndices;
};

struct ShaderBindingPlan {
    Guid ProgramId{};
    uint32_t PassIndex{0};
    vector<ShaderBindingEntryPlan> Entries;
    vector<ShaderBindingGroupPlan> Groups;
    bool Valid{false};
    string Error;
    std::optional<uint32_t> ErrorGroup{};
    std::optional<uint32_t> ErrorBinding{};
};

struct ShaderBindingDiagnostic {
    Guid ProgramId{};
    uint32_t PassIndex{0};
    ShaderVariantKey VariantKey{};
    std::optional<uint32_t> Group{};
    std::optional<uint32_t> Binding{};
    string Reason;

    friend bool operator==(const ShaderBindingDiagnostic&, const ShaderBindingDiagnostic&) noexcept = default;
};

class ShaderBindingPlanLibrary {
public:
    Nullable<const ShaderBindingPlan*> GetOrCreate(
        const ShaderAsset& shader,
        uint32_t passIndex,
        const CompiledShaderVariant& variant) noexcept;

    void Clear() noexcept;
    uint64_t GetHitCount() const noexcept { return _hits; }
    uint64_t GetMissCount() const noexcept { return _misses; }

private:
    struct Entry {
        Guid ProgramId{};
        uint32_t PassIndex{0};
        ShaderVariantKey VariantKey{};
        unique_ptr<ShaderBindingPlan> Plan;
    };

    vector<Entry> _entries;
    uint64_t _hits{0};
    uint64_t _misses{0};
};

}  // namespace radray
