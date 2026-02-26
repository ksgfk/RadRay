#include <radray/render/bind_bridge.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <type_traits>

#include <radray/basic_math.h>

namespace radray::render {

RootSignatureDescriptorContainer::RootSignatureDescriptorContainer(const RootSignatureDescriptor& desc) noexcept {
    _rootDescriptors.assign(desc.RootDescriptors.begin(), desc.RootDescriptors.end());
    _staticSamplers.assign(desc.StaticSamplers.begin(), desc.StaticSamplers.end());
    size_t totalElements = 0;
    for (const auto& set : desc.DescriptorSets) {
        totalElements += set.Elements.size();
    }
    _elements.reserve(totalElements);
    _descriptorSets.reserve(desc.DescriptorSets.size());
    for (const auto& set : desc.DescriptorSets) {
        size_t elemStart = _elements.size();
        size_t elemCount = set.Elements.size();
        _elements.insert(_elements.end(), set.Elements.begin(), set.Elements.end());
        RootSignatureDescriptorSet ownedSet{};
        ownedSet.SetIndex = set.SetIndex;
        ownedSet.Elements = std::span<const RootSignatureSetElement>{
            _elements.data() + elemStart,
            elemCount};
        _descriptorSets.push_back(ownedSet);
    }
    _desc.RootDescriptors = _rootDescriptors;
    _desc.DescriptorSets = _descriptorSets;
    _desc.StaticSamplers = _staticSamplers;
    _desc.Constant = desc.Constant;
}

BindBridgeLayout::BindBridgeLayout(const HlslShaderDesc& desc, std::span<const BindBridgeStaticSampler> staticSamplers) noexcept {
    auto irOpt = this->BuildIRFromHlsl(desc);
    if (irOpt) {
        D3D12BindBridgeLayoutPlanner planner{};
        auto planned = this->PlanLayout(irOpt.value(), planner, staticSamplers);
        if (planned) {
            this->SetPlannedLayout(std::move(planned.value()));
        }
        _cbStorageBuilder = this->CreateCBufferStorageBuilder(desc).value_or(StructuredBufferStorage::Builder{});
    }
}

BindBridgeLayout::BindBridgeLayout(const SpirvShaderDesc& desc, std::span<const BindBridgeStaticSampler> staticSamplers) noexcept {
    auto irOpt = this->BuildIRFromSpirv(desc);
    if (irOpt) {
        VulkanBindBridgeLayoutPlanner planner{};
        auto planned = this->PlanLayout(irOpt.value(), planner, staticSamplers);
        if (planned) {
            this->SetPlannedLayout(std::move(planned.value()));
        }
        _cbStorageBuilder = this->CreateCBufferStorageBuilder(desc).value_or(StructuredBufferStorage::Builder{});
    }
}

BindBridgeLayout::BindBridgeLayout(const MslShaderReflection& desc, std::span<const BindBridgeStaticSampler> staticSamplers) noexcept {
    auto irOpt = this->BuildIRFromMsl(desc);
    if (irOpt) {
        MetalBindBridgeLayoutPlanner planner{};
        auto planned = this->PlanLayout(irOpt.value(), planner, staticSamplers);
        if (planned) {
            this->SetPlannedLayout(std::move(planned.value()));
        }
        _cbStorageBuilder = this->CreateCBufferStorageBuilder(desc).value_or(StructuredBufferStorage::Builder{});
    }
}

BindBridgeLayout::BindBridgeLayout(const BindBridgeIR& ir, const IBindBridgeLayoutPlanner& planner, std::span<const BindBridgeStaticSampler> staticSamplers) noexcept {
    auto planned = this->PlanLayout(ir, planner, staticSamplers);
    if (planned) {
        this->SetPlannedLayout(std::move(planned.value()));
    }
}

void BindBridgeLayout::SetPlannedLayout(BindBridgePlannedLayout&& planned) noexcept {
    _bindings = std::move(planned.Bindings);
    _descriptor = std::move(planned.Descriptor);
    _nameToBindingId = std::move(planned.NameToBindingId);
}

RootSignatureDescriptorContainer BindBridgeLayout::BuildDescriptor(std::span<const BindingEntry> bindings) noexcept {
    RootSignatureDescriptorContainer container{};
    container._staticSamplers.clear();

    vector<std::pair<uint32_t, RootSignatureRootDescriptor>> rootEntries{};
    rootEntries.reserve(bindings.size());
    for (const auto& b : bindings) {
        std::visit(
            [&](const auto& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, RootDescriptorEntry>) {
                    rootEntries.emplace_back(
                        e.RootIndex,
                        RootSignatureRootDescriptor{
                            e.BindPoint,
                            e.Space,
                            e.Type,
                            e.Stages});
                }
            },
            b);
    }
    std::sort(rootEntries.begin(), rootEntries.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    container._rootDescriptors.reserve(rootEntries.size());
    for (const auto& [_, rd] : rootEntries) {
        container._rootDescriptors.push_back(rd);
    }

    unordered_map<uint32_t, vector<const DescriptorSetEntry*>> sets;
    sets.reserve(bindings.size());
    vector<uint32_t> setOrder;
    setOrder.reserve(bindings.size());
    for (const auto& b : bindings) {
        std::visit(
            [&](const auto& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, DescriptorSetEntry>) {
                    if (sets.find(e.SetIndex) == sets.end()) {
                        setOrder.push_back(e.SetIndex);
                    }
                    sets[e.SetIndex].push_back(&e);
                }
            },
            b);
    }
    std::sort(setOrder.begin(), setOrder.end());
    // Pre-reserve to prevent reallocation that would invalidate spans
    size_t totalElements = 0;
    size_t totalStaticSamplers = 0;
    for (auto setIndex : setOrder) {
        for (const auto* e : sets[setIndex]) {
            if (e->IsStaticSampler) {
                totalStaticSamplers += e->BindCount;
            } else {
                totalElements++;
            }
        }
    }

    container._elements.reserve(totalElements);
    container._staticSamplers.reserve(totalStaticSamplers);
    container._descriptorSets.reserve(setOrder.size());
    for (auto setIndex : setOrder) {
        auto& elems = sets[setIndex];
        std::sort(elems.begin(), elems.end(), [](const DescriptorSetEntry* a, const DescriptorSetEntry* b) {
            return a->ElementIndex < b->ElementIndex;
        });
        // Collect static samplers for this set
        for (const auto* e : elems) {
            if (e->IsStaticSampler) {
                for (uint32_t t = 0; t < e->BindCount; t++) {
                    RootSignatureStaticSampler ss{};
                    ss.Slot = e->BindPoint + t;
                    ss.Space = e->Space;
                    ss.SetIndex = setIndex;
                    ss.Stages = e->Stages;
                    if (t < e->StaticSamplerDescs.size()) {
                        ss.Desc = e->StaticSamplerDescs[t];
                    }
                    container._staticSamplers.push_back(ss);
                }
            }
        }
        // Build regular elements and bindless elements
        size_t elemStart = container._elements.size();
        for (const auto* e : elems) {
            if (e->IsStaticSampler) {
                continue;
            }
            RootSignatureSetElement elem{};
            elem.Slot = e->BindPoint;
            elem.Space = e->Space;
            elem.Type = e->Type;
            elem.Count = e->BindCount;
            elem.Stages = e->Stages;
            if (e->BindCount == 0) {
                elem.BindlessCapacity = 262144;  // Default capacity
            }
            container._elements.push_back(elem);
        }
        size_t elemCount = container._elements.size() - elemStart;
        RootSignatureDescriptorSet setDesc{};
        setDesc.SetIndex = setIndex;
        setDesc.Elements = std::span<const RootSignatureSetElement>{
            container._elements.data() + elemStart,
            elemCount};
        container._descriptorSets.push_back(setDesc);
    }

    container._desc.RootDescriptors = container._rootDescriptors;
    container._desc.DescriptorSets = container._descriptorSets;
    container._desc.StaticSamplers = container._staticSamplers;
    bool hasPushConst = false;
    for (const auto& b : bindings) {
        bool found = std::visit(
            [&](const auto& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, PushConstEntry>) {
                    container._desc.Constant = RootSignatureConstant{
                        e.BindPoint,
                        e.Space,
                        e.Size,
                        e.Stages};
                    return true;
                } else {
                    return false;
                }
            },
            b);
        if (found) {
            hasPushConst = true;
            break;
        }
    }
    if (!hasPushConst) {
        container._desc.Constant = std::nullopt;
    }
    return container;
}

RootSignatureDescriptorContainer BindBridgeLayout::GetDescriptor() const noexcept {
    return _descriptor;
}

std::optional<uint32_t> BindBridgeLayout::GetBindingId(std::string_view name) const noexcept {
    auto it = _nameToBindingId.find(name);
    if (it == _nameToBindingId.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<BindBridgePlannedLayout> BindBridgeLayout::PlanLayout(const BindBridgeIR& ir, const IBindBridgeLayoutPlanner& planner, std::span<const BindBridgeStaticSampler> staticSamplers) noexcept {
    return planner.Plan(ir, staticSamplers);
}

namespace {

vector<BindBridgeLayout::BindingEntry> ConvertIRToBindings(const BindBridgeIR& ir) {
    vector<BindBridgeLayout::BindingEntry> bindings{};
    bindings.reserve(ir.Bindings.size());
    for (const auto& b : ir.Bindings) {
        switch (b.PlacementType) {
            case BindBridgeIRBinding::Placement::PushConstant:
                bindings.emplace_back(BindBridgeLayout::PushConstEntry{
                    b.Name,
                    0,
                    b.BindPoint,
                    b.Space,
                    b.Stages,
                    b.Size});
                break;
            case BindBridgeIRBinding::Placement::RootDescriptor:
                bindings.emplace_back(BindBridgeLayout::RootDescriptorEntry{
                    b.Name,
                    0,
                    b.Type,
                    b.BindPoint,
                    b.Space,
                    b.Stages,
                    b.RootIndex});
                break;
            case BindBridgeIRBinding::Placement::DescriptorSet:
                bindings.emplace_back(BindBridgeLayout::DescriptorSetEntry{
                    b.Name,
                    0,
                    b.Type,
                    b.BindCount,
                    b.BindPoint,
                    b.Space,
                    b.Stages,
                    b.SetIndex,
                    b.ElementIndex,
                    false,
                    {}});
                break;
        }
    }
    return bindings;
}

std::optional<BindBridgePlannedLayout> FinalizePlannedLayout(vector<BindBridgeLayout::BindingEntry>&& bindings, std::span<const BindBridgeStaticSampler> staticSamplers) {
    BindBridgePlannedLayout planned{};
    planned.Bindings = std::move(bindings);
    BindBridgeLayout::ApplyStaticSamplers(planned.Bindings, staticSamplers);
    planned.NameToBindingId = BindBridgeLayout::BuildBindingIndex(planned.Bindings);
    planned.Descriptor = BindBridgeLayout::BuildDescriptor(planned.Bindings);
    return planned;
}

std::optional<BindBridgePlannedLayout> PlanByTranscodingIR(const BindBridgeIR& ir, std::span<const BindBridgeStaticSampler> staticSamplers) {
    return FinalizePlannedLayout(ConvertIRToBindings(ir), staticSamplers);
}

bool HasExplicitPlacement(const BindBridgeIR& ir) {
    for (const auto& b : ir.Bindings) {
        if (b.PlacementType != BindBridgeIRBinding::Placement::DescriptorSet) {
            return true;
        }
    }
    return false;
}

bool IsRootDescriptorType(ResourceBindType type) {
    return type == ResourceBindType::CBuffer || type == ResourceBindType::Buffer || type == ResourceBindType::RWBuffer;
}

bool IsResourceType(ResourceBindType type) {
    return type != ResourceBindType::Sampler && type != ResourceBindType::UNKNOWN;
}

}  // namespace

std::optional<BindBridgePlannedLayout> D3D12BindBridgeLayoutPlanner::Plan(const BindBridgeIR& ir, std::span<const BindBridgeStaticSampler> staticSamplers) const noexcept {
    constexpr uint32_t maxRootDWORD = 64;
    constexpr uint32_t maxRootBYTE = maxRootDWORD * 4;

    if (ir.Bindings.empty()) {
        return FinalizePlannedLayout({}, staticSamplers);
    }
    if (HasExplicitPlacement(ir)) {
        return PlanByTranscodingIR(ir, staticSamplers);
    }

    enum class Placement {
        Table,
        RootDescriptor,
        RootConstant,
    };
    vector<Placement> placements(ir.Bindings.size(), Placement::Table);
    auto cmpBySpaceThenBind = [&](size_t lhs, size_t rhs) noexcept {
        const auto& l = ir.Bindings[lhs];
        const auto& r = ir.Bindings[rhs];
        if (l.Space != r.Space) {
            return l.Space < r.Space;
        }
        if (l.BindPoint != r.BindPoint) {
            return l.BindPoint < r.BindPoint;
        }
        return l.Name < r.Name;
    };
    size_t rootConstIndex = std::numeric_limits<size_t>::max();
    uint32_t rootConstSize = 0;
    for (size_t i = 0; i < ir.Bindings.size(); ++i) {
        const auto& b = ir.Bindings[i];
        if (b.Type != ResourceBindType::CBuffer || b.BindCount != 1 || b.Size == 0 || b.Size > maxRootBYTE) {
            continue;
        }
        if (rootConstIndex == std::numeric_limits<size_t>::max() || cmpBySpaceThenBind(i, rootConstIndex)) {
            rootConstIndex = i;
            rootConstSize = b.Size;
        }
    }
    if (rootConstIndex != std::numeric_limits<size_t>::max()) {
        placements[rootConstIndex] = Placement::RootConstant;
    }

    vector<size_t> rootDescs{};
    rootDescs.reserve(ir.Bindings.size());
    for (size_t i = 0; i < ir.Bindings.size(); ++i) {
        if (placements[i] != Placement::Table) {
            continue;
        }
        const auto& b = ir.Bindings[i];
        if (b.BindCount == 1 && IsRootDescriptorType(b.Type)) {
            rootDescs.push_back(i);
            placements[i] = Placement::RootDescriptor;
        }
    }

    auto buildTables = [&]() {
        unordered_map<uint32_t, vector<size_t>> resourceSpace{};
        unordered_map<uint32_t, vector<size_t>> bindlessSpace{};
        unordered_map<uint32_t, vector<size_t>> samplerSpace{};
        resourceSpace.reserve(ir.Bindings.size());
        bindlessSpace.reserve(ir.Bindings.size());
        samplerSpace.reserve(ir.Bindings.size());
        for (size_t i = 0; i < ir.Bindings.size(); ++i) {
            if (placements[i] != Placement::Table) {
                continue;
            }
            const auto& b = ir.Bindings[i];
            if (!IsResourceType(b.Type) && b.Type != ResourceBindType::Sampler) {
                continue;
            }
            if (b.Type == ResourceBindType::Sampler) {
                samplerSpace[b.Space].push_back(i);
            } else if (b.IsBindless || b.BindCount == 0) {
                bindlessSpace[b.Space].push_back(i);
            } else {
                resourceSpace[b.Space].push_back(i);
            }
        }
        vector<vector<size_t>> tables{};
        tables.reserve(resourceSpace.size() + bindlessSpace.size() + samplerSpace.size());
        auto append = [&](unordered_map<uint32_t, vector<size_t>>& groups) {
            vector<uint32_t> spaces{};
            spaces.reserve(groups.size());
            for (const auto& [space, _] : groups) {
                spaces.push_back(space);
            }
            std::sort(spaces.begin(), spaces.end());
            for (auto space : spaces) {
                auto& table = tables.emplace_back(std::move(groups[space]));
                std::sort(table.begin(), table.end(), cmpBySpaceThenBind);
            }
        };
        append(resourceSpace);
        append(bindlessSpace);
        append(samplerSpace);
        return tables;
    };

    vector<vector<size_t>> tables{};
    while (true) {
        std::sort(rootDescs.begin(), rootDescs.end(), cmpBySpaceThenBind);
        tables = buildTables();
        size_t totalDWORD = 0;
        if (rootConstIndex != std::numeric_limits<size_t>::max() && placements[rootConstIndex] == Placement::RootConstant) {
            totalDWORD += Align(rootConstSize, 4) / 4;
        }
        totalDWORD += rootDescs.size() * 2;
        totalDWORD += tables.size();
        if (totalDWORD <= maxRootDWORD) {
            break;
        }
        if (rootConstIndex != std::numeric_limits<size_t>::max() && placements[rootConstIndex] == Placement::RootConstant) {
            placements[rootConstIndex] = Placement::RootDescriptor;
            rootDescs.push_back(rootConstIndex);
            continue;
        }
        if (!rootDescs.empty()) {
            size_t idx = rootDescs.back();
            rootDescs.pop_back();
            placements[idx] = Placement::Table;
            continue;
        }
        RADRAY_ERR_LOG("cannot fit into root signature limits");
        return std::nullopt;
    }

    vector<BindBridgeLayout::BindingEntry> plannedBindings{};
    plannedBindings.reserve(ir.Bindings.size());
    if (rootConstIndex != std::numeric_limits<size_t>::max() && placements[rootConstIndex] == Placement::RootConstant) {
        const auto& b = ir.Bindings[rootConstIndex];
        plannedBindings.emplace_back(BindBridgeLayout::PushConstEntry{
            b.Name,
            0,
            b.BindPoint,
            b.Space,
            b.Stages,
            b.Size});
    }
    uint32_t rootIndex = 0;
    for (auto idx : rootDescs) {
        const auto& b = ir.Bindings[idx];
        plannedBindings.emplace_back(BindBridgeLayout::RootDescriptorEntry{
            b.Name,
            0,
            b.Type,
            b.BindPoint,
            b.Space,
            b.Stages,
            rootIndex++});
    }
    uint32_t setIndex = 0;
    for (const auto& table : tables) {
        uint32_t elemIndex = 0;
        for (auto idx : table) {
            const auto& b = ir.Bindings[idx];
            plannedBindings.emplace_back(BindBridgeLayout::DescriptorSetEntry{
                b.Name,
                0,
                b.Type,
                b.IsBindless ? 0u : b.BindCount,
                b.BindPoint,
                b.Space,
                b.Stages,
                setIndex,
                elemIndex++,
                false,
                {}});
        }
        setIndex++;
    }
    return FinalizePlannedLayout(std::move(plannedBindings), staticSamplers);
}

std::optional<BindBridgePlannedLayout> VulkanBindBridgeLayoutPlanner::Plan(const BindBridgeIR& ir, std::span<const BindBridgeStaticSampler> staticSamplers) const noexcept {
    vector<BindBridgeLayout::BindingEntry> bindings{};
    bindings.reserve(ir.Bindings.size());
    unordered_map<uint32_t, vector<const BindBridgeIRBinding*>> perSet{};
    perSet.reserve(ir.Bindings.size());
    for (const auto& b : ir.Bindings) {
        if (b.PlacementType == BindBridgeIRBinding::Placement::PushConstant) {
            bindings.emplace_back(BindBridgeLayout::PushConstEntry{
                b.Name,
                0,
                b.BindPoint,
                b.Space,
                b.Stages,
                b.Size});
            continue;
        }
        if (b.Type == ResourceBindType::UNKNOWN) {
            continue;
        }
        uint32_t setIndex = b.SetIndex;
        perSet[setIndex].push_back(&b);
    }
    vector<uint32_t> setOrder{};
    setOrder.reserve(perSet.size());
    for (const auto& [set, _] : perSet) {
        setOrder.push_back(set);
    }
    std::sort(setOrder.begin(), setOrder.end());
    for (auto setIndex : setOrder) {
        auto& elems = perSet[setIndex];
        std::sort(elems.begin(), elems.end(), [](const auto* lhs, const auto* rhs) {
            if (lhs->BindPoint != rhs->BindPoint) {
                return lhs->BindPoint < rhs->BindPoint;
            }
            return lhs->Name < rhs->Name;
        });
        bool hasBindless = false;
        bool hasOther = false;
        for (const auto* b : elems) {
            if (b->IsBindless || b->BindCount == 0) {
                hasBindless = true;
            } else {
                hasOther = true;
            }
        }
        if (hasBindless && hasOther) {
            RADRAY_ERR_LOG(
                "Illegal descriptor set layout: set {} contains both bindless and non-bindless resources",
                setIndex);
            return std::nullopt;
        }
        uint32_t elemIndex = 0;
        for (const auto* b : elems) {
            bindings.emplace_back(BindBridgeLayout::DescriptorSetEntry{
                b->Name,
                0,
                b->Type,
                b->IsBindless ? 0u : b->BindCount,
                b->BindPoint,
                b->Space,
                b->Stages,
                setIndex,
                elemIndex++,
                false,
                {}});
        }
    }
    return FinalizePlannedLayout(std::move(bindings), staticSamplers);
}

std::optional<BindBridgePlannedLayout> MetalBindBridgeLayoutPlanner::Plan(const BindBridgeIR& ir, std::span<const BindBridgeStaticSampler> staticSamplers) const noexcept {
    // Metal path consumes descriptor sets as argument buffer indices.
    return VulkanBindBridgeLayoutPlanner{}.Plan(ir, staticSamplers);
}

std::optional<BindBridgeIR> BindBridgeLayout::BuildIRFromHlsl(const HlslShaderDesc& desc) noexcept {
    BindBridgeIR ir{};
    ir.Bindings.reserve(desc.BoundResources.size());
    for (const auto& binding : desc.BoundResources) {
        auto type = binding.MapResourceBindType();
        if (type == ResourceBindType::UNKNOWN) {
            continue;
        }
        BindBridgeIRBinding b{};
        b.Name = binding.Name;
        b.PlacementType = BindBridgeIRBinding::Placement::DescriptorSet;
        b.Type = type;
        b.BindCount = binding.BindCount;
        b.BindPoint = binding.BindPoint;
        b.Space = binding.Space;
        b.Stages = binding.Stages;
        b.IsBindless = binding.BindCount == 0;
        if (type == ResourceBindType::CBuffer) {
            auto cbOpt = desc.FindCBufferByName(binding.Name);
            if (!cbOpt.has_value()) {
                RADRAY_ERR_LOG("cannot find cbuffer data: {}", binding.Name);
                return std::nullopt;
            }
            b.Size = cbOpt.value().get().Size;
        }
        ir.Bindings.emplace_back(std::move(b));
    }
    return ir;
}

std::optional<BindBridgeIR> BindBridgeLayout::BuildIRFromSpirv(const SpirvShaderDesc& desc) noexcept {
    BindBridgeIR ir{};
    ir.Bindings.reserve(desc.ResourceBindings.size() + (desc.PushConstants.empty() ? 0u : 1u));
    if (!desc.PushConstants.empty()) {
        const auto& pc = desc.PushConstants.front();
        ir.Bindings.emplace_back(BindBridgeIRBinding{
            pc.Name,
            BindBridgeIRBinding::Placement::PushConstant,
            ResourceBindType::UNKNOWN,
            0,
            0,
            0,
            pc.Stages,
            pc.Size,
            false,
            0,
            0,
            0});
        if (desc.PushConstants.size() > 1) {
            RADRAY_ERR_LOG("multiple push constants detected, only the first is used: {}", desc.PushConstants.size());
        }
    }
    for (const auto& b : desc.ResourceBindings) {
        auto type = b.MapResourceBindType();
        if (type == ResourceBindType::UNKNOWN) {
            continue;
        }
        uint32_t count = b.ArraySize == 0 ? 1u : b.ArraySize;
        bool isBindless = b.IsUnboundedArray;
        if (isBindless) {
            count = 0;
        }
        ir.Bindings.emplace_back(BindBridgeIRBinding{
            b.Name,
            BindBridgeIRBinding::Placement::DescriptorSet,
            type,
            count,
            b.Binding,
            b.Set,
            b.Stages,
            b.UniformBufferSize,
            isBindless,
            0,
            b.Set,
            0});
    }
    return ir;
}

std::optional<BindBridgeIR> BindBridgeLayout::BuildIRFromMsl(const MslShaderReflection& desc) noexcept {
    auto mapResourceType = [](const MslArgument& arg) -> ResourceBindType {
        switch (arg.Type) {
            case MslArgumentType::Buffer:
                if (arg.Access == MslAccess::ReadWrite || arg.Access == MslAccess::WriteOnly) {
                    return ResourceBindType::RWBuffer;
                }
                if (arg.BufferDataType == MslDataType::Struct) {
                    return ResourceBindType::CBuffer;
                }
                return ResourceBindType::Buffer;
            case MslArgumentType::Texture:
                if (arg.TextureType == MslTextureType::TexBuffer) {
                    if (arg.Access == MslAccess::ReadWrite || arg.Access == MslAccess::WriteOnly) {
                        return ResourceBindType::RWTexelBuffer;
                    }
                    return ResourceBindType::TexelBuffer;
                }
                if (arg.Access == MslAccess::ReadWrite || arg.Access == MslAccess::WriteOnly) {
                    return ResourceBindType::RWTexture;
                }
                return ResourceBindType::Texture;
            case MslArgumentType::Sampler:
                return ResourceBindType::Sampler;
            default:
                return ResourceBindType::UNKNOWN;
        }
    };
    auto mapStages = [](MslStage stage) -> ShaderStages {
        switch (stage) {
            case MslStage::Vertex: return ShaderStage::Vertex;
            case MslStage::Fragment: return ShaderStage::Pixel;
            case MslStage::Compute: return ShaderStage::Compute;
        }
        return ShaderStage::UNKNOWN;
    };
    struct MergedArg {
        string Name;
        ResourceBindType Type;
        uint32_t Index;
        ShaderStages Stages;
        uint64_t ArrayLength;
        bool IsPushConstant;
        bool IsUnboundedArray;
        uint32_t DescriptorSet;
        uint64_t BufferDataSize;
    };
    auto makeKey = [](const MslArgument& arg) -> uint64_t {
        uint64_t pcBit = arg.IsPushConstant ? 1ULL : 0ULL;
        return (static_cast<uint64_t>(arg.Type) << 48) |
               (pcBit << 40) |
               (static_cast<uint64_t>(arg.DescriptorSet) << 32) |
               arg.Index;
    };
    unordered_map<uint64_t, size_t> mergeMap{};
    vector<MergedArg> merged{};
    mergeMap.reserve(desc.Arguments.size());
    merged.reserve(desc.Arguments.size());
    for (const auto& arg : desc.Arguments) {
        if (!arg.IsActive || arg.Type == MslArgumentType::ThreadgroupMemory) {
            continue;
        }
        auto type = mapResourceType(arg);
        if (type == ResourceBindType::UNKNOWN) {
            continue;
        }
        uint64_t key = makeKey(arg);
        auto it = mergeMap.find(key);
        if (it != mergeMap.end()) {
            merged[it->second].Stages = merged[it->second].Stages | mapStages(arg.Stage);
            continue;
        }
        mergeMap[key] = merged.size();
        merged.push_back(MergedArg{
            arg.Name,
            type,
            arg.Index,
            mapStages(arg.Stage),
            arg.ArrayLength,
            arg.IsPushConstant,
            arg.IsUnboundedArray,
            arg.DescriptorSet,
            arg.BufferDataSize});
    }
    BindBridgeIR ir{};
    ir.Bindings.reserve(merged.size());
    for (const auto& m : merged) {
        if (m.IsPushConstant) {
            ir.Bindings.emplace_back(BindBridgeIRBinding{
                m.Name,
                BindBridgeIRBinding::Placement::PushConstant,
                ResourceBindType::UNKNOWN,
                0,
                m.Index,
                0,
                m.Stages,
                static_cast<uint32_t>(m.BufferDataSize),
                false,
                0,
                0,
                0});
            continue;
        }
        uint32_t setIndex = m.DescriptorSet;
        if (desc.UseArgumentBuffers && (m.Stages.HasFlag(ShaderStage::Vertex) || m.Stages.HasFlag(ShaderStage::Pixel))) {
            setIndex = m.DescriptorSet + MetalMaxVertexInputBindings + 1;
        }
        uint32_t count = m.ArrayLength == 0 ? 1u : static_cast<uint32_t>(m.ArrayLength);
        bool isBindless = m.IsUnboundedArray;
        if (isBindless) {
            count = 0;
        }
        ir.Bindings.emplace_back(BindBridgeIRBinding{
            m.Name,
            BindBridgeIRBinding::Placement::DescriptorSet,
            m.Type,
            count,
            m.Index,
            0,
            m.Stages,
            static_cast<uint32_t>(m.BufferDataSize),
            isBindless,
            0,
            setIndex,
            0});
    }
    if (desc.UseArgumentBuffers) {
        for (const auto& b : ir.Bindings) {
            bool graphicsStage = b.Stages.HasFlag(ShaderStage::Vertex) || b.Stages.HasFlag(ShaderStage::Pixel);
            if (!graphicsStage) {
                continue;
            }
            if (b.PlacementType == BindBridgeIRBinding::Placement::PushConstant) {
                if (b.BindPoint != MetalMaxVertexInputBindings) {
                    RADRAY_ERR_LOG(
                        "Invalid Metal push constant slot {} for '{}': expected {} when argument buffers are enabled",
                        b.BindPoint, b.Name, MetalMaxVertexInputBindings);
                    return std::nullopt;
                }
            } else if (b.PlacementType == BindBridgeIRBinding::Placement::DescriptorSet) {
                if (b.SetIndex <= MetalMaxVertexInputBindings) {
                    RADRAY_ERR_LOG(
                        "Invalid Metal descriptor-set slot {} for '{}': must be > {} to avoid vertex-buffer/push-constant overlap",
                        b.SetIndex, b.Name, MetalMaxVertexInputBindings);
                    return std::nullopt;
                }
            }
        }
    }
    return ir;
}

std::optional<vector<BindBridgeLayout::BindingEntry>> BindBridgeLayout::BuildFromHlsl(const HlslShaderDesc& desc) noexcept {
    auto irOpt = BuildIRFromHlsl(desc);
    if (!irOpt.has_value()) {
        return std::nullopt;
    }
    D3D12BindBridgeLayoutPlanner planner{};
    auto plannedOpt = planner.Plan(irOpt.value(), {});
    if (!plannedOpt.has_value()) {
        return std::nullopt;
    }
    return std::make_optional(plannedOpt.value().Bindings);
}

std::optional<vector<BindBridgeLayout::BindingEntry>> BindBridgeLayout::BuildFromSpirv(const SpirvShaderDesc& desc) noexcept {
    auto irOpt = BuildIRFromSpirv(desc);
    if (!irOpt.has_value()) {
        return std::nullopt;
    }
    VulkanBindBridgeLayoutPlanner planner{};
    auto plannedOpt = planner.Plan(irOpt.value(), {});
    if (!plannedOpt.has_value()) {
        return std::nullopt;
    }
    return std::make_optional(plannedOpt.value().Bindings);
}

std::optional<vector<BindBridgeLayout::BindingEntry>> BindBridgeLayout::BuildFromMsl(const MslShaderReflection& desc) noexcept {
    auto irOpt = BuildIRFromMsl(desc);
    if (!irOpt.has_value()) {
        return std::nullopt;
    }
    MetalBindBridgeLayoutPlanner planner{};
    auto plannedOpt = planner.Plan(irOpt.value(), {});
    if (!plannedOpt.has_value()) {
        return std::nullopt;
    }
    return std::make_optional(plannedOpt.value().Bindings);
}

namespace {

struct CBufferBuilderMemberDesc {
    string TypeName;
    string MemberName;
    uint32_t Offset{0};
    size_t SizeInBytes{0};
    uint32_t ArrayCount{0};
    std::optional<uint32_t> ChildStructIndex{};
};

template <class EnumerateMembersFn>
void BuildCBufferStructMembers(
    StructuredBufferStorage::Builder& builder,
    uint32_t rootStructIndex,
    StructuredBufferId rootBdType,
    size_t rootSize,
    EnumerateMembersFn&& enumerateMembers) {
    struct BuildCtx {
        uint32_t StructIndex{0};
        StructuredBufferId BdType{StructuredBufferStorage::InvalidId};
        size_t ParentSize{0};
    };
    stack<BuildCtx> stackCtx{};
    stackCtx.push({rootStructIndex, rootBdType, rootSize});
    while (!stackCtx.empty()) {
        auto ctx = stackCtx.top();
        stackCtx.pop();
        auto members = enumerateMembers(ctx.StructIndex, ctx.ParentSize);
        for (const auto& member : members) {
            auto childBdIdx = builder.AddType(member.TypeName, member.SizeInBytes);
            if (member.ArrayCount > 0) {
                builder.AddMemberForType(ctx.BdType, childBdIdx, member.MemberName, member.Offset, member.ArrayCount);
            } else {
                builder.AddMemberForType(ctx.BdType, childBdIdx, member.MemberName, member.Offset);
            }
            if (member.ChildStructIndex.has_value()) {
                stackCtx.push({member.ChildStructIndex.value(), childBdIdx, member.SizeInBytes});
            }
        }
    }
}

}  // namespace

std::optional<StructuredBufferStorage::Builder> BindBridgeLayout::CreateCBufferStorageBuilder(const HlslShaderDesc& desc) noexcept {
    StructuredBufferStorage::Builder builder{};
    builder.SetAlignment(0);
    auto enumerateMembers = [&](uint32_t parent, size_t parentSize) {
        vector<CBufferBuilderMemberDesc> members{};
        const auto& type = desc.Types[parent];
        members.reserve(type.Members.size());
        for (size_t i = 0; i < type.Members.size(); i++) {
            const auto& member = type.Members[i];
            const auto& memberType = desc.Types[member.Type];
            size_t sizeInBytes = 0;
            if (memberType.IsPrimitive()) {
                sizeInBytes = memberType.GetSizeInBytes();
            } else {
                auto rOffset = i == type.Members.size() - 1 ? parentSize : desc.Types[type.Members[i + 1].Type].Offset;
                sizeInBytes = rOffset - memberType.Offset;
                if (memberType.Elements > 0) {
                    sizeInBytes /= memberType.Elements;
                }
            }
            CBufferBuilderMemberDesc m{};
            m.TypeName = memberType.Name;
            m.MemberName = member.Name;
            m.Offset = static_cast<uint32_t>(memberType.Offset);
            m.SizeInBytes = sizeInBytes;
            m.ArrayCount = memberType.Elements;
            m.ChildStructIndex = static_cast<uint32_t>(member.Type);
            members.emplace_back(std::move(m));
        }
        return members;
    };
    for (const auto& res : desc.BoundResources) {
        if (res.Type != HlslShaderInputType::CBUFFER) {
            continue;
        }
        auto cbOpt = desc.FindCBufferByName(res.Name);
        if (!cbOpt.has_value()) {
            RADRAY_ERR_LOG("cannot find cbuffer: {}", res.Name);
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
            BuildCBufferStructMembers(builder, static_cast<uint32_t>(var.Type), bdTypeIdx, sizeInBytes, enumerateMembers);
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
                BuildCBufferStructMembers(builder, static_cast<uint32_t>(var.Type), bdTypeIdx, sizeInBytes, enumerateMembers);
            }
        }
    }
    return builder;
}

std::optional<StructuredBufferStorage::Builder> BindBridgeLayout::CreateCBufferStorageBuilder(const SpirvShaderDesc& desc) noexcept {
    StructuredBufferStorage::Builder builder{};
    builder.SetAlignment(0);
    auto enumerateMembers = [&](uint32_t parent, size_t /*parentSize*/) {
        vector<CBufferBuilderMemberDesc> members{};
        const auto& type = desc.Types[parent];
        members.reserve(type.Members.size());
        for (const auto& member : type.Members) {
            const auto& memberType = desc.Types[member.TypeIndex];
            size_t sizeInBytes = member.Size;
            if (memberType.ArraySize > 0) {
                sizeInBytes /= memberType.ArraySize;
            }
            CBufferBuilderMemberDesc m{};
            m.TypeName = memberType.Name;
            m.MemberName = member.Name;
            m.Offset = member.Offset;
            m.SizeInBytes = sizeInBytes;
            m.ArrayCount = memberType.ArraySize;
            m.ChildStructIndex = member.TypeIndex;
            members.emplace_back(std::move(m));
        }
        return members;
    };

    for (const auto& res : desc.PushConstants) {
        RADRAY_ASSERT(res.TypeIndex < desc.Types.size());
        auto type = desc.Types[res.TypeIndex];
        StructuredBufferId bdTypeIdx = builder.AddType(type.Name, res.Size);
        builder.AddRoot(res.Name, bdTypeIdx);
        BuildCBufferStructMembers(builder, res.TypeIndex, bdTypeIdx, res.Size, enumerateMembers);
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
            BuildCBufferStructMembers(builder, res.TypeIndex, bdTypeIdx, sizeInBytes, enumerateMembers);
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
                BuildCBufferStructMembers(builder, member.TypeIndex, bdTypeIdx, sizeInBytes, enumerateMembers);
            }
        }
    }
    return builder;
}

std::optional<StructuredBufferStorage::Builder> BindBridgeLayout::CreateCBufferStorageBuilder(const MslShaderReflection& desc) noexcept {
    StructuredBufferStorage::Builder builder{};
    builder.SetAlignment(0);
    auto enumerateMembers = [&](uint32_t structIdx, size_t parentSize) {
        vector<CBufferBuilderMemberDesc> members{};
        if (structIdx >= desc.StructTypes.size()) {
            return members;
        }
        const auto& st = desc.StructTypes[structIdx];
        members.reserve(st.Members.size());
        for (size_t i = 0; i < st.Members.size(); i++) {
            const auto& member = st.Members[i];
            size_t sizeInBytes = 0;
            if (i + 1 < st.Members.size()) {
                sizeInBytes = st.Members[i + 1].Offset - member.Offset;
            } else {
                sizeInBytes = parentSize - member.Offset;
            }
            CBufferBuilderMemberDesc m{};
            m.TypeName = member.Name;
            m.MemberName = member.Name;
            m.Offset = static_cast<uint32_t>(member.Offset);
            m.SizeInBytes = sizeInBytes;
            if (member.DataType == MslDataType::Array && member.ArrayTypeIndex != UINT32_MAX) {
                const auto& arrType = desc.ArrayTypes[member.ArrayTypeIndex];
                if (arrType.ArrayLength > 0) {
                    m.SizeInBytes = sizeInBytes / arrType.ArrayLength;
                    m.ArrayCount = static_cast<uint32_t>(arrType.ArrayLength);
                }
                if (arrType.ElementType == MslDataType::Struct && arrType.ElementStructTypeIndex != UINT32_MAX) {
                    m.ChildStructIndex = arrType.ElementStructTypeIndex;
                }
            } else if (member.DataType == MslDataType::Struct && member.StructTypeIndex != UINT32_MAX) {
                m.ChildStructIndex = member.StructTypeIndex;
            }
            members.emplace_back(std::move(m));
        }
        return members;
    };
    for (const auto& arg : desc.Arguments) {
        if (!arg.IsActive) continue;
        if (arg.Type != MslArgumentType::Buffer) continue;
        if (arg.BufferDataType != MslDataType::Struct) continue;
        if (arg.BufferStructTypeIndex == UINT32_MAX) continue;
        if (arg.Access == MslAccess::ReadWrite || arg.Access == MslAccess::WriteOnly) continue;
        size_t sizeInBytes = arg.BufferDataSize;
        StructuredBufferId bdTypeIdx = builder.AddType(arg.Name, sizeInBytes);
        if (arg.ArrayLength > 0) {
            builder.AddRoot(arg.Name, bdTypeIdx, arg.ArrayLength);
        } else {
            builder.AddRoot(arg.Name, bdTypeIdx);
        }
        BuildCBufferStructMembers(builder, arg.BufferStructTypeIndex, bdTypeIdx, sizeInBytes, enumerateMembers);
    }
    return builder;
}

BindBridgeNameToIdMap BindBridgeLayout::BuildBindingIndex(vector<BindingEntry>& bindings) noexcept {
    BindBridgeNameToIdMap nameToBindingId{};
    nameToBindingId.reserve(bindings.size());
    uint32_t nextId = 0;
    for (auto& entry : bindings) {
        std::visit(
            [&](auto& e) {
                e.Id = nextId;
                if (!e.Name.empty()) {
                    if (nameToBindingId.find(e.Name) == nameToBindingId.end()) {
                        nameToBindingId.emplace(e.Name, nextId);
                    }
                }
            },
            entry);
        ++nextId;
    }
    return nameToBindingId;
}

void BindBridgeLayout::ApplyStaticSamplers(vector<BindingEntry>& bindings, std::span<const BindBridgeStaticSampler> staticSamplers) noexcept {
    for (auto& b : bindings) {
        std::visit(
            [&](auto& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, DescriptorSetEntry>) {
                    if (e.Type == ResourceBindType::Sampler) {
                        e.IsStaticSampler = false;
                    }
                }
            },
            b);
    }
    if (staticSamplers.empty()) {
        return;
    }
    for (const auto& ss : staticSamplers) {
        if (ss.Name.empty() || ss.Samplers.empty()) {
            continue;
        }
        bool matched = false;
        for (auto& b : bindings) {
            std::visit(
                [&](auto& e) {
                    using T = std::decay_t<decltype(e)>;
                    if constexpr (std::is_same_v<T, DescriptorSetEntry>) {
                        if (e.Type != ResourceBindType::Sampler || e.Name != ss.Name) {
                            return;
                        }
                        matched = true;
                        if (ss.Samplers.size() != e.BindCount) {
                            RADRAY_ERR_LOG("static sampler count mismatch: {} {}", e.Name, e.BindCount);
                            return;
                        }
                        e.IsStaticSampler = true;
                        e.StaticSamplerDescs = ss.Samplers;
                    }
                },
                b);
        }
        if (!matched) {
            RADRAY_ERR_LOG("static sampler name not found: {}", ss.Name);
        }
    }
}

BindBridge::BindBridge(Device* device, RootSignature* rootSig, const BindBridgeLayout& layout) {
    auto storageOpt = layout._cbStorageBuilder.Build();
    if (!storageOpt.has_value()) {
        throw BindBridgeException("failed to build cbuffer storage");
    }
    _cbStorage = std::move(storageOpt.value());

    // Collect root/descriptor-set metadata in a single pass
    uint32_t maxRootIndex = 0;
    bool hasRoot = false;
    unordered_map<uint32_t, vector<const BindBridgeLayout::DescriptorSetEntry*>> setBindings;
    unordered_set<uint32_t> setsWithStaticSamplers;
    unordered_set<uint32_t> setsWithBindless;
    unordered_set<uint32_t> allSetIndices;
    setBindings.reserve(layout._bindings.size());
    setsWithStaticSamplers.reserve(layout._bindings.size());
    setsWithBindless.reserve(layout._bindings.size());
    allSetIndices.reserve(layout._bindings.size());

    for (const auto& b : layout._bindings) {
        std::visit(
            [&](const auto& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, BindBridgeLayout::RootDescriptorEntry>) {
                    hasRoot = true;
                    maxRootIndex = std::max(maxRootIndex, e.RootIndex);
                } else if constexpr (std::is_same_v<T, BindBridgeLayout::DescriptorSetEntry>) {
                    allSetIndices.insert(e.SetIndex);

                    if (e.IsStaticSampler) {
                        setsWithStaticSamplers.insert(e.SetIndex);
                    } else if (e.BindCount == 0) {
                        setsWithBindless.insert(e.SetIndex);
                    } else {
                        setBindings[e.SetIndex].push_back(&e);
                    }
                }
            },
            b);
    }

    _rootDescViews.assign(hasRoot ? (maxRootIndex + 1) : 0, RootDescriptorView{});

    _descSets.clear();
    _descSets.reserve(allSetIndices.size());
    // Build element index remap: (setIndex, oldElemIndex) -> newElemIndex
    // This accounts for static samplers being filtered out of record.Bindings
    unordered_map<uint64_t, uint32_t> elemIndexRemap;
    elemIndexRemap.reserve(layout._bindings.size());
    for (auto& [setIndex, bindings] : setBindings) {
        auto& record = _descSets[setIndex];
        record.OwnedSet.reset();
        std::sort(bindings.begin(), bindings.end(), [](const auto* a, const auto* b) {
            return a->ElementIndex < b->ElementIndex;
        });
        record.Bindings.reserve(bindings.size());
        for (uint32_t i = 0; i < bindings.size(); i++) {
            const auto* e = bindings[i];
            uint64_t key = (uint64_t(setIndex) << 32) | e->ElementIndex;
            elemIndexRemap[key] = i;
            DescSetBinding binding{};
            binding.Slot = e->BindPoint;
            binding.Count = e->BindCount;
            binding.Type = e->Type;
            binding.Views.assign(e->BindCount, nullptr);
            binding.Dirty.assign(e->BindCount, 0);
            if (e->Type == ResourceBindType::CBuffer) {
                auto var = _cbStorage.GetVar(e->Name);
                binding.CBufferId = var.IsValid() ? var.GetId() : StructuredBufferStorage::InvalidId;
            }
            record.Bindings.emplace_back(std::move(binding));
        }
    }

    // Now build _bindings with remapped element indices
    _bindings.reserve(layout._bindings.size());
    _nameToBindingId = layout._nameToBindingId;
    unordered_map<string, StructuredBufferId, StringHash, StringEqual> cbufferNameToId{};
    cbufferNameToId.reserve(layout._bindings.size());
    auto getCachedCBufferId = [&](const string& name) noexcept {
        auto it = cbufferNameToId.find(name);
        if (it != cbufferNameToId.end()) {
            return it->second;
        }
        auto var = _cbStorage.GetVar(name);
        StructuredBufferId id = var.IsValid() ? var.GetId() : StructuredBufferStorage::InvalidId;
        cbufferNameToId.emplace(name, id);
        return id;
    };
    for (const auto& b : layout._bindings) {
        std::visit(
            [&](const auto& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, BindBridgeLayout::PushConstEntry>) {
                    PushConstBinding loc{};
                    loc.Size = e.Size;
                    loc.CBufferId = getCachedCBufferId(e.Name);
                    _bindings.emplace_back(loc);
                } else if constexpr (std::is_same_v<T, BindBridgeLayout::RootDescriptorEntry>) {
                    RootDescriptorBinding loc{};
                    loc.RootIndex = e.RootIndex;
                    loc.Type = e.Type;
                    if (e.Type == ResourceBindType::CBuffer) {
                        loc.CBufferId = getCachedCBufferId(e.Name);
                    }
                    _bindings.emplace_back(loc);
                } else {
                    DescriptorSetBindingInfo loc{};
                    loc.SetIndex = e.SetIndex;
                    loc.Type = e.Type;
                    loc.BindCount = e.BindCount;
                    // Remap element index to account for filtered-out static samplers
                    uint64_t key = (uint64_t(e.SetIndex) << 32) | e.ElementIndex;
                    auto remapIt = elemIndexRemap.find(key);
                    loc.ElementIndex = (remapIt != elemIndexRemap.end()) ? remapIt->second : e.ElementIndex;
                    if (e.Type == ResourceBindType::CBuffer) {
                        loc.CBufferId = getCachedCBufferId(e.Name);
                    }
                    _bindings.emplace_back(loc);
                }
            },
            b);
    }

    RenderBackend backend = device->GetBackend();
    for (auto setIndex : allSetIndices) {
        auto it = setBindings.find(setIndex);
        bool hasRegularBindings = it != setBindings.end();
        bool hasStaticSamplers = setsWithStaticSamplers.count(setIndex) > 0;
        bool hasBindless = setsWithBindless.count(setIndex) > 0;

        // Bindless sets are bound separately via BindBindlessArray.
        if (hasBindless) {
            continue;
        }

        // Backend-specific descriptor set creation policy.
        bool shouldCreateDescSet = false;
        switch (backend) {
            case RenderBackend::D3D12:
                // Static samplers are baked into root signature on D3D12.
                shouldCreateDescSet = hasRegularBindings;
                break;
            case RenderBackend::Vulkan:
            case RenderBackend::Metal:
                shouldCreateDescSet = hasRegularBindings || hasStaticSamplers;
                break;
            default:
                shouldCreateDescSet = hasRegularBindings || hasStaticSamplers;
                break;
        }
        if (!shouldCreateDescSet) {
            continue;
        }

        auto setOpt = device->CreateDescriptorSet(rootSig, setIndex);
        if (!setOpt.HasValue()) {
            throw BindBridgeException("failed to create descriptor set for set index " + std::to_string(setIndex));
        }
        auto set = setOpt.Release();
        _descSets[setIndex].OwnedSet = std::move(set);
    }
}

std::optional<uint32_t> BindBridge::GetBindingId(std::string_view name) const noexcept {
    auto it = _nameToBindingId.find(name);
    if (it == _nameToBindingId.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool BindBridge::SetResource(uint32_t id, ResourceView* view, uint32_t arrayIndex) noexcept {
    if (id >= _bindings.size()) {
        RADRAY_ERR_LOG("argument out of range '{}' expected: {}, actual: {}", "id", _bindings.size(), id);
        return false;
    }
    const auto& binding = _bindings[id];
    return std::visit(
        [this, view, arrayIndex](const auto& b) noexcept -> bool {
            using T = std::decay_t<decltype(b)>;
            if constexpr (std::is_same_v<T, PushConstBinding>) {
                RADRAY_ERR_LOG("cannot SetResource on push constant");
                return false;
            } else if constexpr (std::is_same_v<T, RootDescriptorBinding>) {
                RADRAY_ERR_LOG("cannot SetResource on root descriptor");
                return false;
            } else {
                if (b.Type == ResourceBindType::Sampler) {
                    RADRAY_ERR_LOG("cannot SetResource on sampler");
                    return false;
                }
                if (arrayIndex >= b.BindCount) {
                    RADRAY_ERR_LOG("argument out of range '{}' expected: {}, actual: {}", "arrayIndex", b.BindCount, arrayIndex);
                    return false;
                }
                this->SetDescriptorSetResource(b.SetIndex, b.ElementIndex, arrayIndex, view);
                return true;
            }
        },
        binding);
}

bool BindBridge::SetResource(std::string_view name, ResourceView* view, uint32_t arrayIndex) noexcept {
    auto idOpt = this->GetBindingId(name);
    if (!idOpt.has_value()) {
        RADRAY_ERR_LOG("cannot find name: {}", name);
        return false;
    }
    return this->SetResource(idOpt.value(), view, arrayIndex);
}

StructuredBufferView BindBridge::GetCBuffer(uint32_t id) noexcept {
    if (id >= _bindings.size()) {
        return {};
    }
    const auto& binding = _bindings[id];
    StructuredBufferId cbId = StructuredBufferStorage::InvalidId;
    std::visit([&](const auto& b) {
        cbId = b.CBufferId;
    },
               binding);
    if (cbId == StructuredBufferStorage::InvalidId) {
        return {};
    }
    return StructuredBufferView{&_cbStorage, cbId};
}

StructuredBufferReadOnlyView BindBridge::GetCBuffer(uint32_t id) const noexcept {
    if (id >= _bindings.size()) {
        return {};
    }
    const auto& binding = _bindings[id];
    StructuredBufferId cbId = StructuredBufferStorage::InvalidId;
    std::visit(
        [&](const auto& b) {
            cbId = b.CBufferId;
        },
        binding);
    if (cbId == StructuredBufferStorage::InvalidId) {
        return {};
    }
    return StructuredBufferReadOnlyView{&_cbStorage, cbId};
}

void BindBridge::SetRootDescriptor(uint32_t slot, Buffer* buffer, uint64_t offset, uint64_t size) noexcept {
    if (slot >= _rootDescViews.size()) {
        RADRAY_ERR_LOG("argument out of range '{}' expected: {}, actual: {}", "slot", _rootDescViews.size(), slot);
        return;
    }
    _rootDescViews[slot] = RootDescriptorView{buffer, offset, size};
}

void BindBridge::SetDescriptorSetResource(uint32_t setIndex, uint32_t elementIndex, uint32_t arrayIndex, ResourceView* view) noexcept {
    auto recIt = _descSets.find(setIndex);
    if (recIt == _descSets.end()) {
        RADRAY_ERR_LOG("cannot find descriptor set record for setIndex {}", setIndex);
        return;
    }
    auto& record = recIt->second;
    if (elementIndex >= record.Bindings.size()) {
        RADRAY_ERR_LOG("argument out of range '{}' expected: {}, actual: {}", "elementIndex", record.Bindings.size(), elementIndex);
        return;
    }
    auto& binding = record.Bindings[elementIndex];
    if (arrayIndex >= binding.Views.size()) {
        RADRAY_ERR_LOG("argument out of range '{}' expected: {}, actual: {}", "arrayIndex", binding.Views.size(), arrayIndex);
        return;
    }
    if (binding.Views[arrayIndex] != view) {
        binding.Views[arrayIndex] = view;
        binding.Dirty[arrayIndex] = 1;
    }
}

void BindBridge::Bind(CommandEncoder* encoder) {
    if (!encoder) {
        return;
    }
    for (const auto& binding : _bindings) {
        bool pushed = std::visit(
            [this, encoder](const auto& b) noexcept -> bool {
                using T = std::decay_t<decltype(b)>;
                if constexpr (std::is_same_v<T, PushConstBinding>) {
                    if (b.CBufferId == StructuredBufferStorage::InvalidId || b.Size == 0) {
                        return false;
                    }
                    auto span = _cbStorage.GetSpan(b.CBufferId);
                    size_t size = std::min(span.size(), static_cast<size_t>(b.Size));
                    if (size > 0) {
                        encoder->PushConstant(span.data(), size);
                    }
                    return true;
                } else {
                    return false;
                }
            },
            binding);
        if (pushed) {
            break;
        }
    }

    for (size_t i = 0; i < _rootDescViews.size(); ++i) {
        const auto& rootView = _rootDescViews[i];
        if (rootView.Buffer == nullptr) {
            continue;
        }
        encoder->BindRootDescriptor(static_cast<uint32_t>(i), rootView.Buffer, rootView.Offset, rootView.Size);
    }

    for (auto& [setIndex, record] : _descSets) {
        auto* set = record.OwnedSet.get();
        if (!set) {
            continue;
        }
        for (size_t ei = 0; ei < record.Bindings.size(); ei++) {
            auto& binding = record.Bindings[ei];
            if (binding.Type == ResourceBindType::Sampler) {
                continue;
            }
            for (size_t ai = 0; ai < binding.Views.size(); ai++) {
                auto* view = binding.Views[ai];
                if (view && binding.Dirty[ai] != 0) {
                    set->SetResource(static_cast<uint32_t>(ei), static_cast<uint32_t>(ai), view);
                    binding.Dirty[ai] = 0;
                }
            }
        }
        encoder->BindDescriptorSet(setIndex, set);
    }
}

bool BindBridge::Upload(Device* device, CBufferArena& arena) {
    auto& storage = _cbStorage;
    _ownedCBufferViews.clear();
    uint32_t alignment = device->GetDetail().CBufferAlignment;
    if (alignment == 0) {
        alignment = 1;
    }
    for (uint32_t id = 0; id < _bindings.size(); ++id) {
        const auto& binding = _bindings[id];
        if (std::holds_alternative<RootDescriptorBinding>(binding)) {
            const auto& b = std::get<RootDescriptorBinding>(binding);
            if (b.Type != ResourceBindType::CBuffer) {
                continue;
            }
            auto rootView = this->GetCBuffer(id);
            if (!rootView) {
                continue;
            }
            auto span = storage.GetSpan(rootView.GetId(), rootView.GetArrayIndex());
            if (span.empty()) {
                continue;
            }
            size_t uploadSize = Align(span.size(), alignment);
            auto alloc = arena.Allocate(uploadSize);
            std::memcpy(alloc.Mapped, span.data(), span.size());
            this->SetRootDescriptor(b.RootIndex, alloc.Target, alloc.Offset, uploadSize);
        } else if (std::holds_alternative<DescriptorSetBindingInfo>(binding)) {
            const auto& b = std::get<DescriptorSetBindingInfo>(binding);
            if (b.Type != ResourceBindType::CBuffer) {
                continue;
            }
            uint32_t bindCount = b.BindCount;
            auto rootView = this->GetCBuffer(id);
            if (!rootView) {
                continue;
            }
            for (uint32_t arrayIndex = 0; arrayIndex < bindCount; ++arrayIndex) {
                StructuredBufferView view = rootView;
                if (rootView.GetSelf().GetArraySize() > 0) {
                    view = rootView.GetArrayElement(arrayIndex);
                } else if (arrayIndex > 0) {
                    continue;
                }
                auto span = storage.GetSpan(view.GetId(), view.GetArrayIndex());
                if (span.empty()) {
                    continue;
                }
                size_t uploadSize = Align(span.size(), alignment);
                auto alloc = arena.Allocate(uploadSize);
                std::memcpy(alloc.Mapped, span.data(), span.size());
                BufferViewDescriptor viewDesc{
                    alloc.Target,
                    BufferRange{alloc.Offset, uploadSize},
                    0,
                    TextureFormat::UNKNOWN,
                    BufferViewUsage::Uniform};
                auto bvOpt = device->CreateBufferView(viewDesc);
                if (!bvOpt.HasValue()) {
                    RADRAY_ERR_LOG("Device::CreateBufferView failed");
                    return false;
                }
                auto bv = bvOpt.Release();
                _ownedCBufferViews.emplace_back(std::move(bv));
                this->SetResource(id, _ownedCBufferViews.back().get(), arrayIndex);
            }
        }
    }
    return true;
}

void BindBridge::Clear() {
    for (auto& rootDescView : _rootDescViews) {
        rootDescView = RootDescriptorView{};
    }
    for (auto& [_, record] : _descSets) {
        for (auto& binding : record.Bindings) {
            std::fill(binding.Views.begin(), binding.Views.end(), nullptr);
            std::fill(binding.Dirty.begin(), binding.Dirty.end(), 0);
        }
    }
    _ownedCBufferViews.clear();
}

}  // namespace radray::render
