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
    size_t totalBindless = 0;
    for (const auto& set : desc.DescriptorSets) {
        totalElements += set.Elements.size();
        totalBindless += set.BindlessDescriptors.size();
    }
    _elements.reserve(totalElements);
    _bindlessDescriptors.reserve(totalBindless);
    _descriptorSets.reserve(desc.DescriptorSets.size());
    for (const auto& set : desc.DescriptorSets) {
        size_t elemStart = _elements.size();
        size_t bindlessStart = _bindlessDescriptors.size();
        _elements.insert(_elements.end(), set.Elements.begin(), set.Elements.end());
        _bindlessDescriptors.insert(_bindlessDescriptors.end(), set.BindlessDescriptors.begin(), set.BindlessDescriptors.end());
        RootSignatureDescriptorSet ownedSet{};
        ownedSet.Elements = std::span<const RootSignatureSetElement>{
            _elements.data() + elemStart,
            _elements.size() - elemStart};
        ownedSet.BindlessDescriptors = std::span<const RootSignatureBindlessDescriptor>{
            _bindlessDescriptors.data() + bindlessStart,
            _bindlessDescriptors.size() - bindlessStart};
        _descriptorSets.push_back(ownedSet);
    }
    _desc.RootDescriptors = _rootDescriptors;
    _desc.DescriptorSets = _descriptorSets;
    _desc.StaticSamplers = _staticSamplers;
    _desc.Constant = desc.Constant;
}

BindBridgeLayout::BindBridgeLayout(const HlslShaderDesc& desc, std::span<const BindBridgeStaticSampler> staticSamplers) noexcept {
    auto resOpt = this->BuildFromHlsl(desc);
    if (resOpt) {
        _bindings = std::move(resOpt.value());
        this->BuildBindingIndex();
        this->ApplyStaticSamplers(staticSamplers);
        _cbStorageBuilder = this->CreateCBufferStorageBuilder(desc).value_or(StructuredBufferStorage::Builder{});
    }
}

BindBridgeLayout::BindBridgeLayout(const SpirvShaderDesc& desc, std::span<const BindBridgeStaticSampler> staticSamplers) noexcept {
    auto resOpt = this->BuildFromSpirv(desc);
    if (resOpt) {
        _bindings = std::move(resOpt.value());
        this->BuildBindingIndex();
        this->ApplyStaticSamplers(staticSamplers);
        _cbStorageBuilder = this->CreateCBufferStorageBuilder(desc).value_or(StructuredBufferStorage::Builder{});
    }
}

RootSignatureDescriptorContainer BindBridgeLayout::GetDescriptor() const noexcept {
    RootSignatureDescriptorContainer container{};
    container._staticSamplers.clear();

    vector<std::pair<uint32_t, RootSignatureRootDescriptor>> rootEntries;
    for (const auto& b : _bindings) {
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
    vector<uint32_t> setOrder;
    for (const auto& b : _bindings) {
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
    // Also validate for illegal mixed sets (bindless + other descriptors in same set)
    size_t totalElements = 0;
    size_t totalBindless = 0;
    size_t totalStaticSamplers = 0;
    for (auto setIndex : setOrder) {
        bool hasBindless = false;
        bool hasOtherDescriptors = false;
        for (const auto* e : sets[setIndex]) {
            if (e->IsStaticSampler) {
                totalStaticSamplers += e->BindCount;
                hasOtherDescriptors = true;
            } else if (e->BindCount == 0) {
                totalBindless++;
                hasBindless = true;
            } else {
                totalElements++;
                hasOtherDescriptors = true;
            }
        }
        if (hasBindless && hasOtherDescriptors) {
            RADRAY_ERR_LOG(
                "Illegal descriptor set layout: Set {} contains both bindless array and other descriptors. "
                "Bindless arrays must be in their own descriptor set. "
                "This is not supported in HLSL and may cause validation errors.",
                setIndex);
        }
    }

    container._elements.reserve(totalElements);
    container._bindlessDescriptors.reserve(totalBindless);
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
        // Build regular elements and bindless descriptors
        size_t elemStart = container._elements.size();
        size_t bindlessStart = container._bindlessDescriptors.size();
        for (const auto* e : elems) {
            if (e->IsStaticSampler) {
                continue;
            }
            if (e->BindCount == 0) {
                // Bindless descriptor
                RootSignatureBindlessDescriptor bindless{};
                bindless.Slot = e->BindPoint;
                bindless.Space = e->Space;
                bindless.SetIndex = setIndex;
                bindless.Type = e->Type;
                bindless.Stages = e->Stages;
                bindless.Capacity = 262144;  // Default capacity
                container._bindlessDescriptors.push_back(bindless);
            } else {
                // Regular element
                RootSignatureSetElement elem{};
                elem.Slot = e->BindPoint;
                elem.Space = e->Space;
                elem.Type = e->Type;
                elem.Count = e->BindCount;
                elem.Stages = e->Stages;
                container._elements.push_back(elem);
            }
        }
        size_t elemCount = container._elements.size() - elemStart;
        size_t bindlessCount = container._bindlessDescriptors.size() - bindlessStart;
        RootSignatureDescriptorSet setDesc{};
        setDesc.Elements = std::span<const RootSignatureSetElement>{
            container._elements.data() + elemStart,
            elemCount};
        setDesc.BindlessDescriptors = std::span<const RootSignatureBindlessDescriptor>{
            container._bindlessDescriptors.data() + bindlessStart,
            bindlessCount};
        container._descriptorSets.push_back(setDesc);
    }

    container._desc.RootDescriptors = container._rootDescriptors;
    container._desc.DescriptorSets = container._descriptorSets;
    container._desc.StaticSamplers = container._staticSamplers;
    bool hasPushConst = false;
    for (const auto& b : _bindings) {
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

std::optional<uint32_t> BindBridgeLayout::GetBindingId(std::string_view name) const noexcept {
    auto it = _nameToBindingId.find(string{name});
    if (it == _nameToBindingId.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<vector<BindBridgeLayout::BindingEntry>> BindBridgeLayout::BuildFromHlsl(const HlslShaderDesc& desc) noexcept {
    constexpr uint32_t maxRootDWORD = 64;
    constexpr uint32_t maxRootBYTE = maxRootDWORD * 4;

    enum class HlslRSPlacement {
        Table,
        RootDescriptor,
        RootConstant,
    };

    if (desc.BoundResources.empty()) {
        return std::make_optional(vector<BindingEntry>{});
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
        unordered_map<uint32_t, vector<size_t>> resourceSpace, samplerSpace;
        for (size_t i : resourceIndices) {
            resourceSpace[desc.BoundResources[i].Space].push_back(i);
        }
        for (size_t i : samplerIndices) {
            samplerSpace[desc.BoundResources[i].Space].push_back(i);
        }
        vector<vector<size_t>> descriptors;
        auto buildDescriptors = [&](const decltype(resourceSpace)& splits) noexcept {
            for (auto [space, indices] : splits) {
                auto& elements = descriptors.emplace_back();
                elements.reserve(indices.size());
                std::sort(indices.begin(), indices.end(), cmpResource);
                for (size_t i : indices) {
                    elements.emplace_back(i);
                }
            }
        };
        buildDescriptors(resourceSpace);
        buildDescriptors(samplerSpace);
        return descriptors;
    };

    bool hasRootConstant = false;
    uint32_t rootConstantSize = 0;
    size_t rootConstantIndex = std::numeric_limits<size_t>::max();
    size_t bestRootConstIndex = std::numeric_limits<size_t>::max();
    for (size_t i = 0; i < desc.BoundResources.size(); i++) {
        const auto& binding = desc.BoundResources[i];
        if (binding.Type == HlslShaderInputType::CBUFFER && binding.BindCount == 1) {
            auto cbufferDataOpt = desc.FindCBufferByName(binding.Name);
            if (!cbufferDataOpt.has_value()) {
                RADRAY_ERR_LOG("cannot find cbuffer data: {}", binding.Name);
                return std::nullopt;
            }
            const auto& cbufferData = cbufferDataOpt.value().get();
            if (cbufferData.Size > maxRootBYTE) {
                continue;
            }
            uint32_t bestBindPoint = std::numeric_limits<uint32_t>::max();
            uint32_t bestSpace = std::numeric_limits<uint32_t>::max();
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
        hasRootConstant = true;
        rootConstantSize = cbufferData.Size;
        rootConstantIndex = bestRootConstIndex;
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
    vector<vector<size_t>> tables;
    while (true) {
        std::sort(asRootDesc.begin(), asRootDesc.end(), cmpResource);
        tables = buildTable();
        size_t totalDWORD = 0;
        if (hasRootConstant) {
            totalDWORD += Align(rootConstantSize, 4) / 4;
        }
        totalDWORD += asRootDesc.size() * 2;
        totalDWORD += tables.size();
        if (totalDWORD <= maxRootDWORD) {
            break;
        }
        if (hasRootConstant) {
            hasRootConstant = false;
            if (rootConstantIndex != std::numeric_limits<size_t>::max()) {
                asRootDesc.push_back(rootConstantIndex);
                placements[rootConstantIndex] = HlslRSPlacement::RootDescriptor;
            }
            continue;
        }
        if (!asRootDesc.empty()) {
            size_t rmIndex = asRootDesc.back();
            placements[rmIndex] = HlslRSPlacement::Table;
            asRootDesc.pop_back();
            continue;
        }
        RADRAY_ERR_LOG("cannot fit into root signature limits");
        return std::nullopt;
    }
    vector<BindingEntry> bindings;
    if (hasRootConstant && rootConstantIndex != std::numeric_limits<size_t>::max()) {
        const auto& binding = desc.BoundResources[rootConstantIndex];
        bindings.emplace_back(PushConstEntry{
            binding.Name,
            0,
            binding.BindPoint,
            binding.Space,
            binding.Stages,
            rootConstantSize});
    }
    uint32_t rootIndex = 0;
    for (size_t i : asRootDesc) {
        const auto& binding = desc.BoundResources[i];
        bindings.emplace_back(RootDescriptorEntry{
            binding.Name,
            0,
            binding.MapResourceBindType(),
            binding.BindPoint,
            binding.Space,
            binding.Stages,
            rootIndex++});
    }
    uint32_t setIndex = 0;
    for (const auto& table : tables) {
        uint32_t elemIndex = 0;
        for (size_t i : table) {
            const auto& binding = desc.BoundResources[i];
            bindings.emplace_back(DescriptorSetEntry{
                binding.Name,
                0,
                binding.MapResourceBindType(),
                binding.BindCount,
                binding.BindPoint,
                binding.Space,
                binding.Stages,
                setIndex,
                elemIndex++,
                false,
                {}});
        }
        setIndex++;
    }
    bindings.shrink_to_fit();
    return std::make_optional(std::move(bindings));
}

std::optional<vector<BindBridgeLayout::BindingEntry>> BindBridgeLayout::BuildFromSpirv(const SpirvShaderDesc& desc) noexcept {
    if (desc.ResourceBindings.empty() && desc.PushConstants.empty()) {
        return std::make_optional(vector<BindingEntry>{});
    }
    vector<BindingEntry> bindingEntries;
    if (!desc.PushConstants.empty()) {
        const auto& pc = desc.PushConstants.front();
        bindingEntries.emplace_back(PushConstEntry{
            pc.Name,
            0,
            0,
            0,
            pc.Stages,
            pc.Size});
        if (desc.PushConstants.size() > 1) {
            RADRAY_ERR_LOG("multiple push constants detected, only the first is used: {}", desc.PushConstants.size());
        }
    }
    unordered_map<uint32_t, vector<const SpirvResourceBinding*>> perSet;
    for (const auto& binding : desc.ResourceBindings) {
        if (binding.Kind == SpirvResourceKind::UNKNOWN) {
            continue;
        }
        perSet[binding.Set].push_back(&binding);
    }
    vector<uint32_t> setIndices;
    setIndices.reserve(perSet.size());
    for (const auto& [setIndex, _] : perSet) {
        setIndices.push_back(setIndex);
    }
    std::sort(setIndices.begin(), setIndices.end());
    uint32_t setOrderIndex = 0;
    for (auto setIndex : setIndices) {
        auto& bindings = perSet[setIndex];
        std::sort(bindings.begin(), bindings.end(), [](const SpirvResourceBinding* a, const SpirvResourceBinding* b) {
            return a->Binding < b->Binding;
        });
        uint32_t elemIndex = 0;
        for (const auto* b : bindings) {
            auto type = b->MapResourceBindType();
            if (type == ResourceBindType::UNKNOWN) {
                continue;
            }
            uint32_t count = b->ArraySize == 0 ? 1u : b->ArraySize;
            if (b->IsUnboundedArray) {
                count = 0;  // bindless
            }
            bindingEntries.emplace_back(DescriptorSetEntry{
                b->Name,
                0,
                type,
                count,
                b->Binding,
                b->Set,
                b->Stages,
                setOrderIndex,
                elemIndex++,
                false,
                {}});
        }
        setOrderIndex++;
    }
    return std::make_optional(std::move(bindingEntries));
}

std::optional<StructuredBufferStorage::Builder> BindBridgeLayout::CreateCBufferStorageBuilder(const HlslShaderDesc& desc) noexcept {
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
    return builder;
}

std::optional<StructuredBufferStorage::Builder> BindBridgeLayout::CreateCBufferStorageBuilder(const SpirvShaderDesc& desc) noexcept {
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
    return builder;
}

void BindBridgeLayout::BuildBindingIndex() noexcept {
    _nameToBindingId.clear();
    uint32_t nextId = 0;
    for (auto& entry : _bindings) {
        std::visit(
            [&](auto& e) {
                e.Id = nextId;
                if (!e.Name.empty()) {
                    if (_nameToBindingId.find(e.Name) == _nameToBindingId.end()) {
                        _nameToBindingId.emplace(e.Name, nextId);
                    }
                }
            },
            entry);
        ++nextId;
    }
}

void BindBridgeLayout::ApplyStaticSamplers(std::span<const BindBridgeStaticSampler> staticSamplers) noexcept {
    for (auto& b : _bindings) {
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
        for (auto& b : _bindings) {
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

    // Calculate the maximum indices and collect set information in a single pass
    uint32_t maxRootIndex = 0;
    bool hasRoot = false;
    uint32_t maxSetIndex = 0;
    bool hasSet = false;
    unordered_map<uint32_t, vector<const BindBridgeLayout::DescriptorSetEntry*>> setBindings;
    unordered_set<uint32_t> setsWithStaticSamplers;
    unordered_set<uint32_t> setsWithBindless;

    for (const auto& b : layout._bindings) {
        std::visit(
            [&](const auto& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, BindBridgeLayout::RootDescriptorEntry>) {
                    hasRoot = true;
                    maxRootIndex = std::max(maxRootIndex, e.RootIndex);
                } else if constexpr (std::is_same_v<T, BindBridgeLayout::DescriptorSetEntry>) {
                    hasSet = true;
                    maxSetIndex = std::max(maxSetIndex, e.SetIndex);

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
    _descSets.resize(hasSet ? (maxSetIndex + 1) : 0);
    // Build element index remap: (setIndex, oldElemIndex) -> newElemIndex
    // This accounts for static samplers being filtered out of record.Bindings
    unordered_map<uint64_t, uint32_t> elemIndexRemap;
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
    for (const auto& b : layout._bindings) {
        std::visit(
            [&](const auto& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, BindBridgeLayout::PushConstEntry>) {
                    PushConstBinding loc{};
                    loc.Size = e.Size;
                    auto var = _cbStorage.GetVar(e.Name);
                    loc.CBufferId = var.IsValid() ? var.GetId() : StructuredBufferStorage::InvalidId;
                    _bindings.emplace_back(loc);
                } else if constexpr (std::is_same_v<T, BindBridgeLayout::RootDescriptorEntry>) {
                    RootDescriptorBinding loc{};
                    loc.RootIndex = e.RootIndex;
                    loc.Type = e.Type;
                    if (e.Type == ResourceBindType::CBuffer) {
                        auto var = _cbStorage.GetVar(e.Name);
                        loc.CBufferId = var.IsValid() ? var.GetId() : StructuredBufferStorage::InvalidId;
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
                        auto var = _cbStorage.GetVar(e.Name);
                        loc.CBufferId = var.IsValid() ? var.GetId() : StructuredBufferStorage::InvalidId;
                    }
                    _bindings.emplace_back(loc);
                }
            },
            b);
    }

    for (uint32_t i = 0; i < _descSets.size(); ++i) {
        auto it = setBindings.find(i);
        bool hasStaticSamplers = setsWithStaticSamplers.count(i) > 0;
        bool hasBindless = setsWithBindless.count(i) > 0;

        // Skip bindless sets (they are bound separately via BindBindlessArray)
        if (hasBindless && it == setBindings.end() && !hasStaticSamplers) {
            // Pure bindless set with no regular bindings or static samplers
            continue;
        }

        // Create descriptor set for:
        // 1. Sets with regular bindings
        // 2. Sets with static samplers (Vulkan requires these to be bound)
        // 3. Mixed sets (bindless + regular bindings or static samplers)
        if (it == setBindings.end() && !hasStaticSamplers) {
            // No bindings and no static samplers - skip
            continue;
        }

        auto setOpt = device->CreateDescriptorSet(rootSig, i);
        if (!setOpt.HasValue()) {
            // Backend doesn't need a descriptor set for this (e.g., D3D12 static-sampler-only set)
            continue;
        }
        auto set = setOpt.Release();
        _descSets[i].OwnedSet = std::move(set);
    }
}

std::optional<uint32_t> BindBridge::GetBindingId(std::string_view name) const noexcept {
    auto it = _nameToBindingId.find(string{name});
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
    if (setIndex >= _descSets.size()) {
        RADRAY_ERR_LOG("argument out of range '{}' expected: {}, actual: {}", "setIndex", _descSets.size(), setIndex);
        return;
    }
    auto& record = _descSets[setIndex];
    if (elementIndex >= record.Bindings.size()) {
        RADRAY_ERR_LOG("argument out of range '{}' expected: {}, actual: {}", "elementIndex", record.Bindings.size(), elementIndex);
        return;
    }
    auto& binding = record.Bindings[elementIndex];
    if (arrayIndex >= binding.Views.size()) {
        RADRAY_ERR_LOG("argument out of range '{}' expected: {}, actual: {}", "arrayIndex", binding.Views.size(), arrayIndex);
        return;
    }
    binding.Views[arrayIndex] = view;
}

void BindBridge::Bind(CommandEncoder* encoder) const {
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

    for (size_t si = 0; si < _descSets.size(); si++) {
        const auto& record = _descSets[si];
        auto* set = record.OwnedSet.get();
        if (!set) {
            continue;
        }
        for (size_t ei = 0; ei < record.Bindings.size(); ei++) {
            const auto& binding = record.Bindings[ei];
            if (binding.Type == ResourceBindType::Sampler) {
                continue;
            }
            for (size_t ai = 0; ai < binding.Views.size(); ai++) {
                auto* view = binding.Views[ai];
                if (view) {
                    set->SetResource(static_cast<uint32_t>(ei), static_cast<uint32_t>(ai), view);
                }
            }
        }
        encoder->BindDescriptorSet(static_cast<uint32_t>(si), set);
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
                    BufferUse::CBuffer};
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
    _rootDescViews.clear();
    for (auto& record : _descSets) {
        for (auto& binding : record.Bindings) {
            std::fill(binding.Views.begin(), binding.Views.end(), nullptr);
        }
    }
    _ownedCBufferViews.clear();
}

}  // namespace radray::render
