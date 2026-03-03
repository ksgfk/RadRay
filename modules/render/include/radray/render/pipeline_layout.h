#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <variant>

#include <radray/hash.h>
#include <radray/render/common.h>
#include <radray/render/shader_reflection.h>

namespace radray::render {

using ParameterNameToIdMap = unordered_map<string, uint32_t, StringHash, StringEqual>;

class RootSignatureDescriptorContainer {
public:
    RootSignatureDescriptorContainer() noexcept = default;
    explicit RootSignatureDescriptorContainer(const RootSignatureDescriptor& desc) noexcept;

    const RootSignatureDescriptor& Get() const noexcept { return _desc; }

public:
    RootSignatureDescriptor _desc{};
    vector<RootSignatureRootDescriptor> _rootDescriptors;
    vector<RootSignatureSetElement> _elements;
    vector<RootSignatureStaticSampler> _staticSamplers;
    vector<RootSignatureDescriptorSet> _descriptorSets;
};

struct StaticSamplerBinding {
    string Name;
    vector<SamplerDescriptor> Samplers;
};

struct PushConstantLocation {
    uint32_t Offset{0};
    uint32_t Size{0};
};

struct RootDescriptorLocation {
    uint32_t RootIndex{0};
};

struct DescriptorTableLocation {
    uint32_t SetIndex{0};
    uint32_t ElementIndex{0};
};

using ParameterLocation = std::variant<
    PushConstantLocation,
    RootDescriptorLocation,
    DescriptorTableLocation>;

struct ParameterMapping {
    uint32_t Id{0};
    ShaderParameter Parameter;
    ParameterLocation Location;
    bool IsStaticSampler{false};
    vector<SamplerDescriptor> StaticSamplerDescs;
};

struct PipelineLayoutPlan {
    RootSignatureDescriptorContainer SignatureDesc;
    ParameterNameToIdMap MappingIndicesByName;
    vector<ParameterMapping> Mappings;
};

class IPipelineLayoutPlanner {
public:
    virtual ~IPipelineLayoutPlanner() noexcept = default;

    virtual std::optional<PipelineLayoutPlan> Plan(
        std::span<const ShaderParameter> parameters,
        std::span<const StaticSamplerBinding> staticSamplers) const noexcept = 0;
};

class D3D12LayoutPlanner final : public IPipelineLayoutPlanner {
public:
    std::optional<PipelineLayoutPlan> Plan(
        std::span<const ShaderParameter> parameters,
        std::span<const StaticSamplerBinding> staticSamplers) const noexcept override;
};

class VulkanLayoutPlanner final : public IPipelineLayoutPlanner {
public:
    std::optional<PipelineLayoutPlan> Plan(
        std::span<const ShaderParameter> parameters,
        std::span<const StaticSamplerBinding> staticSamplers) const noexcept override;
};

class MetalLayoutPlanner final : public IPipelineLayoutPlanner {
public:
    std::optional<PipelineLayoutPlan> Plan(
        std::span<const ShaderParameter> parameters,
        std::span<const StaticSamplerBinding> staticSamplers) const noexcept override;
};

class PipelineLayout {
public:
    PipelineLayout() noexcept = default;
    explicit PipelineLayout(const HlslShaderDesc& desc, std::span<const StaticSamplerBinding> staticSamplers = {}) noexcept;
    explicit PipelineLayout(const SpirvShaderDesc& desc, std::span<const StaticSamplerBinding> staticSamplers = {}) noexcept;
    explicit PipelineLayout(const MslShaderReflection& desc, std::span<const StaticSamplerBinding> staticSamplers = {}) noexcept;
    explicit PipelineLayout(
        std::span<const ShaderParameter> parameters,
        const IPipelineLayoutPlanner& planner,
        std::span<const StaticSamplerBinding> staticSamplers = {}) noexcept;

    RootSignatureDescriptorContainer GetDescriptor() const noexcept;
    const PipelineLayoutPlan& GetPlan() const noexcept { return _plan; }
    std::span<const ParameterMapping> GetMappings() const noexcept { return _plan.Mappings; }
    std::optional<uint32_t> GetParameterId(std::string_view name) const noexcept;
    std::span<const ParameterMapping> GetBindings() const noexcept { return GetMappings(); }
    std::optional<uint32_t> GetBindingId(std::string_view name) const noexcept { return GetParameterId(name); }
    const StructuredBufferStorage::Builder& GetCBufferStorageBuilder() const noexcept { return _cbStorageBuilder; }

    static RootSignatureDescriptorContainer BuildDescriptor(std::span<const ParameterMapping> mappings) noexcept;
    static ParameterNameToIdMap BuildNameIndex(vector<ParameterMapping>& mappings) noexcept;
    static void ApplyStaticSamplers(vector<ParameterMapping>& mappings, std::span<const StaticSamplerBinding> staticSamplers) noexcept;

private:
    void SetPlan(PipelineLayoutPlan&& plan) noexcept;

    PipelineLayoutPlan _plan{};
    StructuredBufferStorage::Builder _cbStorageBuilder{};
};

}  // namespace radray::render
