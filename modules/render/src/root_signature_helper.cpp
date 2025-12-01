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
    _descriptorSets.resize(desc.DescriptorSets.size());
    for (const auto& i : desc.DescriptorSets) {
        _elements.insert(_elements.end(), i.Elements.begin(), i.Elements.end());
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

static uint32_t _StageUsageScore(ShaderStages stages) noexcept {
    uint32_t score = 0;
    if (stages.HasFlag(ShaderStage::Vertex)) {
        ++score;
    }
    if (stages.HasFlag(ShaderStage::Pixel)) {
        ++score;
    }
    if (stages.HasFlag(ShaderStage::Compute)) {
        ++score;
    }
    return score;
}

static uint32_t _BindTypePriority(ResourceBindType type) noexcept {
    switch (type) {
        case ResourceBindType::CBuffer: return 0;
        case ResourceBindType::Buffer: return 1;
        case ResourceBindType::RWBuffer: return 2;
        case ResourceBindType::Texture: return 3;
        case ResourceBindType::RWTexture: return 4;
        case ResourceBindType::Sampler: return 5;
        default: return 6;
    }
}

vector<HlslRSCombinedBinding> MergeHlslShaderBoundResources(std::span<const HlslShaderDesc*> descs) noexcept {
    size_t totalBindings = 0;
    for (const auto& desc : descs) {
        totalBindings += desc->BoundResources.size();
    }
    vector<HlslRSCombinedBinding> bindings;
    bindings.reserve(totalBindings);
    for (const auto& descPtr : descs) {
        const auto& desc = *descPtr;
        for (const auto& bind : desc.BoundResources) {
            if (bind.BindCount == 0 || bind.BindCount == std::numeric_limits<uint32_t>::max()) {
                RADRAY_ERR_LOG("{} {} {} {}", Errors::D3D12, Errors::InvalidOperation, "unsupported bind count", bind.Name);
                continue;
            }
            ResourceBindType type = bind.MapResourceBindType();
            if (type == ResourceBindType::UNKNOWN) {
                RADRAY_ERR_LOG("{} {} {} {}", Errors::D3D12, Errors::InvalidOperation, "unsupported resource bind type", bind.Name);
                continue;
            }
            auto it = std::find_if(bindings.begin(), bindings.end(), [&](const HlslRSCombinedBinding& existing) {
                return existing.Type == type && existing.Space == bind.Space && existing.Slot == bind.BindPoint;
            });
            if (it == bindings.end()) {
                HlslRSCombinedBinding combined{};
                combined.Type = type;
                combined.Slot = bind.BindPoint;
                combined.Space = bind.Space;
                combined.Count = bind.BindCount;
                combined.Stages = desc.Stage;
                combined.Layout = &bind;
                combined.Name = bind.Name;
                if (type == ResourceBindType::CBuffer) {
                    combined.CBuffer = desc.FindCBufferByName(bind.Name).Release();
                    if (!combined.CBuffer) {
                        RADRAY_ERR_LOG("{} {} {} {}", Errors::D3D12, Errors::InvalidArgument, "missing cbuffer desc", bind.Name);
                        return {};
                    }
                }
                bindings.emplace_back(std::move(combined));
            } else {
                if (*it->Layout != bind) {
                    RADRAY_ERR_LOG("{} {} {} {}", Errors::D3D12, Errors::InvalidOperation, "mismatched resource layout", bind.Name);
                    return {};
                }
                if (type == ResourceBindType::CBuffer) {
                    const auto* rhsCBuffer = desc.FindCBufferByName(bind.Name).Release();
                    if (!rhsCBuffer) {
                        RADRAY_ERR_LOG("{} {} {} {}", Errors::D3D12, Errors::InvalidArgument, "missing cbuffer desc", bind.Name);
                        return {};
                    }
                    if (*(it->CBuffer) != *(rhsCBuffer)) {
                        RADRAY_ERR_LOG("{} {} {} {}", Errors::D3D12, Errors::InvalidOperation, "mismatched cbuffer layout", bind.Name);
                        return {};
                    }
                }
            }
            it->Stages |= desc.Stage;
        }
    }
    return bindings;
}

// static bool _AddOrMergeHlslBinding(
//     vector<HlslRSCombinedBinding>& bindings,
//     const HlslShaderDesc& shaderDesc,
//     const HlslInputBindDesc& bind,
//     ShaderStages stageMask) noexcept {

// }

enum class HlslRSPlacement {
    Table,
    RootDescriptor,
    RootConstant,
};

struct HlslRSRangeSpec {
    ResourceBindType Type{ResourceBindType::UNKNOWN};
    uint32_t Space{0};
    uint32_t Slot{0};
    uint32_t Count{0};
    ShaderStages Stages{ShaderStage::UNKNOWN};
};

struct HlslRSDescriptorTableBuildResult {
    vector<RootSignatureSetElement> Elements;
    vector<RootSignatureDescriptorSet> Sets;
};

static std::optional<HlslRSDescriptorTableBuildResult> _BuildHlslDescriptorSets(
    const vector<HlslRSCombinedBinding>& bindings,
    const vector<HlslRSPlacement>& placements) noexcept {
    HlslRSDescriptorTableBuildResult result{};
    vector<std::pair<size_t, size_t>> setRanges;
    map<uint32_t, vector<size_t>> perSpace;
    for (size_t i = 0; i < bindings.size(); ++i) {
        if (placements[i] != HlslRSPlacement::Table) {
            continue;
        }
        perSpace[bindings[i].Space].push_back(i);
    }

    for (auto& [space, indices] : perSpace) {
        auto& idxs = indices;
        std::sort(idxs.begin(), idxs.end(), [&](size_t lhs, size_t rhs) {
            const auto& l = bindings[lhs];
            const auto& r = bindings[rhs];
            uint32_t lp = _BindTypePriority(l.Type);
            uint32_t rp = _BindTypePriority(r.Type);
            if (lp != rp) {
                return lp < rp;
            }
            if (l.Slot != r.Slot) {
                return l.Slot < r.Slot;
            }
            return l.Name < r.Name;
        });

        vector<HlslRSRangeSpec> ranges;
        HlslRSRangeSpec current{};
        bool hasCurrent = false;
        auto flush = [&]() {
            if (hasCurrent) {
                ranges.push_back(current);
                hasCurrent = false;
            }
        };

        for (size_t idx : idxs) {
            const auto& binding = bindings[idx];
            if (!hasCurrent) {
                current = {binding.Type, space, binding.Slot, binding.Count, binding.Stages};
                hasCurrent = true;
                continue;
            }
            if (binding.Type == current.Type) {
                uint64_t expected = static_cast<uint64_t>(current.Slot) + current.Count;
                if (binding.Slot < expected) {
                    RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidOperation, "overlap in resource registers");
                    return std::nullopt;
                }
                if (binding.Slot == expected) {
                    if (binding.Count > std::numeric_limits<uint32_t>::max() - current.Count) {
                        RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidOperation, "descriptor range overflow");
                        return std::nullopt;
                    }
                    current.Count += binding.Count;
                    current.Stages |= binding.Stages;
                    continue;
                }
            }
            flush();
            current = {binding.Type, space, binding.Slot, binding.Count, binding.Stages};
            hasCurrent = true;
        }
        flush();

        size_t start = result.Elements.size();
        for (const auto& range : ranges) {
            RootSignatureSetElement elem{};
            elem.Slot = range.Slot;
            elem.Space = range.Space;
            elem.Type = range.Type;
            elem.Count = range.Count;
            elem.Stages = range.Stages;
            elem.StaticSamplers = std::span<const SamplerDescriptor>{};
            result.Elements.push_back(elem);
        }

        size_t count = result.Elements.size() - start;
        RootSignatureDescriptorSet set{};
        result.Sets.push_back(set);
        setRanges.emplace_back(start, count);
    }

    for (size_t i = 0; i < result.Sets.size(); ++i) {
        auto [start, count] = setRanges[i];
        result.Sets[i].Elements = std::span<const RootSignatureSetElement>{
            result.Elements.data() + start,
            count};
    }

    return result;
}

std::optional<RootSignatureDescriptorContainer> CreateRootSignatureDescriptor(std::span<const HlslShaderDesc*> descs) noexcept {
    if (descs.empty()) {
        RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidArgument, "descs");
        return std::nullopt;
    }

    vector<HlslRSCombinedBinding> bindings = MergeHlslShaderBoundResources(descs);

    if (bindings.empty()) {
        RootSignatureDescriptor desc{};
        return RootSignatureDescriptorContainer{desc};
    }

    constexpr uint32_t maxRootDWORD = 64;
    vector<HlslRSPlacement> placements(bindings.size(), HlslRSPlacement::Table);

    auto pickRootConstant = [&]() -> std::optional<size_t> {
        std::optional<size_t> best;
        for (size_t i = 0; i < bindings.size(); ++i) {
            const auto& binding = bindings[i];
            if (binding.Type != ResourceBindType::CBuffer || binding.Count != 1 || binding.CBuffer == nullptr) {
                continue;
            }
            if (binding.CBuffer->Size == 0 || binding.CBuffer->Size % 4 != 0 || binding.CBuffer->Size > maxRootDWORD * 4) {
                continue;
            }
            if (!best.has_value()) {
                best = i;
                continue;
            }
            const auto& currentBest = bindings[best.value()];
            if (binding.Space < currentBest.Space || (binding.Space == currentBest.Space && binding.Slot < currentBest.Slot)) {
                best = i;
            }
        }
        return best;
    };

    std::optional<RootSignatureConstant> rootConstant;
    std::optional<size_t> rootConstantBindingIndex;
    uint32_t rootConstDwords = 0;
    if (auto rootConstIndex = pickRootConstant(); rootConstIndex.has_value()) {
        const auto& binding = bindings[rootConstIndex.value()];
        RootSignatureConstant constant{};
        constant.Slot = binding.Slot;
        constant.Space = binding.Space;
        constant.Size = binding.CBuffer->Size;
        constant.Stages = binding.Stages;
        rootConstDwords = constant.Size / 4;
        placements[rootConstIndex.value()] = HlslRSPlacement::RootConstant;
        rootConstant = constant;
        rootConstantBindingIndex = rootConstIndex;
    }

    auto isRootDescriptorCandidate = [&](const HlslRSCombinedBinding& binding) noexcept {
        if (binding.Count != 1) {
            return false;
        }
        switch (binding.Type) {
            case ResourceBindType::CBuffer: return binding.CBuffer != nullptr;
            case ResourceBindType::Buffer:
            case ResourceBindType::RWBuffer: return true;
            default: return false;
        }
    };

    vector<size_t> rootDescCandidates;
    rootDescCandidates.reserve(bindings.size());
    for (size_t i = 0; i < bindings.size(); ++i) {
        if (placements[i] != HlslRSPlacement::Table) {
            continue;
        }
        if (isRootDescriptorCandidate(bindings[i])) {
            rootDescCandidates.push_back(i);
        }
    }

    std::sort(rootDescCandidates.begin(), rootDescCandidates.end(), [&](size_t lhs, size_t rhs) {
        const auto& l = bindings[lhs];
        const auto& r = bindings[rhs];
        uint32_t lp = _BindTypePriority(l.Type);
        uint32_t rp = _BindTypePriority(r.Type);
        if (lp != rp) {
            return lp < rp;
        }
        uint32_t ls = _StageUsageScore(l.Stages);
        uint32_t rs = _StageUsageScore(r.Stages);
        if (ls != rs) {
            return ls > rs;
        }
        if (l.Space != r.Space) {
            return l.Space < r.Space;
        }
        if (l.Slot != r.Slot) {
            return l.Slot < r.Slot;
        }
        return l.Name < r.Name;
    });

    vector<size_t> selectedRootDescs;
    selectedRootDescs.reserve(rootDescCandidates.size());
    for (size_t idx : rootDescCandidates) {
        placements[idx] = HlslRSPlacement::RootDescriptor;
        selectedRootDescs.push_back(idx);
    }

    auto buildTables = [&](HlslRSDescriptorTableBuildResult& out) -> bool {
        auto tables = _BuildHlslDescriptorSets(bindings, placements);
        if (!tables.has_value()) {
            return false;
        }
        out = std::move(tables.value());
        return true;
    };

    HlslRSDescriptorTableBuildResult tableResult{};
    if (!buildTables(tableResult)) {
        return std::nullopt;
    }

    auto calcTotalCost = [&](uint32_t rootDescCount, uint32_t tableCount) noexcept {
        return rootConstDwords + rootDescCount * 2u + tableCount;
    };

    uint32_t totalCost = calcTotalCost(static_cast<uint32_t>(selectedRootDescs.size()), static_cast<uint32_t>(tableResult.Sets.size()));
    auto reduceRootDescriptorUsage = [&]() -> bool {
        while (totalCost > maxRootDWORD && !selectedRootDescs.empty()) {
            size_t idx = selectedRootDescs.back();
            selectedRootDescs.pop_back();
            placements[idx] = HlslRSPlacement::Table;
            if (!buildTables(tableResult)) {
                return false;
            }
            totalCost = calcTotalCost(static_cast<uint32_t>(selectedRootDescs.size()), static_cast<uint32_t>(tableResult.Sets.size()));
        }
        return true;
    };

    if (!reduceRootDescriptorUsage()) {
        return std::nullopt;
    }

    if (totalCost > maxRootDWORD && rootConstant.has_value() && rootConstantBindingIndex.has_value()) {
        placements[rootConstantBindingIndex.value()] = HlslRSPlacement::Table;
        rootConstant.reset();
        rootConstantBindingIndex.reset();
        rootConstDwords = 0;
        if (!buildTables(tableResult)) {
            return std::nullopt;
        }
        totalCost = calcTotalCost(static_cast<uint32_t>(selectedRootDescs.size()), static_cast<uint32_t>(tableResult.Sets.size()));
        if (!reduceRootDescriptorUsage()) {
            return std::nullopt;
        }
    }

    if (totalCost > maxRootDWORD) {
        RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidOperation, "root signature exceeds limit");
        return std::nullopt;
    }

    if (!selectedRootDescs.empty()) {
        std::sort(selectedRootDescs.begin(), selectedRootDescs.end(), [&](size_t lhs, size_t rhs) {
            const auto& l = bindings[lhs];
            const auto& r = bindings[rhs];
            uint32_t lp = _BindTypePriority(l.Type);
            uint32_t rp = _BindTypePriority(r.Type);
            if (lp != rp) {
                return lp < rp;
            }
            if (l.Space != r.Space) {
                return l.Space < r.Space;
            }
            if (l.Slot != r.Slot) {
                return l.Slot < r.Slot;
            }
            return l.Name < r.Name;
        });
    }

    vector<RootSignatureRootDescriptor> rootDescriptors;
    rootDescriptors.reserve(selectedRootDescs.size());
    for (size_t idx : selectedRootDescs) {
        const auto& binding = bindings[idx];
        RootSignatureRootDescriptor rd{};
        rd.Slot = binding.Slot;
        rd.Space = binding.Space;
        rd.Type = binding.Type;
        rd.Stages = binding.Stages;
        rootDescriptors.push_back(rd);
    }

    RootSignatureDescriptor desc{};
    desc.RootDescriptors = std::span<const RootSignatureRootDescriptor>{rootDescriptors};
    desc.DescriptorSets = std::span<const RootSignatureDescriptorSet>{tableResult.Sets};
    if (rootConstant.has_value()) {
        desc.Constant = rootConstant;
    }

    return RootSignatureDescriptorContainer{desc};
}

}  // namespace radray::render
