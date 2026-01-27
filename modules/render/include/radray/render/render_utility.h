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
std::optional<StructuredBufferStorage> CreateCBufferStorage(const HlslShaderDesc& desc) noexcept;
std::optional<StructuredBufferStorage> CreateCBufferStorage(const SpirvShaderDesc& desc) noexcept;
// -----------------------------------------------------------------------------------------------

// ----------------------------------- Root Signature Utility ------------------------------------
class RootSignatureBinder;

class RootSignatureDetail {
public:
    struct BindData {
        string Name;
        uint32_t BindPoint;
        uint32_t BindCount;
        uint32_t Space;
        ShaderStages Stages;
    };

    struct PushConst : BindData {
        uint32_t Size;
    };

    struct RootDesc : BindData {
        ResourceBindType Type;
    };

    struct DescSetElem : BindData {
        ResourceBindType Type;
    };

    struct DescSet {
        vector<DescSetElem> Elems;
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

        friend class RootSignatureDetail;
    };

    RootSignatureDetail() noexcept = default;
    explicit RootSignatureDetail(const HlslShaderDesc& desc) noexcept;
    explicit RootSignatureDetail(const SpirvShaderDesc& desc) noexcept;
    RootSignatureDetail(
        vector<DescSet> descSets,
        vector<RootDesc> rootDescs,
        std::optional<PushConst> pushConst) noexcept;

    View MakeView() const noexcept;
    RootSignatureBinder MakeBinder() const noexcept;

    const std::optional<PushConst>& GetPushConstant() const noexcept { return _pushConst; }
    std::span<const RootDesc> GetRootDescs() const noexcept { return _rootDescs; }
    std::span<const DescSet> GetDescSets() const noexcept { return _descSets; }

private:
    vector<DescSet> _descSets;
    vector<RootDesc> _rootDescs;
    std::optional<PushConst> _pushConst;
};

class RootSignatureBinder {
public:
private:
    friend class RootSignatureDetail;
};

Nullable<unique_ptr<RootSignature>> CreateSerializedRootSignature(Device* device, std::span<const byte> data) noexcept;

std::optional<RootSignatureDetail> CreateRootSignatureDetail(const HlslShaderDesc& desc) noexcept;
// -----------------------------------------------------------------------------------------------

}  // namespace radray::render
