#pragma once

#include <radray/types.h>
#include <radray/render/common.h>
#include <radray/runtime/shader_variant.h>

namespace radray {

struct ShaderCompileKey {
    string Name{};
    string EntryPoint{};
    render::ShaderStage Stage{render::ShaderStage::UNKNOWN};
    render::RenderBackend Backend{render::RenderBackend::D3D12};
    ShaderVariantKey Variant{};

    friend bool operator==(const ShaderCompileKey&, const ShaderCompileKey&) noexcept = default;
};

struct RootSignatureLayoutKey {
    uint32_t DescriptorSetCount{0};
    vector<render::BindingParameterLayout> Parameters{};
    vector<render::PushConstantRange> PushConstantRanges{};
    vector<render::BindlessSetLayout> BindlessSetLayouts{};
    vector<render::StaticSamplerLayout> StaticSamplerLayouts{};

    static RootSignatureLayoutKey From(const render::RootSignatureLayoutPreview& preview);

    friend bool operator==(const RootSignatureLayoutKey&, const RootSignatureLayoutKey&) noexcept = default;
};

struct CompiledShaderEntry {
    render::Shader* Target{nullptr};
    ShaderCompileKey Key{};
};

}  // namespace radray
