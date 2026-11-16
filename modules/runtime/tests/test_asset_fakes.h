#pragma once
#include "test_fakes.h"
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/pipeline_layout_cache.h>

namespace radray::test {
class AssetTestShader final : public render::Shader {
public:
    AssetTestShader(render::ShaderStages stages, uint32_t& destroyed) : Stages(stages), Destroyed(destroyed) {}
    ~AssetTestShader() noexcept override { ++Destroyed; }
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    render::ShaderStages GetStages() const noexcept override { return Stages; }
    render::ShaderStages Stages;
    uint32_t& Destroyed;
};
class LayoutGroup final : public render::BindingGroupLayout {
public:
    explicit LayoutGroup(std::span<const render::BindingDescriptor> entries) : Bindings(entries.begin(), entries.end()) {}
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}
    render::BindingGroupLayoutDescriptor GetDesc() const noexcept override { return {Bindings}; }
    vector<render::BindingDescriptor> Bindings;
};

class LayoutPipeline final : public render::PipelineLayout {
public:
    explicit LayoutPipeline(const render::PipelineLayoutDescriptor& desc)
        : Groups(desc.Groups.begin(), desc.Groups.end()), Pushes(desc.PushConstants.begin(), desc.PushConstants.end()) {}
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}
    render::PipelineLayoutDescriptor GetDesc() const noexcept override { return {Groups, Pushes}; }
    vector<render::PipelineLayoutGroupDescriptor> Groups;
    vector<render::PushConstantDescriptor> Pushes;
};

class LayoutDevice final : public test::FakeDevice {
public:
    uint32_t ShaderCreates{0}, GroupCreates{0}, LayoutCreates{0}, ShaderDestroys{0};
    uint32_t FailShaderAt{0};
    bool FailGroup{false}, FailLayout{false};
    Nullable<unique_ptr<render::Shader>> CreateShader(const render::ShaderDescriptor& desc) noexcept override {
        ++ShaderCreates;
        if (FailShaderAt == ShaderCreates) return nullptr;
        return make_unique<AssetTestShader>(desc.Stages, ShaderDestroys);
    }
    render::RenderBackend Backend{render::RenderBackend::D3D12};
    render::RenderBackend GetBackend() noexcept override { return Backend; }
    Nullable<unique_ptr<render::BindingGroupLayout>> CreateBindingGroupLayout(const render::BindingGroupLayoutDescriptor& desc) noexcept override {
        ++GroupCreates;
        if (FailGroup) return nullptr;
        return make_unique<LayoutGroup>(desc.Bindings);
    }
    Nullable<unique_ptr<render::PipelineLayout>> CreatePipelineLayout(const render::PipelineLayoutDescriptor& desc) noexcept override {
        ++LayoutCreates;
        if (FailLayout) return nullptr;
        return make_unique<LayoutPipeline>(desc);
    }
};

inline ShaderPipelineLayoutDesc MatrixLayout() {
    ShaderPipelineLayoutDesc desc;
    desc.Types.push_back({.Name = "Payload", .Size = 92, .ElementSize = 92});
    desc.Types.push_back({.Name = "Matrices", .ParentIndex = 0, .Kind = shader::ShaderTypeKind::Array, .ElementKind = shader::ShaderTypeKind::Matrix, .ElementCount = 2, .Size = 92, .ElementSize = 44, .Stride = 48, .MatrixStride = 16, .ScalarKind = shader::ShaderScalarKind::Float, .RowCount = 3, .ColumnCount = 3});
    ShaderBindingGroupDesc group{.Name = "Material", .GroupSlot = 2};
    group.Bindings.push_back({.Name = "Constants", .Binding = {.BindingId = 7, .Kind = render::BindingKind::CBuffer, .Stages = render::ShaderStage::Compute, .D3D12 = render::D3D12BindingDescriptor{}, .Vulkan = render::VulkanBindingDescriptor{.Set = 4}}, .TypeIndex = 0, .BufferByteSize = 96});
    desc.Groups.push_back(std::move(group));
    return desc;
}

inline ShaderPipelineLayoutDesc MaterialLayout() {
    auto desc = MatrixLayout();
    desc.Types.clear();
    desc.Types.push_back({.Name = "MaterialData", .Size = 32, .ElementSize = 32});
    for (const auto& [name, offset] : array{std::pair{"Roughness", 0u}, std::pair{"Metallic", 4u}})
        desc.Types.push_back({.Name = name, .ParentIndex = 0, .Kind = shader::ShaderTypeKind::Scalar, .ElementKind = shader::ShaderTypeKind::Scalar, .Offset = offset, .Size = 4, .ElementSize = 4, .ScalarKind = shader::ShaderScalarKind::Float, .RowCount = 1, .ColumnCount = 1});
    desc.Types.push_back({.Name = "Tint", .ParentIndex = 0, .Kind = shader::ShaderTypeKind::Vector, .ElementKind = shader::ShaderTypeKind::Vector, .Offset = 16, .Size = 16, .ElementSize = 16, .ScalarKind = shader::ShaderScalarKind::Float, .RowCount = 1, .ColumnCount = 4});
    desc.Groups[0].Bindings[0].BufferByteSize = 32;
    return desc;
}

inline ShaderPipelineLayoutDesc ResourceLayout() {
    ShaderPipelineLayoutDesc desc;
    ShaderBindingGroupDesc group{.Name = "Resources", .GroupSlot = 0};
    for (uint32_t id = 0; id < 2; ++id) group.Bindings.push_back({.Name = id == 0 ? "First" : "Second", .Binding = {.BindingId = id, .Kind = render::BindingKind::Texture, .Stages = render::ShaderStage::Pixel, .D3D12 = render::D3D12BindingDescriptor{render::BindingRegisterClass::SRV, id}, .Vulkan = render::VulkanBindingDescriptor{0, id}}});
    desc.Groups.push_back(std::move(group));
    return desc;
}

inline ShaderAssetDesc MakeShaderDesc(ShaderPipelineLayoutDesc layout = {}, render::RenderBackend backend = render::RenderBackend::D3D12) {
    ShaderAssetDesc desc;
    desc.Family.SharedLayout = std::move(layout);
    ShaderProgramInterfaceDesc program;
    program.Name = "Default";
    ShaderTargetProgramInterfaceDesc interface;
    interface.Stages = render::ShaderStage::Vertex | render::ShaderStage::Pixel;
    if (backend == render::RenderBackend::D3D12)
        program.DXIL = interface;
    else
        program.SPIRV = interface;
    desc.Family.Variants.push_back(program);
    desc.Stages = {{0, backend, render::ShaderStage::Vertex, "VS", {byte{1}}}, {0, backend, render::ShaderStage::Pixel, "PS", {byte{2}}}};
    return desc;
}
inline MaterialAssetDesc MakeMaterialDesc(AssetHandle<ShaderAsset> shader, vector<MaterialParameter> parameters = {}, vector<string> subset = {}, string block = {}) {
    MaterialAssetDesc desc;
    desc.Programs = {{"Color", shader, 0, "VS", "PS"}, {"Depth", shader, 0, "VS", ""}};
    desc.Parameters = std::move(parameters);
    desc.Families = {{shader, std::move(block), std::move(subset)}};
    return desc;
}
}  // namespace radray::test
