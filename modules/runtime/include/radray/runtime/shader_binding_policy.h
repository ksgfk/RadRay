#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <utility>

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
    render::ShaderDiagnosticContext Context{};
    std::optional<render::ShaderDiagnosticContext> RelatedContext{};
    string ProviderName;

    friend bool operator==(const ShaderBindingDiagnostic&, const ShaderBindingDiagnostic&) = default;
};

struct ShaderBindingProviderMatchResult {
    bool Compatible{false};
    string Message;
    std::optional<uint32_t> Binding{};

    static ShaderBindingProviderMatchResult Success() { return {.Compatible = true, .Message = {}}; }
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
        const render::ShaderBindingGroupInterfaceDesc& group) const = 0;
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
    render::ShaderBindingGroupInterfaceDesc Interface;
};

struct ResolvedShaderBindingPlan {
    render::ShaderHash InterfaceHash{};
    vector<ResolvedProviderBindingGroup> ProviderGroups;
    vector<render::ShaderBindingGroupInterfaceDesc> UserGroups;
};

struct ShaderBindingResolutionResult {
    std::optional<ResolvedShaderBindingPlan> Plan;
    vector<ShaderBindingDiagnostic> Diagnostics;

    bool Succeeded() const noexcept { return Plan.has_value() && Diagnostics.empty(); }
};

ShaderBindingResolutionResult ResolveShaderBindings(
    const render::ShaderInterfaceDesc& interface,
    const PipelineBindingPolicy& policy,
    const render::ShaderDiagnosticContext& context = {});

struct ShaderBindingProviderSchemaEntry {
    // Alternatives share one binding index. This permits a provider to support
    // several physical projections at the same reserved location.
    vector<render::ShaderBindingDesc> AcceptedBindings;
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
        const render::ShaderBindingGroupInterfaceDesc& group) const override;

private:
    string _name;
    vector<ShaderBindingProviderSchemaEntry> _entries;
    bool _valid{false};
};

}  // namespace radray
