#include <radray/render/pipeline_layout.h>

#include <algorithm>
#include <limits>

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

namespace {

enum class AutoPlacement {
    Table,
    RootDescriptor,
    RootConstant,
};

bool IsRootDescriptorType(ResourceBindType type) {
    return type == ResourceBindType::CBuffer || type == ResourceBindType::Buffer || type == ResourceBindType::RWBuffer;
}

bool IsResourceType(ResourceBindType type) {
    return type != ResourceBindType::Sampler && type != ResourceBindType::UNKNOWN;
}

std::optional<PipelineLayoutPlan> FinalizePlan(vector<ParameterMapping>&& mappings, std::span<const StaticSamplerBinding> staticSamplers) {
    PipelineLayoutPlan plan{};
    plan.Mappings = std::move(mappings);
    PipelineLayout::ApplyStaticSamplers(plan.Mappings, staticSamplers);
    plan.MappingIndicesByName = PipelineLayout::BuildNameIndex(plan.Mappings);
    plan.SignatureDesc = PipelineLayout::BuildDescriptor(plan.Mappings);
    return plan;
}

}  // namespace

std::optional<PipelineLayoutPlan> D3D12LayoutPlanner::Plan(
    std::span<const ShaderParameter> parameters,
    std::span<const StaticSamplerBinding> staticSamplers) const noexcept {
    constexpr uint32_t maxRootDWORD = 64;
    constexpr uint32_t maxRootBYTE = maxRootDWORD * 4;

    if (parameters.empty()) {
        return FinalizePlan({}, staticSamplers);
    }

    vector<AutoPlacement> placements(parameters.size(), AutoPlacement::Table);
    auto cmpBySpaceThenBind = [&](size_t lhs, size_t rhs) noexcept {
        const auto& l = parameters[lhs];
        const auto& r = parameters[rhs];
        if (l.Space != r.Space) {
            return l.Space < r.Space;
        }
        if (l.Register != r.Register) {
            return l.Register < r.Register;
        }
        return l.Name < r.Name;
    };

    size_t forcedRootConstIndex = std::numeric_limits<size_t>::max();
    uint32_t forcedRootConstCount = 0;
    for (size_t i = 0; i < parameters.size(); ++i) {
        const auto& p = parameters[i];
        if (!p.IsPushConstant) {
            continue;
        }
        forcedRootConstCount++;
        if (forcedRootConstIndex == std::numeric_limits<size_t>::max() || cmpBySpaceThenBind(i, forcedRootConstIndex)) {
            forcedRootConstIndex = i;
        }
    }
    if (forcedRootConstCount > 1) {
        RADRAY_ERR_LOG("multiple push constants detected, only the first is used: {}", forcedRootConstCount);
    }

    size_t rootConstIndex = std::numeric_limits<size_t>::max();
    uint32_t rootConstSize = 0;
    bool rootConstIsForced = false;
    if (forcedRootConstIndex != std::numeric_limits<size_t>::max()) {
        rootConstIndex = forcedRootConstIndex;
        rootConstSize = parameters[forcedRootConstIndex].TypeSizeInBytes;
        rootConstIsForced = true;
    } else {
        for (size_t i = 0; i < parameters.size(); ++i) {
            const auto& p = parameters[i];
            if (p.Type != ResourceBindType::CBuffer || p.ArrayLength != 1 || p.TypeSizeInBytes == 0 || p.TypeSizeInBytes > maxRootBYTE) {
                continue;
            }
            if (rootConstIndex == std::numeric_limits<size_t>::max() || cmpBySpaceThenBind(i, rootConstIndex)) {
                rootConstIndex = i;
                rootConstSize = p.TypeSizeInBytes;
            }
        }
    }
    if (rootConstIndex != std::numeric_limits<size_t>::max()) {
        placements[rootConstIndex] = AutoPlacement::RootConstant;
    }

    vector<size_t> rootDescs{};
    rootDescs.reserve(parameters.size());
    for (size_t i = 0; i < parameters.size(); ++i) {
        if (placements[i] != AutoPlacement::Table) {
            continue;
        }
        const auto& p = parameters[i];
        if (p.ArrayLength == 1 && IsRootDescriptorType(p.Type)) {
            rootDescs.push_back(i);
            placements[i] = AutoPlacement::RootDescriptor;
        }
    }

    auto buildTables = [&]() {
        unordered_map<uint32_t, vector<size_t>> resourceSpace{};
        unordered_map<uint32_t, vector<size_t>> bindlessSpace{};
        unordered_map<uint32_t, vector<size_t>> samplerSpace{};
        resourceSpace.reserve(parameters.size());
        bindlessSpace.reserve(parameters.size());
        samplerSpace.reserve(parameters.size());
        for (size_t i = 0; i < parameters.size(); ++i) {
            if (placements[i] != AutoPlacement::Table) {
                continue;
            }
            const auto& p = parameters[i];
            if (p.IsPushConstant) {
                continue;
            }
            if (!IsResourceType(p.Type) && p.Type != ResourceBindType::Sampler) {
                continue;
            }
            if (p.Type == ResourceBindType::Sampler) {
                samplerSpace[p.Space].push_back(i);
            } else if (p.IsBindless || p.ArrayLength == 0) {
                bindlessSpace[p.Space].push_back(i);
            } else {
                resourceSpace[p.Space].push_back(i);
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
        if (rootConstIndex != std::numeric_limits<size_t>::max() && placements[rootConstIndex] == AutoPlacement::RootConstant) {
            totalDWORD += Align(rootConstSize, 4) / 4;
        }
        totalDWORD += rootDescs.size() * 2;
        totalDWORD += tables.size();
        if (totalDWORD <= maxRootDWORD) {
            break;
        }
        if (rootConstIndex != std::numeric_limits<size_t>::max() && placements[rootConstIndex] == AutoPlacement::RootConstant) {
            if (rootConstIsForced) {
                RADRAY_ERR_LOG("cannot fit push constant into root signature limits");
                return std::nullopt;
            }
            placements[rootConstIndex] = AutoPlacement::RootDescriptor;
            rootDescs.push_back(rootConstIndex);
            continue;
        }
        if (!rootDescs.empty()) {
            size_t idx = rootDescs.back();
            rootDescs.pop_back();
            placements[idx] = AutoPlacement::Table;
            continue;
        }
        RADRAY_ERR_LOG("cannot fit into root signature limits");
        return std::nullopt;
    }

    vector<ParameterMapping> mappings{};
    mappings.reserve(parameters.size());
    if (rootConstIndex != std::numeric_limits<size_t>::max() && placements[rootConstIndex] == AutoPlacement::RootConstant) {
        ParameterMapping m{};
        m.Parameter = parameters[rootConstIndex];
        m.Location = PushConstantLocation{
            m.Parameter.Register,
            m.Parameter.TypeSizeInBytes};
        mappings.emplace_back(std::move(m));
    }

    uint32_t rootIndex = 0;
    for (auto idx : rootDescs) {
        ParameterMapping m{};
        m.Parameter = parameters[idx];
        m.Location = RootDescriptorLocation{rootIndex++};
        mappings.emplace_back(std::move(m));
    }

    uint32_t setIndex = 0;
    for (const auto& table : tables) {
        uint32_t elemIndex = 0;
        for (auto idx : table) {
            ParameterMapping m{};
            m.Parameter = parameters[idx];
            m.Location = DescriptorTableLocation{
                setIndex,
                elemIndex++};
            mappings.emplace_back(std::move(m));
        }
        setIndex++;
    }

    return FinalizePlan(std::move(mappings), staticSamplers);
}

std::optional<PipelineLayoutPlan> VulkanLayoutPlanner::Plan(
    std::span<const ShaderParameter> parameters,
    std::span<const StaticSamplerBinding> staticSamplers) const noexcept {
    vector<ParameterMapping> mappings{};
    mappings.reserve(parameters.size());
    unordered_map<uint32_t, vector<const ShaderParameter*>> perSet{};
    perSet.reserve(parameters.size());
    for (const auto& p : parameters) {
        if (p.IsPushConstant) {
            ParameterMapping m{};
            m.Parameter = p;
            m.Location = PushConstantLocation{
                p.Register,
                p.TypeSizeInBytes};
            mappings.emplace_back(std::move(m));
            continue;
        }
        if (p.Type == ResourceBindType::UNKNOWN) {
            continue;
        }
        uint32_t setIndex = p.Space;
        perSet[setIndex].push_back(&p);
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
            if (lhs->Register != rhs->Register) {
                return lhs->Register < rhs->Register;
            }
            return lhs->Name < rhs->Name;
        });
        bool hasBindless = false;
        bool hasOther = false;
        for (const auto* p : elems) {
            if (p->IsBindless || p->ArrayLength == 0) {
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
        for (const auto* p : elems) {
            ParameterMapping m{};
            m.Parameter = *p;
            m.Location = DescriptorTableLocation{
                setIndex,
                elemIndex++};
            mappings.emplace_back(std::move(m));
        }
    }
    return FinalizePlan(std::move(mappings), staticSamplers);
}

std::optional<PipelineLayoutPlan> MetalLayoutPlanner::Plan(
    std::span<const ShaderParameter> parameters,
    std::span<const StaticSamplerBinding> staticSamplers) const noexcept {
    // Metal consumes descriptor sets as argument buffer indices.
    return VulkanLayoutPlanner{}.Plan(parameters, staticSamplers);
}

PipelineLayout::PipelineLayout(const HlslShaderDesc& desc, std::span<const StaticSamplerBinding> staticSamplers) noexcept {
    auto params = ShaderReflection::ExtractParameters(desc);
    D3D12LayoutPlanner planner{};
    auto planOpt = planner.Plan(params, staticSamplers);
    if (planOpt.has_value()) {
        SetPlan(std::move(planOpt.value()));
    }
    _cbStorageBuilder = ShaderReflection::CreateCBufferLayout(desc).value_or(StructuredBufferStorage::Builder{});
}

PipelineLayout::PipelineLayout(const SpirvShaderDesc& desc, std::span<const StaticSamplerBinding> staticSamplers) noexcept {
    auto params = ShaderReflection::ExtractParameters(desc);
    VulkanLayoutPlanner planner{};
    auto planOpt = planner.Plan(params, staticSamplers);
    if (planOpt.has_value()) {
        SetPlan(std::move(planOpt.value()));
    }
    _cbStorageBuilder = ShaderReflection::CreateCBufferLayout(desc).value_or(StructuredBufferStorage::Builder{});
}

PipelineLayout::PipelineLayout(const MslShaderReflection& desc, std::span<const StaticSamplerBinding> staticSamplers) noexcept {
    auto params = ShaderReflection::ExtractParameters(desc);
    MetalLayoutPlanner planner{};
    auto planOpt = planner.Plan(params, staticSamplers);
    if (planOpt.has_value()) {
        SetPlan(std::move(planOpt.value()));
    }
    _cbStorageBuilder = ShaderReflection::CreateCBufferLayout(desc).value_or(StructuredBufferStorage::Builder{});
}

PipelineLayout::PipelineLayout(
    std::span<const ShaderParameter> parameters,
    const IPipelineLayoutPlanner& planner,
    std::span<const StaticSamplerBinding> staticSamplers) noexcept {
    auto planOpt = planner.Plan(parameters, staticSamplers);
    if (planOpt.has_value()) {
        SetPlan(std::move(planOpt.value()));
    }
}

void PipelineLayout::SetPlan(PipelineLayoutPlan&& plan) noexcept {
    _plan = std::move(plan);
}

RootSignatureDescriptorContainer PipelineLayout::GetDescriptor() const noexcept {
    return _plan.SignatureDesc;
}

std::optional<uint32_t> PipelineLayout::GetParameterId(std::string_view name) const noexcept {
    auto it = _plan.MappingIndicesByName.find(name);
    if (it == _plan.MappingIndicesByName.end()) {
        return std::nullopt;
    }
    return it->second;
}

RootSignatureDescriptorContainer PipelineLayout::BuildDescriptor(std::span<const ParameterMapping> mappings) noexcept {
    RootSignatureDescriptorContainer container{};
    container._staticSamplers.clear();

    vector<std::pair<uint32_t, RootSignatureRootDescriptor>> rootEntries{};
    rootEntries.reserve(mappings.size());
    for (const auto& m : mappings) {
        if (!std::holds_alternative<RootDescriptorLocation>(m.Location)) {
            continue;
        }
        const auto& loc = std::get<RootDescriptorLocation>(m.Location);
        rootEntries.emplace_back(
            loc.RootIndex,
            RootSignatureRootDescriptor{
                m.Parameter.Register,
                m.Parameter.Space,
                m.Parameter.Type,
                m.Parameter.Stages});
    }
    std::sort(rootEntries.begin(), rootEntries.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    container._rootDescriptors.reserve(rootEntries.size());
    for (const auto& [_, rd] : rootEntries) {
        container._rootDescriptors.push_back(rd);
    }

    unordered_map<uint32_t, vector<const ParameterMapping*>> sets{};
    sets.reserve(mappings.size());
    vector<uint32_t> setOrder{};
    setOrder.reserve(mappings.size());
    for (const auto& m : mappings) {
        if (!std::holds_alternative<DescriptorTableLocation>(m.Location)) {
            continue;
        }
        const auto& loc = std::get<DescriptorTableLocation>(m.Location);
        if (sets.find(loc.SetIndex) == sets.end()) {
            setOrder.push_back(loc.SetIndex);
        }
        sets[loc.SetIndex].push_back(&m);
    }
    std::sort(setOrder.begin(), setOrder.end());

    size_t totalElements = 0;
    size_t totalStaticSamplers = 0;
    for (auto setIndex : setOrder) {
        for (const auto* m : sets[setIndex]) {
            if (m->IsStaticSampler) {
                totalStaticSamplers += m->Parameter.ArrayLength;
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
        std::sort(elems.begin(), elems.end(), [](const ParameterMapping* a, const ParameterMapping* b) {
            const auto& la = std::get<DescriptorTableLocation>(a->Location);
            const auto& lb = std::get<DescriptorTableLocation>(b->Location);
            return la.ElementIndex < lb.ElementIndex;
        });

        for (const auto* m : elems) {
            if (!m->IsStaticSampler) {
                continue;
            }
            for (uint32_t t = 0; t < m->Parameter.ArrayLength; t++) {
                RootSignatureStaticSampler ss{};
                ss.Slot = m->Parameter.Register + t;
                ss.Space = m->Parameter.Space;
                ss.SetIndex = setIndex;
                ss.Stages = m->Parameter.Stages;
                if (t < m->StaticSamplerDescs.size()) {
                    ss.Desc = m->StaticSamplerDescs[t];
                }
                container._staticSamplers.push_back(ss);
            }
        }

        size_t elemStart = container._elements.size();
        for (const auto* m : elems) {
            if (m->IsStaticSampler) {
                continue;
            }
            RootSignatureSetElement elem{};
            elem.Slot = m->Parameter.Register;
            elem.Space = m->Parameter.Space;
            elem.Type = m->Parameter.Type;
            elem.Count = m->Parameter.ArrayLength;
            elem.Stages = m->Parameter.Stages;
            if (m->Parameter.ArrayLength == 0) {
                elem.BindlessCapacity = 262144;
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
    container._desc.Constant = std::nullopt;
    for (const auto& m : mappings) {
        if (!std::holds_alternative<PushConstantLocation>(m.Location)) {
            continue;
        }
        const auto& pc = std::get<PushConstantLocation>(m.Location);
        container._desc.Constant = RootSignatureConstant{
            m.Parameter.Register,
            m.Parameter.Space,
            pc.Size,
            m.Parameter.Stages};
        break;
    }
    return container;
}

ParameterNameToIdMap PipelineLayout::BuildNameIndex(vector<ParameterMapping>& mappings) noexcept {
    ParameterNameToIdMap nameToId{};
    nameToId.reserve(mappings.size());
    uint32_t nextId = 0;
    for (auto& m : mappings) {
        m.Id = nextId;
        if (!m.Parameter.Name.empty()) {
            if (nameToId.find(m.Parameter.Name) == nameToId.end()) {
                nameToId.emplace(m.Parameter.Name, nextId);
            }
        }
        ++nextId;
    }
    return nameToId;
}

void PipelineLayout::ApplyStaticSamplers(vector<ParameterMapping>& mappings, std::span<const StaticSamplerBinding> staticSamplers) noexcept {
    for (auto& m : mappings) {
        if (!std::holds_alternative<DescriptorTableLocation>(m.Location)) {
            continue;
        }
        if (m.Parameter.Type == ResourceBindType::Sampler) {
            m.IsStaticSampler = false;
            m.StaticSamplerDescs.clear();
        }
    }
    if (staticSamplers.empty()) {
        return;
    }
    for (const auto& ss : staticSamplers) {
        if (ss.Name.empty() || ss.Samplers.empty()) {
            continue;
        }
        bool matched = false;
        for (auto& m : mappings) {
            if (!std::holds_alternative<DescriptorTableLocation>(m.Location)) {
                continue;
            }
            if (m.Parameter.Type != ResourceBindType::Sampler || m.Parameter.Name != ss.Name) {
                continue;
            }
            matched = true;
            if (ss.Samplers.size() != m.Parameter.ArrayLength) {
                RADRAY_ERR_LOG("static sampler count mismatch: {} {}", m.Parameter.Name, m.Parameter.ArrayLength);
                continue;
            }
            m.IsStaticSampler = true;
            m.StaticSamplerDescs = ss.Samplers;
        }
        if (!matched) {
            RADRAY_ERR_LOG("static sampler name not found: {}", ss.Name);
        }
    }
}

}  // namespace radray::render
