#pragma once

#include <span>
#include <string_view>
#include <stdexcept>

#include <radray/allocator.h>
#include <radray/vertex_data.h>
#include <radray/structured_buffer.h>

#include <radray/render/common.h>
#include <radray/render/dxc.h>
#include <radray/render/spvc.h>
#include <radray/render/gpu_resource.h>

namespace radray::render {

struct SemanticMapping {
    std::string_view Semantic{};
    uint32_t SemanticIndex{0};
    uint32_t Location{0};
    VertexFormat Format{VertexFormat::UNKNOWN};
};
std::optional<vector<VertexElement>> MapVertexElements(std::span<const VertexBufferEntry> layouts, std::span<const SemanticMapping> semantics) noexcept;

// --------------------------------------- CBuffer Utility ---------------------------------------
class CBufferStorage {
public:
    CBufferStorage() noexcept = default;
    CBufferStorage(const HlslShaderDesc& hlslDesc, const RootSignatureDescriptor& rsDesc, const StructuredBufferStorage& storage);

    void Bind(CommandEncoder* encode, CBufferArena* arena);

private:
    StructuredBufferId _constantId{StructuredBufferId::Invalid()};
};

std::optional<StructuredBufferStorage> CreateCBufferStorage(const HlslShaderDesc& desc) noexcept;
std::optional<StructuredBufferStorage> CreateCBufferStorage(const SpirvShaderDesc& desc) noexcept;
// -----------------------------------------------------------------------------------------------

// ----------------------------------- Root Signature Utility ------------------------------------
class RootSignatureDescriptorContainer {
public:
    struct SetElementData {
        uint32_t Slot{0};
        uint32_t Space{0};
        ResourceBindType Type{ResourceBindType::UNKNOWN};
        uint32_t Count{0};
        ShaderStages Stages{ShaderStage::UNKNOWN};
        uint32_t StaticSamplerOffset{0};
        uint32_t StaticSamplerCount{0};
    };

    struct DescriptorSetRange {
        size_t ElementOffset{0};
        size_t ElementCount{0};
    };

    class View {
    public:
        const RootSignatureDescriptor& Get() const noexcept { return _desc; }

    private:
        RootSignatureDescriptor _desc{};
        vector<RootSignatureRootDescriptor> _rootDescriptors;
        vector<RootSignatureSetElement> _elements;
        vector<SamplerDescriptor> _staticSamplers;
        vector<RootSignatureDescriptorSet> _descriptorSets;

        friend class RootSignatureDescriptorContainer;
    };

    RootSignatureDescriptorContainer() noexcept = default;
    explicit RootSignatureDescriptorContainer(const RootSignatureDescriptor& desc) noexcept;

    View MakeView() const noexcept;

    std::span<const RootSignatureRootDescriptor> GetRootDescriptors() const noexcept { return _rootDescriptors; }
    std::span<const SetElementData> GetElements() const noexcept { return _elements; }
    std::span<const DescriptorSetRange> GetDescriptorSets() const noexcept { return _descriptorSets; }
    const std::optional<RootSignatureConstant>& GetConstant() const noexcept { return _constant; }

private:
    vector<RootSignatureRootDescriptor> _rootDescriptors;
    vector<SetElementData> _elements;
    vector<SamplerDescriptor> _staticSamplers;
    vector<DescriptorSetRange> _descriptorSets;
    std::optional<RootSignatureConstant> _constant;
};
Nullable<unique_ptr<RootSignature>> CreateSerializedRootSignature(Device* device, std::span<const byte> data) noexcept;
std::optional<RootSignatureDescriptorContainer> CreateRootSignatureDescriptor(const HlslShaderDesc& desc) noexcept;
std::optional<RootSignatureDescriptorContainer> CreateRootSignatureDescriptor(const SpirvShaderDesc& desc) noexcept;
// -----------------------------------------------------------------------------------------------

}  // namespace radray::render
