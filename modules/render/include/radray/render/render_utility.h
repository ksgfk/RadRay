#pragma once

#include <span>
#include <string_view>
#include <stdexcept>
#include <unordered_map>

#include <radray/allocator.h>
#include <radray/vertex_data.h>
#include <radray/structured_buffer.h>

#include <radray/render/common.h>
#include <radray/render/dxc.h>
#include <radray/render/spvc.h>
#include <radray/render/gpu_resource.h>

namespace radray::render {

class BindBridgeLayout;

struct SemanticMapping {
    std::string_view Semantic{};
    uint32_t SemanticIndex{0};
    uint32_t Location{0};
    VertexFormat Format{VertexFormat::UNKNOWN};
};
std::optional<vector<VertexElement>> MapVertexElements(std::span<const VertexBufferEntry> layouts, std::span<const SemanticMapping> semantics) noexcept;

std::optional<StructuredBufferStorage::Builder> CreateCBufferStorageBuilder(const HlslShaderDesc& desc) noexcept;
std::optional<StructuredBufferStorage::Builder> CreateCBufferStorageBuilder(const SpirvShaderDesc& desc) noexcept;

class RootSignatureDescriptorContainer {
public:
    const RootSignatureDescriptor& Get() const noexcept { return _desc; }

private:
    RootSignatureDescriptor _desc{};
    vector<RootSignatureRootDescriptor> _rootDescriptors;
    vector<RootSignatureSetElement> _elements;
    vector<SamplerDescriptor> _staticSamplers;
    vector<RootSignatureDescriptorSet> _descriptorSets;

    friend class BindBridgeLayout;
};

Nullable<unique_ptr<RootSignature>> CreateSerializedRootSignature(Device* device, std::span<const byte> data) noexcept;

}  // namespace radray::render
