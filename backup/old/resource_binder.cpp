#include <radray/render/resource_binder.h>

#include <algorithm>
#include <cstring>

#include <radray/basic_math.h>

namespace radray::render {

namespace {

StructuredBufferStorage BuildCBufferStorageOrThrow(const PipelineLayout& layout) {
    auto storageOpt = layout.GetCBufferStorageBuilder().Build();
    if (!storageOpt.has_value()) {
        throw ResourceBinderException("failed to build cbuffer storage");
    }
    return std::move(storageOpt.value());
}

}  // namespace

ResourceBinder::ResourceBinder(Device* device, RootSignature* rootSig, const PipelineLayout& layout)
    : ResourceBinder(device, rootSig, layout.GetPlan(), BuildCBufferStorageOrThrow(layout)) {}

ResourceBinder::ResourceBinder(Device* device, RootSignature* rootSig, const PipelineLayoutPlan& plan, StructuredBufferStorage cbStorage) {
    _cbStorage = std::move(cbStorage);
    _nameToParameterId = plan.MappingIndicesByName;

    uint32_t maxRootIndex = 0;
    bool hasRoot = false;
    unordered_map<uint32_t, vector<const ParameterMapping*>> setBindings;
    unordered_set<uint32_t> setsWithStaticSamplers;
    unordered_set<uint32_t> setsWithBindless;
    unordered_set<uint32_t> allSetIndices;
    setBindings.reserve(plan.Mappings.size());
    setsWithStaticSamplers.reserve(plan.Mappings.size());
    setsWithBindless.reserve(plan.Mappings.size());
    allSetIndices.reserve(plan.Mappings.size());

    for (const auto& m : plan.Mappings) {
        if (std::holds_alternative<RootDescriptorLocation>(m.Location)) {
            const auto& loc = std::get<RootDescriptorLocation>(m.Location);
            hasRoot = true;
            maxRootIndex = std::max(maxRootIndex, loc.RootIndex);
            continue;
        }
        if (!std::holds_alternative<DescriptorTableLocation>(m.Location)) {
            continue;
        }
        const auto& loc = std::get<DescriptorTableLocation>(m.Location);
        allSetIndices.insert(loc.SetIndex);
        if (m.IsStaticSampler) {
            setsWithStaticSamplers.insert(loc.SetIndex);
        } else if (m.Parameter.ArrayLength == 0) {
            setsWithBindless.insert(loc.SetIndex);
        } else {
            setBindings[loc.SetIndex].push_back(&m);
        }
    }

    _rootDescViews.assign(hasRoot ? (maxRootIndex + 1) : 0, RootDescriptorView{});

    _descSets.clear();
    _descSets.reserve(allSetIndices.size());

    unordered_map<uint64_t, uint32_t> elemIndexRemap;
    elemIndexRemap.reserve(plan.Mappings.size());
    for (auto& [setIndex, mappings] : setBindings) {
        auto& record = _descSets[setIndex];
        record.OwnedSet.reset();
        std::sort(mappings.begin(), mappings.end(), [](const auto* a, const auto* b) {
            const auto& la = std::get<DescriptorTableLocation>(a->Location);
            const auto& lb = std::get<DescriptorTableLocation>(b->Location);
            return la.ElementIndex < lb.ElementIndex;
        });
        record.Bindings.reserve(mappings.size());
        for (uint32_t i = 0; i < mappings.size(); i++) {
            const auto* m = mappings[i];
            const auto& loc = std::get<DescriptorTableLocation>(m->Location);
            uint64_t key = (uint64_t(setIndex) << 32) | loc.ElementIndex;
            elemIndexRemap[key] = i;

            DescSetBinding binding{};
            binding.Slot = m->Parameter.Register;
            binding.Count = m->Parameter.ArrayLength;
            binding.Type = m->Parameter.Type;
            binding.Views.assign(binding.Count, nullptr);
            binding.Dirty.assign(binding.Count, 0);
            if (m->Parameter.Type == ResourceBindType::CBuffer) {
                auto var = _cbStorage.GetVar(m->Parameter.Name);
                binding.CBufferId = var.IsValid() ? var.GetId() : StructuredBufferStorage::InvalidId;
            }
            record.Bindings.emplace_back(std::move(binding));
        }
    }

    _bindings.reserve(plan.Mappings.size());
    for (const auto& m : plan.Mappings) {
        BindingRuntime runtime{};
        runtime.Parameter = m.Parameter;
        runtime.Location = m.Location;
        runtime.IsStaticSampler = m.IsStaticSampler;
        if (std::holds_alternative<DescriptorTableLocation>(runtime.Location)) {
            auto loc = std::get<DescriptorTableLocation>(runtime.Location);
            uint64_t key = (uint64_t(loc.SetIndex) << 32) | loc.ElementIndex;
            auto remapIt = elemIndexRemap.find(key);
            if (remapIt != elemIndexRemap.end()) {
                loc.ElementIndex = remapIt->second;
                runtime.Location = loc;
            }
        }
        if (runtime.Parameter.Type == ResourceBindType::CBuffer) {
            auto var = _cbStorage.GetVar(runtime.Parameter.Name);
            runtime.CBufferId = var.IsValid() ? var.GetId() : StructuredBufferStorage::InvalidId;
        }
        _bindings.emplace_back(std::move(runtime));
    }

    RenderBackend backend = device->GetBackend();
    for (auto setIndex : allSetIndices) {
        auto it = setBindings.find(setIndex);
        bool hasRegularBindings = it != setBindings.end();
        bool hasStaticSamplers = setsWithStaticSamplers.count(setIndex) > 0;
        bool hasBindless = setsWithBindless.count(setIndex) > 0;

        if (hasBindless) {
            continue;
        }

        bool shouldCreateDescSet = false;
        switch (backend) {
            case RenderBackend::D3D12:
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
            throw ResourceBinderException("failed to create descriptor set for set index " + std::to_string(setIndex));
        }
        auto set = setOpt.Release();
        _descSets[setIndex].OwnedSet = std::move(set);
    }
}

std::optional<uint32_t> ResourceBinder::GetParameterId(std::string_view name) const noexcept {
    auto it = _nameToParameterId.find(name);
    if (it == _nameToParameterId.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool ResourceBinder::SetResource(uint32_t id, ResourceView* view, uint32_t arrayIndex) noexcept {
    if (id >= _bindings.size()) {
        RADRAY_ERR_LOG("argument out of range '{}' expected: {}, actual: {}", "id", _bindings.size(), id);
        return false;
    }
    const auto& binding = _bindings[id];
    if (!std::holds_alternative<DescriptorTableLocation>(binding.Location)) {
        RADRAY_ERR_LOG("cannot SetResource on non-descriptor-table parameter");
        return false;
    }
    if (binding.IsStaticSampler || binding.Parameter.Type == ResourceBindType::Sampler) {
        RADRAY_ERR_LOG("cannot SetResource on sampler/static sampler");
        return false;
    }
    if (binding.Parameter.ArrayLength == 0) {
        RADRAY_ERR_LOG("cannot SetResource on bindless parameter");
        return false;
    }
    if (arrayIndex >= binding.Parameter.ArrayLength) {
        RADRAY_ERR_LOG(
            "argument out of range '{}' expected: {}, actual: {}",
            "arrayIndex", binding.Parameter.ArrayLength, arrayIndex);
        return false;
    }
    const auto& loc = std::get<DescriptorTableLocation>(binding.Location);
    SetDescriptorSetResource(loc.SetIndex, loc.ElementIndex, arrayIndex, view);
    return true;
}

bool ResourceBinder::SetResource(std::string_view name, ResourceView* view, uint32_t arrayIndex) noexcept {
    auto idOpt = GetParameterId(name);
    if (!idOpt.has_value()) {
        RADRAY_ERR_LOG("cannot find name: {}", name);
        return false;
    }
    return SetResource(idOpt.value(), view, arrayIndex);
}

StructuredBufferView ResourceBinder::GetCBufferStorage(uint32_t id) noexcept {
    if (id >= _bindings.size()) {
        return {};
    }
    auto cbId = _bindings[id].CBufferId;
    if (cbId == StructuredBufferStorage::InvalidId) {
        return {};
    }
    return StructuredBufferView{&_cbStorage, cbId};
}

StructuredBufferReadOnlyView ResourceBinder::GetCBufferStorage(uint32_t id) const noexcept {
    if (id >= _bindings.size()) {
        return {};
    }
    auto cbId = _bindings[id].CBufferId;
    if (cbId == StructuredBufferStorage::InvalidId) {
        return {};
    }
    return StructuredBufferReadOnlyView{&_cbStorage, cbId};
}

void ResourceBinder::SetRootDescriptor(uint32_t slot, Buffer* buffer, uint64_t offset, uint64_t size) noexcept {
    if (slot >= _rootDescViews.size()) {
        RADRAY_ERR_LOG("argument out of range '{}' expected: {}, actual: {}", "slot", _rootDescViews.size(), slot);
        return;
    }
    _rootDescViews[slot] = RootDescriptorView{buffer, offset, size};
}

void ResourceBinder::SetDescriptorSetResource(uint32_t setIndex, uint32_t elementIndex, uint32_t arrayIndex, ResourceView* view) noexcept {
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

void ResourceBinder::Bind(CommandEncoder* encoder) {
    if (!encoder) {
        return;
    }

    for (const auto& binding : _bindings) {
        if (!std::holds_alternative<PushConstantLocation>(binding.Location)) {
            continue;
        }
        const auto& loc = std::get<PushConstantLocation>(binding.Location);
        if (binding.CBufferId == StructuredBufferStorage::InvalidId || loc.Size == 0) {
            continue;
        }
        auto span = _cbStorage.GetSpan(binding.CBufferId);
        size_t size = std::min(span.size(), static_cast<size_t>(loc.Size));
        if (size > 0) {
            encoder->PushConstant(span.data(), size);
        }
        break;
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

bool ResourceBinder::FlushConstantsTransfers(Device* device, CBufferArena& arena) {
    auto& storage = _cbStorage;
    _ownedCBufferViews.clear();
    uint32_t alignment = device->GetDetail().CBufferAlignment;
    if (alignment == 0) {
        alignment = 1;
    }
    for (uint32_t id = 0; id < _bindings.size(); ++id) {
        const auto& binding = _bindings[id];
        if (binding.CBufferId == StructuredBufferStorage::InvalidId) {
            continue;
        }
        if (std::holds_alternative<RootDescriptorLocation>(binding.Location)) {
            if (binding.Parameter.Type != ResourceBindType::CBuffer) {
                continue;
            }
            const auto& loc = std::get<RootDescriptorLocation>(binding.Location);
            auto rootView = GetCBufferStorage(id);
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
            SetRootDescriptor(loc.RootIndex, alloc.Target, alloc.Offset, uploadSize);
        } else if (std::holds_alternative<DescriptorTableLocation>(binding.Location)) {
            if (binding.Parameter.Type != ResourceBindType::CBuffer || binding.Parameter.ArrayLength == 0) {
                continue;
            }
            uint32_t bindCount = binding.Parameter.ArrayLength;
            auto rootView = GetCBufferStorage(id);
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
                    BufferViewUsage::CBuffer};
                auto bvOpt = device->CreateBufferView(viewDesc);
                if (!bvOpt.HasValue()) {
                    RADRAY_ERR_LOG("Device::CreateBufferView failed");
                    return false;
                }
                auto bv = bvOpt.Release();
                _ownedCBufferViews.emplace_back(std::move(bv));
                SetResource(id, _ownedCBufferViews.back().get(), arrayIndex);
            }
        }
    }
    return true;
}

void ResourceBinder::ResetState() {
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
