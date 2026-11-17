#pragma once
#include "test_fakes.h"
#include "../../shader/tests/layout_test_data.h"
#include <radray/runtime/asset_manager.h>

namespace radray::test {
class AssetTestShader final : public gpu::Shader {
public:
    AssetTestShader(gpu::ShaderStages stages, uint32_t& destroyed) : Stages(stages), Destroyed(destroyed) {}
    ~AssetTestShader() noexcept override { ++Destroyed; }
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    gpu::ShaderStages GetStages() const noexcept override { return Stages; }
    gpu::ShaderStages Stages;
    uint32_t& Destroyed;
};
class LayoutGroup final : public gpu::BindingGroupLayout {
public:
    explicit LayoutGroup(std::span<const gpu::BindingDescriptor> entries, uint32_t& destroyed) : Bindings(entries.begin(), entries.end()), Destroyed(destroyed) {}
    ~LayoutGroup() noexcept override { ++Destroyed; }
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}
    gpu::BindingGroupLayoutDescriptor GetDesc() const noexcept override { return {Bindings}; }
    vector<gpu::BindingDescriptor> Bindings;
    uint32_t& Destroyed;
};

class LayoutPipeline final : public gpu::PipelineLayout {
public:
    explicit LayoutPipeline(const gpu::PipelineLayoutDescriptor& desc, uint32_t& destroyed)
        : Groups(desc.Groups.begin(), desc.Groups.end()), Pushes(desc.PushConstants.begin(), desc.PushConstants.end()), Destroyed(destroyed) {}
    ~LayoutPipeline() noexcept override { ++Destroyed; }
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}
    gpu::PipelineLayoutDescriptor GetDesc() const noexcept override { return {Groups, Pushes}; }
    vector<gpu::PipelineLayoutGroupDescriptor> Groups;
    vector<gpu::PushConstantDescriptor> Pushes;
    uint32_t& Destroyed;
};

class LayoutDevice final : public test::FakeDevice {
public:
    uint32_t ShaderCreates{0}, GroupCreates{0}, LayoutCreates{0}, ShaderDestroys{0};
    uint32_t GroupDestroys{0}, LayoutDestroys{0};
    uint32_t FailShaderAt{0};
    bool FailGroup{false}, FailLayout{false};
    Nullable<unique_ptr<gpu::Shader>> CreateShader(const gpu::ShaderDescriptor& desc) noexcept override {
        ++ShaderCreates;
        if (FailShaderAt == ShaderCreates) return nullptr;
        return make_unique<AssetTestShader>(desc.Stages, ShaderDestroys);
    }
    gpu::RenderBackend Backend{gpu::RenderBackend::D3D12};
    gpu::RenderBackend GetBackend() noexcept override { return Backend; }
    Nullable<unique_ptr<gpu::BindingGroupLayout>> CreateBindingGroupLayout(const gpu::BindingGroupLayoutDescriptor& desc) noexcept override {
        ++GroupCreates;
        if (FailGroup) return nullptr;
        return make_unique<LayoutGroup>(desc.Bindings, GroupDestroys);
    }
    Nullable<unique_ptr<gpu::PipelineLayout>> CreatePipelineLayout(const gpu::PipelineLayoutDescriptor& desc) noexcept override {
        ++LayoutCreates;
        if (FailLayout) return nullptr;
        return make_unique<LayoutPipeline>(desc, LayoutDestroys);
    }
};



inline shader::ShaderPipelineLayoutDesc MaterialLayout() {
    auto desc = MatrixLayout();
    desc.Types.clear();
    desc.Types.push_back({.Name = "MaterialData", .Size = 32, .ElementSize = 32});
    for (const auto& [name, offset] : array{std::pair{"Roughness", 0u}, std::pair{"Metallic", 4u}})
        desc.Types.push_back({.Name = name, .ParentIndex = 0, .Kind = shader::ShaderTypeKind::Scalar, .ElementKind = shader::ShaderTypeKind::Scalar, .Offset = offset, .Size = 4, .ElementSize = 4, .ScalarKind = shader::ShaderScalarKind::Float, .RowCount = 1, .ColumnCount = 1});
    desc.Types.push_back({.Name = "Tint", .ParentIndex = 0, .Kind = shader::ShaderTypeKind::Vector, .ElementKind = shader::ShaderTypeKind::Vector, .Offset = 16, .Size = 16, .ElementSize = 16, .ScalarKind = shader::ShaderScalarKind::Float, .RowCount = 1, .ColumnCount = 4});
    desc.Groups[0].Bindings[0].BufferByteSize = 32;
    return desc;
}



inline ShaderAssetDesc MakeShaderDesc(shader::ShaderPipelineLayoutDesc layout = {}, gpu::RenderBackend backend = gpu::RenderBackend::D3D12) {
    ShaderAssetDesc desc;
    desc.Family.SharedLayout = std::move(layout);
    shader::ShaderVariantInterfaceDesc program;
    program.Name = "Default";
    shader::ShaderVariantTargetInterfaceDesc interface;
    interface.Stages = gpu::ShaderStage::Vertex | gpu::ShaderStage::Pixel;
    if (backend == gpu::RenderBackend::D3D12)
        program.DXIL = interface;
    else
        program.SPIRV = interface;
    desc.Family.Variants.push_back(program);
    desc.Stages = {{0, backend, gpu::ShaderStage::Vertex, "VS", {byte{1}}}, {0, backend, gpu::ShaderStage::Pixel, "PS", {byte{2}}}};
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
