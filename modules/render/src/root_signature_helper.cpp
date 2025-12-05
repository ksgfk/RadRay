#include <radray/render/root_signature_helper.h>

#ifdef RADRAY_ENABLE_D3D12
#include <radray/render/backend/d3d12_impl.h>
#endif

#ifdef RADRAY_ENABLE_VULKAN
#include <radray/render/backend/vulkan_impl.h>
#endif

#include <radray/errors.h>

namespace radray::render {

RootSignatureDescriptorContainer::RootSignatureDescriptorContainer(const RootSignatureDescriptor& desc) noexcept
    : _rootDescriptors{desc.RootDescriptors.begin(), desc.RootDescriptors.end()},
      _desc{desc} {
    size_t total = 0;
    for (const auto& i : desc.DescriptorSets) {
        total += i.Elements.size();
    }
    _elements.reserve(total);
    _descriptorSets.reserve(desc.DescriptorSets.size());
    for (const auto& i : desc.DescriptorSets) {
        _elements.insert(_elements.end(), i.Elements.begin(), i.Elements.end());
        _descriptorSets.emplace_back(std::span{_elements.data(), i.Elements.size()});
    }
    Refresh();
}

RootSignatureDescriptorContainer::RootSignatureDescriptorContainer(const RootSignatureDescriptorContainer& other) noexcept
    : _rootDescriptors{other._rootDescriptors},
      _elements{other._elements},
      _descriptorSets{other._descriptorSets},
      _desc{other._desc} {
    Refresh();
}

RootSignatureDescriptorContainer::RootSignatureDescriptorContainer(RootSignatureDescriptorContainer&& other) noexcept
    : _rootDescriptors(std::move(other._rootDescriptors)),
      _elements(std::move(other._elements)),
      _descriptorSets(std::move(other._descriptorSets)),
      _desc(other._desc) {
    Refresh();
    other._desc.Constant.reset();
    other.Refresh();
}

const RootSignatureDescriptor& RootSignatureDescriptorContainer::Get() const noexcept {
    return _desc;
}

RootSignatureDescriptorContainer& RootSignatureDescriptorContainer::operator=(const RootSignatureDescriptorContainer& other) noexcept {
    RootSignatureDescriptorContainer tmp{other};
    swap(*this, tmp);
    return *this;
}

RootSignatureDescriptorContainer& RootSignatureDescriptorContainer::operator=(RootSignatureDescriptorContainer&& other) noexcept {
    RootSignatureDescriptorContainer tmp{std::move(other)};
    swap(*this, tmp);
    return *this;
}

void RootSignatureDescriptorContainer::Refresh() noexcept {
    size_t offset = 0;
    for (auto& set : _descriptorSets) {
        const size_t count = set.Elements.size();
        set.Elements = std::span{_elements.data() + offset, count};
        offset += count;
    }
    _desc.RootDescriptors = std::span{_rootDescriptors};
    _desc.DescriptorSets = std::span{_descriptorSets};
}

void swap(RootSignatureDescriptorContainer& lhs, RootSignatureDescriptorContainer& rhs) noexcept {
    using std::swap;
    swap(lhs._rootDescriptors, rhs._rootDescriptors);
    swap(lhs._elements, rhs._elements);
    swap(lhs._descriptorSets, rhs._descriptorSets);
    swap(lhs._desc, rhs._desc);
    lhs.Refresh();
    rhs.Refresh();
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
    DynamicLibrary d3d12Dll{"d3d12"};
    if (!d3d12Dll.IsValid()) {
        return nullptr;
    }
    auto D3D12CreateVersionedRootSignatureDeserializer_F = d3d12Dll.GetFunction<PFN_D3D12_CREATE_ROOT_SIGNATURE_DESERIALIZER>("D3D12CreateVersionedRootSignatureDeserializer");
    if (!D3D12CreateVersionedRootSignatureDeserializer_F) {
        return nullptr;
    }
    d3d12::ComPtr<ID3D12VersionedRootSignatureDeserializer> deserializer;
    if (HRESULT hr = D3D12CreateVersionedRootSignatureDeserializer_F(data.data(), data.size(), IID_PPV_ARGS(&deserializer));
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

std::optional<RootSignatureDescriptorContainer> CreateRootSignatureDescriptor(std::span<const HlslShaderDesc*> descs) noexcept {
    constexpr uint32_t maxRootDWORD = 64;
    constexpr uint32_t maxRootBYTE = maxRootDWORD * 4;

    enum class HlslRSPlacement {
        Table,
        RootDescriptor,
        RootConstant,
    };

    MergedHlslShaderDesc desc = MergeHlslShaderDesc(descs);
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

}  // namespace radray::render
