#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

#include <radray/basic_math.h>
#include <radray/render/common.h>
#include <radray/runtime/texture_asset.h>
#include <radray/shader/shader_binary.h>
#include <radray/types.h>

namespace radray {

enum class ShaderBindingDiagnosticCode : uint8_t {
    InvalidInterface,
    InvalidPolicy,
    ProviderRejectedGroup,
    InterfaceMismatch,
};

struct ShaderBindingDiagnostic {
    ShaderBindingDiagnosticCode Code{ShaderBindingDiagnosticCode::InvalidInterface};
    string Message;
    shader::ShaderDiagnosticContext Context{};
    std::optional<shader::ShaderDiagnosticContext> RelatedContext{};
    string ProviderName;

    friend bool operator==(const ShaderBindingDiagnostic&, const ShaderBindingDiagnostic&) = default;
};

struct ShaderBindingProviderMatchResult {
    bool Compatible{false};
    string Message;
    std::optional<uint32_t> Binding{};

    static ShaderBindingProviderMatchResult Success() { return {.Compatible = true}; }
    static ShaderBindingProviderMatchResult Failure(
        string message,
        std::optional<uint32_t> binding = {}) {
        return {
            .Compatible = false,
            .Message = std::move(message),
            .Binding = binding};
    }
};

class IShaderBindingProvider {
public:
    virtual ~IShaderBindingProvider() noexcept = default;

    virtual std::string_view GetName() const noexcept = 0;
    virtual ShaderBindingProviderMatchResult Match(
        const shader::ShaderBindingGroupInterfaceDesc& group) const = 0;
};

struct PipelineBindingReservation {
    uint32_t GroupIndex{0};
    shared_ptr<const IShaderBindingProvider> Provider;

    friend bool operator==(const PipelineBindingReservation&, const PipelineBindingReservation&) = default;
};

class PipelineBindingPolicy {
public:
    PipelineBindingPolicy() noexcept = default;
    explicit PipelineBindingPolicy(vector<PipelineBindingReservation> reservations) noexcept;

    bool IsValid() const noexcept;
    std::span<const PipelineBindingReservation> Reservations() const noexcept { return _reservations; }
    const PipelineBindingReservation* Find(uint32_t groupIndex) const noexcept;

    friend bool operator==(const PipelineBindingPolicy&, const PipelineBindingPolicy&) = default;

private:
    vector<PipelineBindingReservation> _reservations;
};

struct ResolvedProviderBindingGroup {
    uint32_t GroupIndex{0};
    shared_ptr<const IShaderBindingProvider> Provider;
    shader::ShaderBindingGroupInterfaceDesc Interface;
};

struct ResolvedShaderBindingPlan {
    shader::ShaderHash InterfaceHash{};
    vector<ResolvedProviderBindingGroup> ProviderGroups;
    vector<shader::ShaderBindingGroupInterfaceDesc> UserGroups;
};

struct ShaderBindingResolutionResult {
    std::optional<ResolvedShaderBindingPlan> Plan;
    vector<ShaderBindingDiagnostic> Diagnostics;

    bool Succeeded() const noexcept { return Plan.has_value() && Diagnostics.empty(); }
};

ShaderBindingResolutionResult ResolveShaderBindings(
    const shader::ShaderInterfaceDesc& interface,
    const PipelineBindingPolicy& policy,
    const shader::ShaderDiagnosticContext& context = {});

struct ShaderBindingProviderSchemaEntry {
    // Alternatives share one binding index. This permits a provider to support
    // several physical projections at the same reserved location.
    vector<shader::ShaderBindingDesc> AcceptedBindings;
    bool Required{false};
};

// A reusable whole-group provider matcher. Every shader binding must be known;
// optional schema entries may be absent from a shader variant.
class ShaderBindingSchemaProvider final : public IShaderBindingProvider {
public:
    ShaderBindingSchemaProvider(
        string name,
        vector<ShaderBindingProviderSchemaEntry> entries) noexcept;

    std::string_view GetName() const noexcept override { return _name; }
    ShaderBindingProviderMatchResult Match(
        const shader::ShaderBindingGroupInterfaceDesc& group) const override;

private:
    string _name;
    vector<ShaderBindingProviderSchemaEntry> _entries;
    bool _valid{false};
};

struct ShaderParameterLocation {
    uint32_t Group{0};
    uint32_t Binding{0};

    friend bool operator==(const ShaderParameterLocation&, const ShaderParameterLocation&) = default;
};

struct ShaderParameterFieldDesc {
    string Name;
    string QualifiedName;
    uint32_t Offset{0};
    uint32_t Size{0};
    shader::ShaderValueTypeDesc Type{};

    friend bool operator==(const ShaderParameterFieldDesc&, const ShaderParameterFieldDesc&) = default;
};

struct ShaderParameterBindingDesc {
    ShaderParameterLocation Location{};
    shader::ShaderBindingDesc Interface;
    vector<ShaderParameterFieldDesc> Fields;

    friend bool operator==(const ShaderParameterBindingDesc&, const ShaderParameterBindingDesc&) = default;
};

struct ShaderProgramInterfaceRecord {
    shader::ShaderInterfaceDesc Interface;
    shader::ShaderDiagnosticContext Context{};

    friend bool operator==(const ShaderProgramInterfaceRecord&, const ShaderProgramInterfaceRecord&) = default;
};

class ShaderParameterLayout {
public:
    bool IsValid() const noexcept;
    bool Empty() const noexcept { return _bindings.empty(); }
    shader::ShaderHash GetHash() const noexcept { return _hash; }

    std::span<const shader::ShaderBindingGroupInterfaceDesc> Groups() const noexcept { return _groups; }
    std::span<const ShaderParameterBindingDesc> Bindings() const noexcept { return _bindings; }
    Nullable<const ShaderParameterBindingDesc*> FindBinding(ShaderParameterLocation location) const noexcept;
    Nullable<const ShaderParameterBindingDesc*> FindBinding(std::string_view name) const noexcept;
    Nullable<const ShaderParameterFieldDesc*> FindField(
        ShaderParameterLocation location,
        std::string_view name) const noexcept;
    Nullable<const ShaderParameterFieldDesc*> FindField(std::string_view name) const noexcept;

    friend bool operator==(const ShaderParameterLayout&, const ShaderParameterLayout&) = default;

private:
    vector<shader::ShaderBindingGroupInterfaceDesc> _groups;
    vector<ShaderParameterBindingDesc> _bindings;
    shader::ShaderHash _hash{};

    friend struct ShaderParameterLayoutBuildResult;
    friend ShaderParameterLayoutBuildResult BuildShaderParameterLayout(
        const shader::ShaderBinary&,
        const PipelineBindingPolicy&,
        shader::ShaderProgramKind);
    friend ShaderParameterLayoutBuildResult BuildShaderParameterLayout(
        std::span<const shader::ShaderInterfaceDesc>,
        const PipelineBindingPolicy&,
        shader::ShaderProgramKind);
    friend ShaderParameterLayoutBuildResult BuildShaderParameterLayout(
        std::span<const ShaderProgramInterfaceRecord>,
        const PipelineBindingPolicy&,
        shader::ShaderProgramKind);
};

struct ShaderParameterLayoutBuildResult {
    std::optional<ShaderParameterLayout> Layout;
    vector<ShaderBindingDiagnostic> Diagnostics;

    bool Succeeded() const noexcept { return Layout.has_value() && Diagnostics.empty(); }
};

ShaderParameterLayoutBuildResult BuildShaderParameterLayout(
    const shader::ShaderBinary& binary,
    const PipelineBindingPolicy& policy,
    shader::ShaderProgramKind kind);
ShaderParameterLayoutBuildResult BuildShaderParameterLayout(
    std::span<const shader::ShaderInterfaceDesc> interfaces,
    const PipelineBindingPolicy& policy,
    shader::ShaderProgramKind kind);
ShaderParameterLayoutBuildResult BuildShaderParameterLayout(
    std::span<const ShaderProgramInterfaceRecord> interfaces,
    const PipelineBindingPolicy& policy,
    shader::ShaderProgramKind kind);

struct ShaderTextureParameterValue {
    StreamingAssetRef<TextureAsset> Texture;
    TextureSubViewDesc View{};
};

struct ShaderBufferParameterValue {
    Nullable<render::Buffer*> Buffer{nullptr};
    render::BufferRange Range{};
};

using ShaderResourceParameterValue = std::variant<
    std::monostate,
    ShaderTextureParameterValue,
    render::SamplerDescriptor,
    ShaderBufferParameterValue>;

struct ShaderParameterBindingValue {
    ShaderParameterLocation Location{};
    shader::ShaderBindingKind Kind{shader::ShaderBindingKind::Unknown};
    vector<byte> ConstantData;
    vector<ShaderResourceParameterValue> Resources;
};

class ShaderParameterSet {
public:
    bool Reset(const ShaderParameterLayout& layout, bool preserveCompatibleValues = false) noexcept;

    const ShaderParameterLayout& GetLayout() const noexcept { return _layout; }
    uint64_t GetRevision() const noexcept { return _revision; }
    bool IsComplete() const noexcept;
    bool IsCompleteFor(const ResolvedShaderBindingPlan& plan) const noexcept;
    std::span<const ShaderParameterBindingValue> Values() const noexcept { return _values; }

    bool SetFloat(std::string_view name, float value, uint32_t arrayIndex = 0) noexcept;
    bool SetFloat(
        ShaderParameterLocation location,
        std::string_view field,
        float value,
        uint32_t arrayIndex = 0) noexcept;
    bool SetInt(std::string_view name, int32_t value, uint32_t arrayIndex = 0) noexcept;
    bool SetInt(
        ShaderParameterLocation location,
        std::string_view field,
        int32_t value,
        uint32_t arrayIndex = 0) noexcept;
    bool SetUInt(std::string_view name, uint32_t value, uint32_t arrayIndex = 0) noexcept;
    bool SetUInt(
        ShaderParameterLocation location,
        std::string_view field,
        uint32_t value,
        uint32_t arrayIndex = 0) noexcept;
    bool SetBool(std::string_view name, bool value, uint32_t arrayIndex = 0) noexcept;
    bool SetBool(
        ShaderParameterLocation location,
        std::string_view field,
        bool value,
        uint32_t arrayIndex = 0) noexcept;
    bool SetVector(std::string_view name, const Eigen::Vector4f& value, uint32_t arrayIndex = 0) noexcept;
    bool SetVector(
        ShaderParameterLocation location,
        std::string_view field,
        const Eigen::Vector4f& value,
        uint32_t arrayIndex = 0) noexcept;
    bool SetValue(
        std::string_view name,
        shader::ShaderScalarType scalar,
        uint32_t columns,
        std::span<const byte> data,
        uint32_t arrayIndex = 0) noexcept;
    bool SetValue(
        ShaderParameterLocation location,
        std::string_view field,
        shader::ShaderScalarType scalar,
        uint32_t columns,
        std::span<const byte> data,
        uint32_t arrayIndex = 0) noexcept;
    bool SetMatrix(
        std::string_view name,
        std::span<const float> rowMajorValues,
        uint32_t rows,
        uint32_t columns,
        uint32_t arrayIndex = 0) noexcept;
    bool SetMatrix(
        ShaderParameterLocation location,
        std::string_view field,
        std::span<const float> rowMajorValues,
        uint32_t rows,
        uint32_t columns,
        uint32_t arrayIndex = 0) noexcept;
    bool SetConstantBuffer(ShaderParameterLocation location, std::span<const byte> data) noexcept;
    bool SetConstantBuffer(std::string_view name, std::span<const byte> data) noexcept;

    bool SetTexture(
        ShaderParameterLocation location,
        StreamingAssetRef<TextureAsset> texture,
        const TextureSubViewDesc& view = {},
        uint32_t arrayIndex = 0) noexcept;
    bool SetTexture(
        std::string_view name,
        StreamingAssetRef<TextureAsset> texture,
        const TextureSubViewDesc& view = {},
        uint32_t arrayIndex = 0) noexcept;
    bool SetSampler(
        ShaderParameterLocation location,
        const render::SamplerDescriptor& sampler,
        uint32_t arrayIndex = 0) noexcept;
    bool SetSampler(
        std::string_view name,
        const render::SamplerDescriptor& sampler,
        uint32_t arrayIndex = 0) noexcept;
    bool SetBuffer(
        ShaderParameterLocation location,
        Nullable<render::Buffer*> buffer,
        render::BufferRange range = render::BufferRange::AllRange(),
        uint32_t arrayIndex = 0) noexcept;
    bool SetBuffer(
        std::string_view name,
        Nullable<render::Buffer*> buffer,
        render::BufferRange range = render::BufferRange::AllRange(),
        uint32_t arrayIndex = 0) noexcept;
    bool ClearResource(ShaderParameterLocation location, uint32_t arrayIndex = 0) noexcept;
    bool ClearResource(std::string_view name, uint32_t arrayIndex = 0) noexcept;

private:
    struct FieldTarget {
        ShaderParameterBindingValue* Value{nullptr};
        const ShaderParameterFieldDesc* Field{nullptr};
    };

    ShaderParameterBindingValue* FindValue(ShaderParameterLocation location) noexcept;
    const ShaderParameterBindingValue* FindValue(ShaderParameterLocation location) const noexcept;
    FieldTarget FindField(std::string_view name) noexcept;
    FieldTarget FindField(
        ShaderParameterLocation location,
        std::string_view name) noexcept;
    bool SetValue(
        FieldTarget target,
        shader::ShaderScalarType scalar,
        uint32_t columns,
        std::span<const byte> data,
        uint32_t arrayIndex) noexcept;
    bool SetMatrix(
        FieldTarget target,
        std::span<const float> rowMajorValues,
        uint32_t rows,
        uint32_t columns,
        uint32_t arrayIndex) noexcept;
    bool SetResourceValue(
        const ShaderParameterBindingDesc& binding,
        ShaderResourceParameterValue value,
        uint32_t arrayIndex) noexcept;

    ShaderParameterLayout _layout;
    vector<ShaderParameterBindingValue> _values;
    uint64_t _revision{0};
};

}  // namespace radray
