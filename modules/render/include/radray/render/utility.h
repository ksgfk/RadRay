#pragma once

#include <algorithm>
#include <utility>

#include <radray/vertex_data.h>

#include <radray/render/common.h>
#include <radray/render/dxc.h>

namespace radray::render {

struct StagedHlslShaderDesc {
    const HlslShaderDesc* Desc{nullptr};
    ShaderStage Stage{ShaderStage::UNKNOWN};
};

class ShaderCBufferType;

class ShaderCBufferVariable {
public:
private:
    string _name;
    const ShaderCBufferType* _type{nullptr};
    size_t _offset{0};
};

class ShaderCBufferType {
public:
private:
    string _name;
    vector<const ShaderCBufferVariable*> _members;
    size_t _size{0};
};

class ShaderCBufferLayout {
public:
private:
    string _name;
    const ShaderCBufferType* _root{nullptr};
    size_t _size{0};
};

class ShaderCBufferStorage {
public:
private:
    vector<ShaderCBufferLayout> _layout;
    vector<ShaderCBufferType> _types;
    vector<ShaderCBufferVariable> _variables;
    vector<byte> _buffer;
};

class HlslResourceBindingTableExtraData {
public:
    std::span<const StagedHlslShaderDesc> StagedDescs{};
};
using BuildResourceBindingTableExtraData = std::variant<HlslResourceBindingTableExtraData>;
class ResourceBindingTable : public RenderBase {
public:
    ~ResourceBindingTable() noexcept override = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTags{RenderObjectTag::UNKNOWN}; }
};
Nullable<unique_ptr<ResourceBindingTable>> CreateResourceBindingTable(Device* device, RootSignature* rs, const BuildResourceBindingTableExtraData& extraData) noexcept;

Nullable<unique_ptr<RootSignature>> CreateSerializedRootSignature(Device* device, std::span<const byte> data) noexcept;
class RootSignatureDescriptorContainer {
public:
    RootSignatureDescriptorContainer() noexcept = default;
    explicit RootSignatureDescriptorContainer(const RootSignatureDescriptor& desc) noexcept;
    RootSignatureDescriptorContainer(const RootSignatureDescriptorContainer&) noexcept;
    RootSignatureDescriptorContainer(RootSignatureDescriptorContainer&&) noexcept;
    RootSignatureDescriptorContainer& operator=(const RootSignatureDescriptorContainer&) noexcept;
    RootSignatureDescriptorContainer& operator=(RootSignatureDescriptorContainer&&) noexcept;
    ~RootSignatureDescriptorContainer() noexcept = default;

    const RootSignatureDescriptor& Get() const noexcept;

    friend void swap(RootSignatureDescriptorContainer& lhs, RootSignatureDescriptorContainer& rhs) noexcept;

private:
    void Refresh() noexcept;

    vector<RootSignatureRootDescriptor> _rootDescriptors;
    vector<RootSignatureSetElement> _elements;
    vector<RootSignatureDescriptorSet> _descriptorSets;
    RootSignatureDescriptor _desc{};
};
std::optional<RootSignatureDescriptorContainer> CreateRootSignatureDescriptor(std::span<const StagedHlslShaderDesc> descs) noexcept;

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
struct SemanticMapping {
    std::string_view Semantic{};
    uint32_t SemanticIndex{0};
    uint32_t Location{0};
    VertexFormat Format{VertexFormat::UNKNOWN};
};
std::optional<vector<VertexElement>> MapVertexElements(std::span<const VertexBufferEntry> layouts, std::span<const SemanticMapping> semantics) noexcept;

}  // namespace radray::render
