#pragma once

#include <optional>

#include <radray/guid.h>
#include <radray/hash.h>
#include <radray/render/common.h>
#include <radray/render/shader/hlsl.h>
#include <radray/types.h>

namespace radray {

struct ShaderVariantKey {
    Guid ProgramId;
    uint32_t PassIndex{0};
    uint32_t Backend{0};
    uint64_t KeywordBitmask{0};
    uint64_t SourceVersion{0};
    uint32_t ShaderModel{0};
    uint32_t CompileOptions{0};

    friend bool operator==(const ShaderVariantKey&, const ShaderVariantKey&) noexcept = default;
};

static_assert(std::is_trivially_copyable_v<ShaderVariantKey>);

struct ShaderVariantStageDesc {
    std::string_view Source{};
    std::string_view EntryPoint{};
    render::ShaderStage Stage{render::ShaderStage::UNKNOWN};
};

struct ShaderInterfaceBinding {
    string Name;
    uint32_t Group{0};
    uint32_t Binding{0};
    render::ShaderParameterKind Kind{render::ShaderParameterKind::Resource};
    render::ResourceBindType Type{render::ResourceBindType::UNKNOWN};
    uint32_t Count{1};
    render::ShaderStages Stages{render::ShaderStage::UNKNOWN};
    bool HasDynamicOffset{false};
    bool IsStaticSampler{false};
    bool Required{true};

    friend bool operator==(const ShaderInterfaceBinding&, const ShaderInterfaceBinding&) noexcept = default;
};

struct ShaderInterfacePushConstant {
    string Name;
    uint32_t Group{0};
    uint32_t Binding{0};
    render::ShaderStages Stages{render::ShaderStage::UNKNOWN};
    uint32_t Offset{0};
    uint32_t Size{0};
    bool Required{true};

    friend bool operator==(const ShaderInterfacePushConstant&, const ShaderInterfacePushConstant&) noexcept = default;
};

struct ShaderInterfaceSchema {
    vector<ShaderInterfaceBinding> Bindings;
    vector<ShaderInterfacePushConstant> PushConstants;
    bool AllowAdditionalBindings{false};
};

bool ValidateShaderInterfaceSchema(
    const ShaderInterfaceSchema& schema,
    const render::PipelineLayout& layout,
    string* error = nullptr) noexcept;

struct ShaderVariantDescriptor {
    Guid ProgramId{};
    uint32_t PassIndex{0};
    uint64_t KeywordBitmask{0};
    uint64_t SourceVersion{0};
    std::span<std::string_view> Defines{};
    std::span<std::string_view> Includes{};
    std::span<const ShaderVariantStageDesc> Stages{};
    std::span<const render::StaticSamplerDescriptor> StaticSamplers{};
    std::span<const render::DynamicBufferBinding> DynamicBufferBindings{};
    std::span<const render::PushConstantBinding> PushConstantBindings{};
    const ShaderInterfaceSchema* InterfaceSchema{nullptr};
    render::HlslShaderModel SM{render::HlslShaderModel::SM60};
    bool IsOptimize{false};
    std::string_view LogicalName{};
};

struct CompiledShaderVariant {
    struct DeclaredBinding {
        string Name;
        render::ShaderBindingLocation Location{};
    };

    render::Shader* VS{nullptr};
    render::Shader* PS{nullptr};
    render::Shader* CS{nullptr};
    render::PipelineLayout* Layout{nullptr};
    ShaderVariantKey Key{};
    vector<DeclaredBinding> DeclaredBindings;
};

std::optional<render::ShaderBindingLocation> FindShaderBindingLocation(
    const CompiledShaderVariant& variant,
    std::string_view name) noexcept;

struct ShaderVariantLibraryStats {
    uint64_t VariantHits{0};
    uint64_t VariantMisses{0};
    uint64_t ModuleHits{0};
    uint64_t ModuleMisses{0};
    uint64_t LayoutHits{0};
    uint64_t LayoutMisses{0};
};

std::optional<ShaderVariantKey> BuildShaderVariantKey(
    const ShaderVariantDescriptor& desc,
    render::RenderBackend backend) noexcept;

class PipelineLayoutLibrary {
public:
    explicit PipelineLayoutLibrary(render::Device* device) noexcept;
    ~PipelineLayoutLibrary() noexcept;
    PipelineLayoutLibrary(const PipelineLayoutLibrary&) = delete;
    PipelineLayoutLibrary& operator=(const PipelineLayoutLibrary&) = delete;

    Nullable<render::PipelineLayout*> GetOrCreate(
        const render::PipelineLayoutDescriptor& desc,
        const ShaderInterfaceSchema* schema = nullptr) noexcept;
    void Clear() noexcept;
    uint32_t Count() const noexcept { return static_cast<uint32_t>(_entries.size()); }
    uint64_t GetHitCount() const noexcept { return _hits; }
    uint64_t GetMissCount() const noexcept { return _misses; }

private:
    struct Entry {
        string Key;
        unique_ptr<render::PipelineLayout> Layout;
    };

    struct BindingGroupLayoutEntry {
        string Key;
        render::PipelineLayout* Layout{nullptr};
        uint32_t Group{0};
    };

    static string BuildCanonicalKey(
        render::PipelineLayout& layout,
        const render::PipelineLayoutDescriptor& desc) noexcept;
    static std::optional<string> BuildBindingGroupCanonicalKey(
        const render::BindingGroupLayout& group,
        const render::PipelineLayoutDescriptor& desc) noexcept;

    render::Device* _device;
    vector<Entry> _entries;
    vector<BindingGroupLayoutEntry> _bindingGroupLayouts;
    uint64_t _hits{0};
    uint64_t _misses{0};
};

class ShaderVariantLibrary {
public:
    virtual ~ShaderVariantLibrary() noexcept = default;
    virtual Nullable<const CompiledShaderVariant*> Find(const ShaderVariantKey& key) const noexcept = 0;
    virtual Nullable<const CompiledShaderVariant*> GetOrCreate(const ShaderVariantDescriptor& desc) noexcept = 0;
    virtual void Clear() noexcept = 0;
    virtual uint32_t Count() const noexcept = 0;
    virtual ShaderVariantLibraryStats GetStats() const noexcept = 0;
};

Nullable<unique_ptr<ShaderVariantLibrary>> CreatePrecompiledShaderVariantLibrary(
    render::Device* device,
    PipelineLayoutLibrary* layoutLibrary,
    std::string_view bakeRoot) noexcept;

#ifdef RADRAY_ENABLE_DXC
namespace render {
class Dxc;
}

Nullable<unique_ptr<ShaderVariantLibrary>> CreateShaderVariantLibrary(
    render::Device* device,
    render::Dxc* dxc,
    PipelineLayoutLibrary* layoutLibrary) noexcept;
#endif

}  // namespace radray
