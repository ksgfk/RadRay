#pragma once

#include <radray/shader/shader_layout.h>

namespace radray::test {
inline shader::ShaderPipelineLayoutDesc MatrixLayout() {
    shader::ShaderPipelineLayoutDesc desc;
    desc.Types.push_back({.Name = "Payload", .Size = 92, .ElementSize = 92});
    desc.Types.push_back({.Name = "Matrices", .ParentIndex = 0, .Kind = shader::ShaderTypeKind::Array, .ElementKind = shader::ShaderTypeKind::Matrix, .ElementCount = 2, .Size = 92, .ElementSize = 44, .Stride = 48, .MatrixStride = 16, .ScalarKind = shader::ShaderScalarKind::Float, .RowCount = 3, .ColumnCount = 3});
    shader::ShaderBindingGroupDesc group{.Name = "Material", .GroupSlot = 2};
    group.Bindings.push_back({.Name = "Constants", .Binding = {.BindingId = 7, .Kind = gpu::BindingKind::CBuffer, .Stages = gpu::ShaderStage::Compute, .D3D12 = gpu::D3D12BindingDescriptor{}, .Vulkan = gpu::VulkanBindingDescriptor{.Set = 4}}, .TypeIndex = 0, .BufferByteSize = 96});
    desc.Groups.push_back(std::move(group));
    return desc;
}

inline shader::ShaderPipelineLayoutDesc ResourceLayout() {
    shader::ShaderPipelineLayoutDesc desc;
    shader::ShaderBindingGroupDesc group{.Name = "Resources", .GroupSlot = 0};
    for (uint32_t id = 0; id < 2; ++id) group.Bindings.push_back({.Name = id == 0 ? "First" : "Second", .Binding = {.BindingId = id, .Kind = gpu::BindingKind::Texture, .Stages = gpu::ShaderStage::Pixel, .D3D12 = gpu::D3D12BindingDescriptor{gpu::BindingRegisterClass::SRV, id}, .Vulkan = gpu::VulkanBindingDescriptor{0, id}}});
    desc.Groups.push_back(std::move(group));
    return desc;
}
}  // namespace radray::test
