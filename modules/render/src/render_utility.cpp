#include <radray/render/render_utility.h>

#include <radray/errors.h>

#include <radray/basic_math.h>

#include <cstring>
#include <bit>

#ifdef RADRAY_ENABLE_D3D12
#include <radray/render/backend/d3d12_impl.h>
#endif
#ifdef RADRAY_ENABLE_VULKAN
#include <radray/render/backend/vulkan_impl.h>
#endif

namespace radray::render {

std::optional<vector<VertexElement>> MapVertexElements(std::span<const VertexBufferEntry> layouts, std::span<const SemanticMapping> semantics) noexcept {
    vector<VertexElement> result;
    result.reserve(semantics.size());
    for (const auto& want : semantics) {
        uint32_t wantSize = GetVertexFormatSizeInBytes(want.Format);
        const VertexBufferEntry* found = nullptr;
        for (const auto& l : layouts) {
            uint32_t preSize = GetVertexDataSizeInBytes(l.Type, l.ComponentCount);
            if (l.Semantic == want.Semantic && l.SemanticIndex == want.SemanticIndex && preSize == wantSize) {
                found = &l;
                break;
            }
        }
        if (!found) {
            return std::nullopt;
        }
        VertexElement& ve = result.emplace_back();
        ve.Offset = found->Offset;
        ve.Semantic = found->Semantic;
        ve.SemanticIndex = found->SemanticIndex;
        ve.Format = want.Format;
        ve.Location = want.Location;
    }
    return result;
}

std::optional<StructuredBufferStorage> CreateCBufferStorage(const HlslShaderDesc& desc) noexcept {
    StructuredBufferStorage::Builder builder{};
    builder.SetAlignment(0);
    auto createType = [&](size_t parent, StructuredBufferId bdType, size_t size) {
        struct TypeCreateCtx {
            size_t par;
            StructuredBufferId bd;
            size_t s;
        };
        stack<TypeCreateCtx> s;
        s.push({parent, bdType, size});
        while (!s.empty()) {
            auto ctx = s.top();
            s.pop();
            const auto& type = desc.Types[ctx.par];
            for (size_t i = 0; i < type.Members.size(); i++) {
                const auto& member = type.Members[i];
                const auto& memberType = desc.Types[member.Type];
                size_t sizeInBytes = 0;
                if (memberType.IsPrimitive()) {
                    sizeInBytes = memberType.GetSizeInBytes();
                } else {
                    auto rOffset = i == type.Members.size() - 1 ? ctx.s : desc.Types[type.Members[i + 1].Type].Offset;
                    sizeInBytes = rOffset - memberType.Offset;
                    if (memberType.Elements > 0) {
                        sizeInBytes /= memberType.Elements;
                    }
                }
                auto childBdIdx = builder.AddType(memberType.Name, sizeInBytes);
                if (memberType.Elements == 0) {
                    builder.AddMemberForType(ctx.bd, childBdIdx, member.Name, memberType.Offset);
                } else {
                    builder.AddMemberForType(ctx.bd, childBdIdx, member.Name, memberType.Offset, memberType.Elements);
                }
                s.push({member.Type, childBdIdx, sizeInBytes});
            }
        }
    };
    for (const auto& res : desc.BoundResources) {
        if (res.Type != HlslShaderInputType::CBUFFER) {
            continue;
        }
        auto cbOpt = desc.FindCBufferByName(res.Name);
        if (!cbOpt.has_value()) {
            RADRAY_ERR_LOG("{} {}", "cannot find cbuffer", res.Name);
            return std::nullopt;
        }
        const auto& cb = cbOpt.value().get();
        if (cb.IsViewInHlsl) {
            RADRAY_ASSERT(cb.Variables.size() == 1);
            size_t varIdx = cb.Variables[0];
            const auto& var = desc.Variables[varIdx];
            const auto& type = desc.Types[var.Type];
            size_t sizeInBytes = cb.Size;
            StructuredBufferId bdTypeIdx = builder.AddType(type.Name, sizeInBytes);
            if (res.BindCount > 1) {
                builder.AddRoot(var.Name, bdTypeIdx, res.BindCount);
            } else {
                builder.AddRoot(var.Name, bdTypeIdx);
            }
            createType(var.Type, bdTypeIdx, sizeInBytes);
        } else {
            for (size_t i = 0; i < cb.Variables.size(); i++) {
                size_t varIdx = cb.Variables[i];
                const auto& var = desc.Variables[varIdx];
                const auto& type = desc.Types[var.Type];
                size_t sizeInBytes = 0;
                if (i == cb.Variables.size() - 1) {
                    sizeInBytes = cb.Size - var.StartOffset;
                } else {
                    sizeInBytes = desc.Variables[cb.Variables[i + 1]].StartOffset - var.StartOffset;
                }
                if (type.Elements > 0) {
                    sizeInBytes /= type.Elements;
                }
                StructuredBufferId bdTypeIdx = builder.AddType(type.Name, sizeInBytes);
                if (type.Elements == 0) {
                    builder.AddRoot(var.Name, bdTypeIdx);
                } else {
                    builder.AddRoot(var.Name, bdTypeIdx, type.Elements);
                }
                createType(var.Type, bdTypeIdx, sizeInBytes);
            }
        }
    }
    return builder.Build();
}

std::optional<StructuredBufferStorage> CreateCBufferStorage(const SpirvShaderDesc& desc) noexcept {
    StructuredBufferStorage::Builder builder{};
    builder.SetAlignment(0);
    auto createType = [&](size_t parent, StructuredBufferId bdType, size_t size) {
        struct TypeCreateCtx {
            size_t par;
            StructuredBufferId bd;
            size_t s;
        };
        stack<TypeCreateCtx> s;
        s.push({parent, bdType, size});
        while (!s.empty()) {
            auto ctx = s.top();
            s.pop();
            const auto& type = desc.Types[ctx.par];
            for (size_t i = 0; i < type.Members.size(); i++) {
                const auto& member = type.Members[i];
                const auto& memberType = desc.Types[member.TypeIndex];
                size_t sizeInBytes = member.Size;
                if (memberType.ArraySize > 0) {
                    sizeInBytes /= memberType.ArraySize;
                }
                auto childBdIdx = builder.AddType(memberType.Name, sizeInBytes);
                if (memberType.ArraySize == 0) {
                    builder.AddMemberForType(ctx.bd, childBdIdx, member.Name, member.Offset);
                } else {
                    builder.AddMemberForType(ctx.bd, childBdIdx, member.Name, member.Offset, memberType.ArraySize);
                }
                s.push({member.TypeIndex, childBdIdx, sizeInBytes});
            }
        }
    };

    for (const auto& res : desc.PushConstants) {
        RADRAY_ASSERT(res.TypeIndex < desc.Types.size());
        auto type = desc.Types[res.TypeIndex];
        StructuredBufferId bdTypeIdx = builder.AddType(type.Name, res.Size);
        builder.AddRoot(res.Name, bdTypeIdx);
        createType(res.TypeIndex, bdTypeIdx, res.Size);
    }
    for (const auto& res : desc.ResourceBindings) {
        if (res.Kind != SpirvResourceKind::UniformBuffer) {
            continue;
        }
        RADRAY_ASSERT(res.TypeIndex < desc.Types.size());
        auto type = desc.Types[res.TypeIndex];
        if (res.IsViewInHlsl) {
            size_t sizeInBytes = res.UniformBufferSize;
            if (res.ArraySize > 0) {
                sizeInBytes /= res.ArraySize;
            }
            StructuredBufferId bdTypeIdx = builder.AddType(type.Name, sizeInBytes);
            if (res.ArraySize == 0) {
                builder.AddRoot(res.Name, bdTypeIdx);
            } else {
                builder.AddRoot(res.Name, bdTypeIdx, res.ArraySize);
            }
            createType(res.TypeIndex, bdTypeIdx, sizeInBytes);
        } else {
            for (size_t i = 0; i < type.Members.size(); i++) {
                const auto& member = type.Members[i];
                const auto& memberType = desc.Types[member.TypeIndex];
                size_t sizeInBytes;
                if (i == type.Members.size() - 1) {
                    sizeInBytes = res.UniformBufferSize - member.Offset;
                } else {
                    sizeInBytes = type.Members[i + 1].Offset - member.Offset;
                }
                if (memberType.ArraySize > 0) {
                    sizeInBytes /= memberType.ArraySize;
                }
                StructuredBufferId bdTypeIdx = builder.AddType(memberType.Name, sizeInBytes);
                if (memberType.ArraySize == 0) {
                    builder.AddRoot(member.Name, bdTypeIdx);
                } else {
                    builder.AddRoot(member.Name, bdTypeIdx, memberType.ArraySize);
                }
                createType(member.TypeIndex, bdTypeIdx, sizeInBytes);
            }
        }
    }
    return builder.Build();
}

CBufferStorage::CBufferStorage(const HlslShaderDesc& hlslDesc, const RootSignatureDescriptor& rsDesc, const StructuredBufferStorage& storage) {
    if (rsDesc.Constant) {
        auto& c = rsDesc.Constant.value();
        auto& cDesc = *std::find_if(hlslDesc.BoundResources.begin(), hlslDesc.BoundResources.end(), [&](const auto& cb) {
            return cb.Type == HlslShaderInputType::CBUFFER && cb.BindPoint == c.Slot && cb.Space == c.Space;
        });
        _constantId = storage.GetVar(cDesc.Name).GetId();
    }
}

void CBufferStorage::Bind(CommandEncoder* encode, CBufferArena* arena) {
    // TODO:
}

RootSignatureDescriptorContainer::RootSignatureDescriptorContainer(const RootSignatureDescriptor& desc) noexcept
    : _constant(desc.Constant) {
    _rootDescriptors.clear();
    _rootDescriptors.reserve(desc.RootDescriptors.size());
    for (const auto& rd : desc.RootDescriptors) {
        _rootDescriptors.emplace_back(rd);
    }
    size_t totalElements = 0;
    size_t totalSamplers = 0;
    for (const auto& set : desc.DescriptorSets) {
        totalElements += set.Elements.size();
        for (const auto& elem : set.Elements) {
            totalSamplers += elem.StaticSamplers.size();
        }
    }
    _elements.clear();
    _elements.reserve(totalElements);
    _staticSamplers.clear();
    _staticSamplers.reserve(totalSamplers);
    _descriptorSets.clear();
    _descriptorSets.reserve(desc.DescriptorSets.size());
    for (const auto& set : desc.DescriptorSets) {
        auto& setData = _descriptorSets.emplace_back();
        setData.ElementOffset = _elements.size();
        setData.ElementCount = set.Elements.size();
        for (const auto& elem : set.Elements) {
            auto& out = _elements.emplace_back();
            out.Slot = elem.Slot;
            out.Space = elem.Space;
            out.Type = elem.Type;
            out.Count = elem.Count;
            out.Stages = elem.Stages;
            out.StaticSamplerOffset = static_cast<uint32_t>(_staticSamplers.size());
            out.StaticSamplerCount = static_cast<uint32_t>(elem.StaticSamplers.size());
            for (const auto& s : elem.StaticSamplers) {
                _staticSamplers.emplace_back(s);
            }
        }
    }
}

RootSignatureDescriptorContainer::View RootSignatureDescriptorContainer::MakeView() const noexcept {
    View view{};
    view._rootDescriptors.clear();
    view._rootDescriptors.reserve(_rootDescriptors.size());
    for (const auto& rd : _rootDescriptors) {
        view._rootDescriptors.emplace_back(rd);
    }
    view._staticSamplers.clear();
    view._staticSamplers.reserve(_staticSamplers.size());
    for (const auto& s : _staticSamplers) {
        view._staticSamplers.emplace_back(s);
    }
    view._elements.clear();
    view._elements.reserve(_elements.size());
    for (const auto& e : _elements) {
        auto& out = view._elements.emplace_back();
        out.Slot = e.Slot;
        out.Space = e.Space;
        out.Type = e.Type;
        out.Count = e.Count;
        out.Stages = e.Stages;
        if (e.StaticSamplerCount == 0) {
            out.StaticSamplers = {};
        } else {
            RADRAY_ASSERT(static_cast<size_t>(e.StaticSamplerOffset) + static_cast<size_t>(e.StaticSamplerCount) <= view._staticSamplers.size());
            out.StaticSamplers = std::span{view._staticSamplers.data() + e.StaticSamplerOffset, e.StaticSamplerCount};
        }
    }
    view._descriptorSets.clear();
    view._descriptorSets.reserve(_descriptorSets.size());
    for (const auto& s : _descriptorSets) {
        auto& out = view._descriptorSets.emplace_back();
        if (s.ElementCount == 0) {
            out.Elements = {};
        } else {
            RADRAY_ASSERT(s.ElementOffset + s.ElementCount <= view._elements.size());
            out.Elements = std::span{view._elements.data() + s.ElementOffset, s.ElementCount};
        }
    }
    view._desc.RootDescriptors = std::span{view._rootDescriptors};
    view._desc.DescriptorSets = std::span{view._descriptorSets};
    view._desc.Constant = _constant;
    return view;
}

Nullable<unique_ptr<RootSignature>> CreateSerializedRootSignature(Device* device_, std::span<const byte> data) noexcept {
#ifdef RADRAY_ENABLE_D3D12
    if (device_->GetBackend() != RenderBackend::D3D12) {
        RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidOperation, "device");
        return nullptr;
    }
    auto device = d3d12::CastD3D12Object(device_);
    d3d12::ComPtr<ID3D12RootSignature> rootSig;
    if (HRESULT hr = device->_device->CreateRootSignature(0, data.data(), data.size(), IID_PPV_ARGS(&rootSig));
        FAILED(hr)) {
        RADRAY_ERR_LOG("{} {}::{} {}", Errors::D3D12, "ID3D12Device", "CreateRootSignature", hr);
        return nullptr;
    }
    d3d12::ComPtr<ID3D12VersionedRootSignatureDeserializer> deserializer;
    if (HRESULT hr = ::D3D12CreateVersionedRootSignatureDeserializer(data.data(), data.size(), IID_PPV_ARGS(&deserializer));
        FAILED(hr)) {
        RADRAY_ERR_LOG("{} {}::{} {}", Errors::D3D12, "D3D12CreateVersionedRootSignatureDeserializer", d3d12::GetErrorName(hr), hr);
        return nullptr;
    }
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc;
    if (HRESULT hr = deserializer->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &desc);
        FAILED(hr)) {
        RADRAY_ERR_LOG("{} {}::{} {}", Errors::D3D12, "ID3D12VersionedRootSignatureDeserializer", "GetRootSignatureDescAtVersion", hr);
        return nullptr;
    }
    if (desc->Version != D3D_ROOT_SIGNATURE_VERSION_1_1) {
        RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, "unknown version", desc->Version);
        return nullptr;
    }
    auto result = make_unique<d3d12::RootSigD3D12>(device, std::move(rootSig));
    result->_desc = d3d12::VersionedRootSignatureDescContainer{*desc};
    return result;
#else
    RADRAY_ERR_LOG("only d3d12 backend supports serialized root signature");
    return nullptr;
#endif
}

std::optional<RootSignatureDescriptorContainer> CreateRootSignatureDescriptor(const HlslShaderDesc& desc) noexcept {
    constexpr uint32_t maxRootDWORD = 64;
    constexpr uint32_t maxRootBYTE = maxRootDWORD * 4;

    enum class HlslRSPlacement {
        Table,
        RootDescriptor,
        RootConstant,
    };

    if (desc.BoundResources.empty()) {
        RootSignatureDescriptor empty{};
        return RootSignatureDescriptorContainer{empty};
    }
    vector<HlslRSPlacement> placements{desc.BoundResources.size(), HlslRSPlacement::Table};
    auto cmpResource = [&](size_t lhs, size_t rhs) noexcept {
        const auto& l = desc.BoundResources[lhs];
        const auto& r = desc.BoundResources[rhs];
        return l.BindPoint < r.BindPoint;
    };
    auto buildTable = [&]() {
        vector<size_t> asTable;
        for (size_t i = 0; i < desc.BoundResources.size(); i++) {
            if (placements[i] != HlslRSPlacement::Table) {
                continue;
            }
            asTable.push_back(i);
        }
        vector<size_t> resourceIndices, samplerIndices;
        for (size_t i : asTable) {
            const auto& binding = desc.BoundResources[i];
            ResourceBindType type = binding.MapResourceBindType();
            if (type == ResourceBindType::Sampler) {
                samplerIndices.push_back(i);
            } else {
                resourceIndices.push_back(i);
            }
        }
        unordered_map<UINT, vector<size_t>> resourceSpace, samplerSpace;
        for (size_t i : resourceIndices) {
            resourceSpace[desc.BoundResources[i].Space].push_back(i);
        }
        for (size_t i : samplerIndices) {
            samplerSpace[desc.BoundResources[i].Space].push_back(i);
        }
        vector<vector<RootSignatureSetElement>> descriptors;
        auto buildDescriptors = [&](const decltype(resourceSpace)& splits) noexcept {
            for (auto [space, indices] : splits) {
                auto& elements = descriptors.emplace_back(vector<RootSignatureSetElement>{});
                elements.reserve(indices.size());
                std::sort(indices.begin(), indices.end(), cmpResource);
                for (size_t i : indices) {
                    const auto& binding = desc.BoundResources[i];
                    RootSignatureSetElement elem{};
                    elem.Slot = binding.BindPoint;
                    elem.Space = binding.Space;
                    elem.Type = binding.MapResourceBindType();
                    elem.Count = binding.BindCount;
                    elem.Stages = binding.Stages;
                    elem.StaticSamplers = {};
                    elements.push_back(elem);
                }
            }
        };
        buildDescriptors(resourceSpace);
        buildDescriptors(samplerSpace);
        return descriptors;
    };

    std::optional<RootSignatureConstant> rootConstant;
    size_t bestRootConstIndex = std::numeric_limits<size_t>::max();
    for (size_t i = 0; i < desc.BoundResources.size(); i++) {
        const auto& binding = desc.BoundResources[i];
        if (binding.Type == HlslShaderInputType::CBUFFER && binding.BindCount == 1) {
            auto cbufferDataOpt = desc.FindCBufferByName(binding.Name);
            if (!cbufferDataOpt.has_value()) {
                RADRAY_ERR_LOG("{} {}", "CreateRootSignatureDescriptor", "cannot find cbuffer data");
                return std::nullopt;
            }
            const auto& cbufferData = cbufferDataOpt.value().get();
            if (cbufferData.Size > maxRootBYTE) {
                continue;
            }
            UINT bestBindPoint = std::numeric_limits<UINT>::max(), bestSpace = std::numeric_limits<UINT>::max();
            if (bestRootConstIndex != std::numeric_limits<size_t>::max()) {
                const auto& bestBinding = desc.BoundResources[bestRootConstIndex];
                bestBindPoint = bestBinding.BindPoint;
                bestSpace = bestBinding.Space;
            }
            if (binding.Space < bestSpace || (binding.Space == bestSpace && binding.BindPoint < bestBindPoint)) {
                bestRootConstIndex = i;
            }
        }
    }
    if (bestRootConstIndex != std::numeric_limits<size_t>::max()) {
        const auto& binding = desc.BoundResources[bestRootConstIndex];
        auto cbufferDataOpt = desc.FindCBufferByName(binding.Name);
        RADRAY_ASSERT(cbufferDataOpt.has_value());
        const auto& cbufferData = cbufferDataOpt.value().get();
        RootSignatureConstant constant{};
        constant.Slot = binding.BindPoint;
        constant.Space = binding.Space;
        constant.Size = cbufferData.Size;
        constant.Stages = desc.Stages;
        rootConstant = constant;
        placements[bestRootConstIndex] = HlslRSPlacement::RootConstant;
    }
    vector<size_t> asRootDesc;
    for (size_t i = 0; i < desc.BoundResources.size(); i++) {
        const auto& binding = desc.BoundResources[i];
        if (placements[i] != HlslRSPlacement::Table) {
            continue;
        }
        if (binding.BindCount != 1) {
            continue;
        }
        ResourceBindType type = binding.MapResourceBindType();
        if (type == ResourceBindType::CBuffer ||
            type == ResourceBindType::Buffer ||
            type == ResourceBindType::RWBuffer) {
            asRootDesc.push_back(i);
            placements[i] = HlslRSPlacement::RootDescriptor;
        }
    }
    vector<vector<RootSignatureSetElement>> tables;
    while (true) {
        std::sort(asRootDesc.begin(), asRootDesc.end(), cmpResource);
        tables = buildTable();
        size_t totalDWORD = 0;
        if (rootConstant.has_value()) {
            totalDWORD += Align(rootConstant->Size, 4) / 4;
        }
        totalDWORD += asRootDesc.size() * 2;
        totalDWORD += tables.size();
        if (totalDWORD <= maxRootDWORD) {
            break;
        }
        if (rootConstant.has_value()) {
            rootConstant.reset();
            asRootDesc.push_back(bestRootConstIndex);
            placements[bestRootConstIndex] = HlslRSPlacement::RootDescriptor;
            continue;
        }
        if (!asRootDesc.empty()) {
            size_t rmIndex = asRootDesc.back();
            placements[rmIndex] = HlslRSPlacement::Table;
            asRootDesc.pop_back();
            continue;
        }
        RADRAY_ERR_LOG("{} {}", "CreateRootSignatureDescriptor", "cannot fit into root signature limits");
        return std::nullopt;
    }
    vector<RootSignatureRootDescriptor> rootDescs;
    for (size_t i : asRootDesc) {
        const auto& binding = desc.BoundResources[i];
        RootSignatureRootDescriptor rootDesc{};
        rootDesc.Slot = binding.BindPoint;
        rootDesc.Space = binding.Space;
        rootDesc.Type = binding.MapResourceBindType();
        rootDesc.Stages = binding.Stages;
        rootDescs.push_back(rootDesc);
    }
    vector<RootSignatureDescriptorSet> descriptorSets;
    for (const auto& table : tables) {
        RootSignatureDescriptorSet set{};
        set.Elements = std::span{table};
        descriptorSets.push_back(set);
    }
    RootSignatureDescriptor rsDesc{};
    rsDesc.RootDescriptors = rootDescs;
    rsDesc.DescriptorSets = descriptorSets;
    rsDesc.Constant = rootConstant;
    return RootSignatureDescriptorContainer{rsDesc};
}

std::optional<RootSignatureDescriptorContainer> CreateRootSignatureDescriptor(const SpirvShaderDesc& desc) noexcept {
    unordered_map<uint32_t, vector<RootSignatureSetElement>> sets;
    for (const auto& binding : desc.ResourceBindings) {
        auto& elements = sets[binding.Set];
        RootSignatureSetElement& elem = elements.emplace_back();
        elem.Slot = binding.Binding;
        elem.Space = binding.Set;
        elem.Count = binding.ArraySize == 0 ? 1 : binding.ArraySize;
        elem.Stages = binding.Stages;
        switch (binding.Kind) {
            case SpirvResourceKind::UniformBuffer:
                elem.Type = ResourceBindType::CBuffer;
                break;
            case SpirvResourceKind::StorageBuffer:
                elem.Type = (binding.ReadOnly && !binding.WriteOnly) ? ResourceBindType::Buffer : ResourceBindType::RWBuffer;
                break;
            case SpirvResourceKind::SampledImage:
            case SpirvResourceKind::SeparateImage:
                elem.Type = ResourceBindType::Texture;
                break;
            case SpirvResourceKind::SeparateSampler:
                elem.Type = ResourceBindType::Sampler;
                break;
            case SpirvResourceKind::StorageImage:
                elem.Type = ResourceBindType::RWTexture;
                break;
            case SpirvResourceKind::AccelerationStructure:
                elem.Type = ResourceBindType::Buffer;
                break;
            default:
                elem.Type = ResourceBindType::UNKNOWN;
                break;
        }
    }
    vector<uint32_t> setIndices;
    setIndices.reserve(sets.size());
    for (const auto& [k, v] : sets) {
        setIndices.emplace_back(k);
    }
    std::sort(setIndices.begin(), setIndices.end());
    vector<RootSignatureDescriptorSet> descriptorSets;
    descriptorSets.reserve(sets.size());
    for (uint32_t setIdx : setIndices) {
        auto& elements = sets[setIdx];
        std::sort(elements.begin(), elements.end(), [](const RootSignatureSetElement& a, const RootSignatureSetElement& b) {
            return a.Slot < b.Slot;
        });
        RootSignatureDescriptorSet& setDesc = descriptorSets.emplace_back();
        setDesc.Elements = elements;
    }
    std::optional<RootSignatureConstant> constant;
    if (!desc.PushConstants.empty()) {
        uint32_t size = 0;
        ShaderStages stages = ShaderStage::UNKNOWN;
        for (const auto& pc : desc.PushConstants) {
            size = std::max(size, pc.Offset + pc.Size);
            stages |= pc.Stages;
        }
        if (size > 0) {
            RootSignatureConstant c{};
            c.Size = size;
            c.Stages = stages;
            constant = c;
        }
    }
    RootSignatureDescriptor rsDesc{};
    rsDesc.DescriptorSets = descriptorSets;
    rsDesc.Constant = constant;
    return RootSignatureDescriptorContainer{rsDesc};
}

}  // namespace radray::render
