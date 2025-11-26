#pragma once

#include <radray/vertex_data.h>

#include <radray/render/common.h>
#include <radray/render/dxc.h>

namespace radray::render {

class RootSignatureSetElementContainer {
public:
    explicit RootSignatureSetElementContainer(const RootSignatureSetElement& elem) noexcept;
    RootSignatureSetElementContainer(const RootSignatureSetElementContainer&) noexcept;
    RootSignatureSetElementContainer(RootSignatureSetElementContainer&&) noexcept;
    RootSignatureSetElementContainer& operator=(const RootSignatureSetElementContainer&) noexcept;
    RootSignatureSetElementContainer& operator=(RootSignatureSetElementContainer&&) noexcept;
    ~RootSignatureSetElementContainer() noexcept = default;

    static vector<RootSignatureSetElementContainer> FromView(std::span<const RootSignatureSetElement> elems) noexcept;

    friend void swap(RootSignatureSetElementContainer& lhs, RootSignatureSetElementContainer& rhs) noexcept;

public:
    void Refresh() noexcept;

    RootSignatureSetElement _elem;
    vector<SamplerDescriptor> _staticSamplers;
};

struct SemanticMapping {
    std::string_view Semantic;
    uint32_t SemanticIndex;
    uint32_t Location;
    VertexFormat Format;
};

class RootSignatureDescriptorContainer {
public:
    RootSignatureDescriptorContainer() noexcept = default;
    explicit RootSignatureDescriptorContainer(const RootSignatureDescriptor& desc) noexcept;
    RootSignatureDescriptorContainer(const RootSignatureDescriptorContainer&) noexcept;
    RootSignatureDescriptorContainer(RootSignatureDescriptorContainer&&) noexcept;
    RootSignatureDescriptorContainer& operator=(const RootSignatureDescriptorContainer&) noexcept;
    RootSignatureDescriptorContainer& operator=(RootSignatureDescriptorContainer&&) noexcept;
    ~RootSignatureDescriptorContainer() noexcept = default;

    RootSignatureDescriptor Get() const noexcept;

    friend void swap(RootSignatureDescriptorContainer& lhs, RootSignatureDescriptorContainer& rhs) noexcept;

private:
    void Refresh() noexcept;

    struct DescriptorSetRange {
        size_t Offset{0};
        size_t Count{0};
    };

    struct SamplerRange {
        size_t Offset{0};
        size_t Count{0};
    };

    vector<RootSignatureRootDescriptor> _rootDescriptors{};
    vector<RootSignatureSetElement> _descriptorSetElements{};
    vector<DescriptorSetRange> _descriptorSetRanges{};
    vector<RootSignatureDescriptorSet> _descriptorSets{};
    vector<SamplerDescriptor> _staticSamplers{};
    vector<SamplerRange> _staticSamplerRanges{};
    std::optional<RootSignatureConstant> _constant{};
};

bool IsDepthStencilFormat(TextureFormat format) noexcept;
uint32_t GetVertexFormatSize(VertexFormat format) noexcept;
uint32_t GetIndexFormatSize(IndexFormat format) noexcept;
PrimitiveState DefaultPrimitiveState() noexcept;
DepthStencilState DefaultDepthStencilState() noexcept;
StencilState DefaultStencilState() noexcept;
MultiSampleState DefaultMultiSampleState() noexcept;
ColorTargetState DefaultColorTargetState(TextureFormat format) noexcept;
BlendState DefaultBlendState() noexcept;
IndexFormat MapIndexType(uint32_t size) noexcept;
std::optional<vector<VertexElement>> MapVertexElements(std::span<const VertexBufferEntry> layouts, std::span<const SemanticMapping> semantics) noexcept;

Nullable<shared_ptr<RootSignature>> CreateSerializedRootSignature(Device* device, std::span<const byte> data) noexcept;
class StagedHlslShaderDesc {
public:
    const HlslShaderDesc* Desc{nullptr};
    ShaderStage Stage{ShaderStage::UNKNOWN};
};
std::optional<RootSignatureDescriptorContainer> GenerateRSDescFromHlslShaderDescs(std::span<const StagedHlslShaderDesc> descs) noexcept;

}  // namespace radray::render
