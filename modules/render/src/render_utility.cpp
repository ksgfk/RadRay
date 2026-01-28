#include <radray/render/render_utility.h>

#include <radray/errors.h>

#include <radray/basic_math.h>

#include <cstring>
#include <bit>
#include <algorithm>

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

std::optional<StructuredBufferStorage::Builder> CreateCBufferStorageBuilder(const HlslShaderDesc& desc) noexcept {
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
    return builder;
}

std::optional<StructuredBufferStorage::Builder> CreateCBufferStorageBuilder(const SpirvShaderDesc& desc) noexcept {
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

std::optional<StructuredBufferStorage> CreateCBufferStorage(const HlslShaderDesc& desc) noexcept {
    auto builderOpt = CreateCBufferStorageBuilder(desc);
    if (!builderOpt.has_value()) {
        return std::nullopt;
    }
    return builderOpt->Build();
}

std::optional<StructuredBufferStorage> CreateCBufferStorage(const SpirvShaderDesc& desc) noexcept {
    auto builderOpt = CreateCBufferStorageBuilder(desc);
    if (!builderOpt.has_value()) {
        return std::nullopt;
    }
    return builderOpt->Build();
}

RootSignatureDetail::RootSignatureDetail(const HlslShaderDesc& desc) noexcept {
    auto ins = CreateRootSignatureDetail(desc);
    if (ins.has_value()) {
        *this = std::move(ins.value());
    }
}

RootSignatureDetail::RootSignatureDetail(const SpirvShaderDesc& desc) noexcept {
    // TODO:
}

RootSignatureDetail::RootSignatureDetail(
    vector<DescSet> descSets,
    vector<RootDesc> rootDescs,
    std::optional<PushConst> pushConst,
    StructuredBufferStorage::Builder builder) noexcept
    : _descSets(std::move(descSets)),
      _rootDescs(std::move(rootDescs)),
      _pushConst(std::move(pushConst)),
      _cbStorageBuilder(std::move(builder)) {
    this->BuildBindingIndex();
}

void RootSignatureDetail::BuildBindingIndex() noexcept {
    _bindings.clear();
    _nameToBindingId.clear();
    uint32_t nextId = 0;

    auto addEntry = [&](BindingEntry entry) {
        entry.Id = nextId++;
        if (!entry.Name.empty()) {
            if (_nameToBindingId.find(entry.Name) == _nameToBindingId.end()) {
                _nameToBindingId.emplace(entry.Name, entry.Id);
            }
        }
        _bindings.emplace_back(std::move(entry));
    };

    if (_pushConst.has_value()) {
        const auto& pc = _pushConst.value();
        addEntry(BindingEntry{
            0,
            pc.Name,
            BindingKind::PushConst,
            ResourceBindType::CBuffer,
            1,
            0,
            0,
            0,
            pc.Size});
    }

    for (size_t i = 0; i < _rootDescs.size(); i++) {
        const auto& rd = _rootDescs[i];
        addEntry(BindingEntry{
            0,
            rd.Name,
            BindingKind::RootDescriptor,
            rd.Type,
            rd.BindCount,
            0,
            0,
            static_cast<uint32_t>(i),
            0});
    }

    for (size_t si = 0; si < _descSets.size(); si++) {
        const auto& set = _descSets[si];
        for (size_t ei = 0; ei < set.Elems.size(); ei++) {
            const auto& e = set.Elems[ei];
            addEntry(BindingEntry{
                0,
                e.Name,
                BindingKind::DescriptorSet,
                e.Type,
                e.BindCount,
                static_cast<uint32_t>(si),
                static_cast<uint32_t>(ei),
                0,
                0});
        }
    }
}

const RootSignatureDetail::BindingEntry* RootSignatureDetail::GetBinding(uint32_t id) const noexcept {
    if (id >= _bindings.size()) {
        return nullptr;
    }
    return &_bindings[id];
}

std::optional<uint32_t> RootSignatureDetail::GetBindingId(std::string_view name) const noexcept {
    auto it = _nameToBindingId.find(string{name});
    if (it == _nameToBindingId.end()) {
        return std::nullopt;
    }
    return it->second;
}

RootSignatureDescriptorContainer RootSignatureDetail::ToDescriptor() const noexcept {
    RootSignatureDescriptorContainer container{};
    container._rootDescriptors.reserve(_rootDescs.size());
    for (const auto& rd : _rootDescs) {
        container._rootDescriptors.push_back(RootSignatureRootDescriptor{
            rd.BindPoint,
            rd.Space,
            rd.Type,
            rd.Stages});
    }

    size_t totalElems = 0;
    for (const auto& set : _descSets) {
        totalElems += set.Elems.size();
    }
    container._elements.reserve(totalElems);
    container._descriptorSets.reserve(_descSets.size());

    for (const auto& set : _descSets) {
        size_t elemStart = container._elements.size();
        for (const auto& e : set.Elems) {
            RootSignatureSetElement elem{};
            elem.Slot = e.BindPoint;
            elem.Space = e.Space;
            elem.Type = e.Type;
            elem.Count = e.BindCount;
            elem.Stages = e.Stages;
            elem.StaticSamplers = {};
            container._elements.push_back(elem);
        }
        size_t elemCount = container._elements.size() - elemStart;
        RootSignatureDescriptorSet setDesc{};
        setDesc.Elements = std::span<const RootSignatureSetElement>{
            container._elements.data() + elemStart,
            elemCount};
        container._descriptorSets.push_back(setDesc);
    }

    container._desc.RootDescriptors = container._rootDescriptors;
    container._desc.DescriptorSets = container._descriptorSets;
    if (_pushConst.has_value()) {
        const auto& pc = _pushConst.value();
        container._desc.Constant = RootSignatureConstant{
            pc.BindPoint,
            pc.Space,
            pc.Size,
            pc.Stages};
    } else {
        container._desc.Constant = std::nullopt;
    }
    return container;
}

RootSignatureBinder RootSignatureDetail::MakeBinder() const noexcept {
    RootSignatureBinder binder{};
    auto storageOpt = _cbStorageBuilder.Build();
    if (!storageOpt.has_value()) {
        return binder;
    }
    binder._cbStorage = std::move(storageOpt.value());

    binder._bindings.reserve(_bindings.size());
    binder._nameToBindingId = _nameToBindingId;
    for (const auto& b : _bindings) {
        RootSignatureBinder::BindingLocator loc{};
        loc.Kind = b.Kind;
        loc.Type = b.Type;
        loc.BindCount = b.BindCount;
        loc.SetIndex = b.SetIndex;
        loc.ElementIndex = b.ElementIndex;
        loc.RootIndex = b.RootIndex;
        loc.PushConstSize = b.PushConstSize;
        if (b.Type == ResourceBindType::CBuffer || b.Kind == BindingKind::PushConst) {
            auto var = binder._cbStorage.GetVar(b.Name);
            loc.CBufferId = var.IsValid() ? var.GetId() : StructuredBufferStorage::InvalidId;
        }
        binder._bindings.emplace_back(loc);
    }

    if (_pushConst.has_value()) {
        const auto& pc = _pushConst.value();
        auto var = binder._cbStorage.GetVar(pc.Name);
        binder._cbPushConst = var.IsValid() ? var.GetId() : StructuredBufferStorage::InvalidId;
        binder._pushConstSize = pc.Size;
    }

    binder._cbRootDescs.reserve(_rootDescs.size());
    binder._rootDescViews.assign(_rootDescs.size(), nullptr);
    for (const auto& rd : _rootDescs) {
        if (rd.Type == ResourceBindType::CBuffer) {
            auto var = binder._cbStorage.GetVar(rd.Name);
            binder._cbRootDescs.push_back(var.IsValid() ? var.GetId() : StructuredBufferStorage::InvalidId);
        } else {
            binder._cbRootDescs.push_back(StructuredBufferStorage::InvalidId);
        }
    }

    binder._descSets.resize(_descSets.size());
    for (size_t si = 0; si < _descSets.size(); si++) {
        const auto& set = _descSets[si];
        auto& record = binder._descSets[si];
        record.Set = nullptr;
        record.Bindings.reserve(set.Elems.size());
        for (const auto& e : set.Elems) {
            RootSignatureBinder::DescSetBinding binding{};
            binding.Slot = e.BindPoint;
            binding.Count = e.BindCount;
            binding.Type = e.Type;
            binding.Views.assign(e.BindCount, nullptr);
            if (e.Type == ResourceBindType::CBuffer) {
                auto var = binder._cbStorage.GetVar(e.Name);
                binding.CBufferId = var.IsValid() ? var.GetId() : StructuredBufferStorage::InvalidId;
            }
            record.Bindings.emplace_back(std::move(binding));
        }
    }
    return binder;
}

std::optional<uint32_t> RootSignatureBinder::GetBindingId(std::string_view name) const noexcept {
    auto it = _nameToBindingId.find(string{name});
    if (it == _nameToBindingId.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool RootSignatureBinder::SetResource(uint32_t id, ResourceView* view, uint32_t arrayIndex) noexcept {
    if (id >= _bindings.size()) {
        RADRAY_ERR_LOG("{} {} '{}'", Errors::InvalidOperation, Errors::ArgumentOutOfRange, "id");
        return false;
    }
    const auto& binding = _bindings[id];
    if (binding.Kind == RootSignatureDetail::BindingKind::PushConst) {
        RADRAY_ERR_LOG("{} {} '{}'", Errors::InvalidOperation, Errors::InvalidArgument, "push constant");
        return false;
    }
    if (binding.Type == ResourceBindType::Sampler) {
        RADRAY_ERR_LOG("{} {} '{}'", Errors::InvalidOperation, Errors::InvalidArgument, "sampler");
        return false;
    }
    if (arrayIndex >= binding.BindCount) {
        RADRAY_ERR_LOG("{} {} '{}'", Errors::InvalidOperation, Errors::ArgumentOutOfRange, "arrayIndex");
        return false;
    }
    if (binding.Kind == RootSignatureDetail::BindingKind::RootDescriptor) {
        if (binding.RootIndex >= _rootDescViews.size()) {
            RADRAY_ERR_LOG("{} {} '{}'", Errors::InvalidOperation, Errors::ArgumentOutOfRange, "rootIndex");
            return false;
        }
        _rootDescViews[binding.RootIndex] = view;
        return true;
    }
    if (binding.Kind == RootSignatureDetail::BindingKind::DescriptorSet) {
        if (binding.SetIndex >= _descSets.size()) {
            RADRAY_ERR_LOG("{} {} '{}'", Errors::InvalidOperation, Errors::ArgumentOutOfRange, "setIndex");
            return false;
        }
        auto& record = _descSets[binding.SetIndex];
        if (binding.ElementIndex >= record.Bindings.size()) {
            RADRAY_ERR_LOG("{} {} '{}'", Errors::InvalidOperation, Errors::ArgumentOutOfRange, "elementIndex");
            return false;
        }
        auto& elem = record.Bindings[binding.ElementIndex];
        if (arrayIndex >= elem.Views.size()) {
            RADRAY_ERR_LOG("{} {} '{}'", Errors::InvalidOperation, Errors::ArgumentOutOfRange, "arrayIndex");
            return false;
        }
        elem.Views[arrayIndex] = view;
        return true;
    }
    return false;
}

bool RootSignatureBinder::SetResource(std::string_view name, ResourceView* view, uint32_t arrayIndex) noexcept {
    auto idOpt = this->GetBindingId(name);
    if (!idOpt.has_value()) {
        RADRAY_ERR_LOG("{} {} '{}'", Errors::InvalidOperation, Errors::InvalidArgument, "name");
        return false;
    }
    return this->SetResource(idOpt.value(), view, arrayIndex);
}

StructuredBufferView RootSignatureBinder::GetCBuffer(uint32_t id) noexcept {
    if (id >= _bindings.size()) {
        return {};
    }
    const auto& binding = _bindings[id];
    if (binding.CBufferId == StructuredBufferStorage::InvalidId) {
        return {};
    }
    return StructuredBufferView{&_cbStorage, binding.CBufferId};
}

StructuredBufferReadOnlyView RootSignatureBinder::GetCBuffer(uint32_t id) const noexcept {
    if (id >= _bindings.size()) {
        return {};
    }
    const auto& binding = _bindings[id];
    if (binding.CBufferId == StructuredBufferStorage::InvalidId) {
        return {};
    }
    return StructuredBufferReadOnlyView{&_cbStorage, binding.CBufferId};
}

void RootSignatureBinder::SetRootDescriptor(uint32_t slot, ResourceView* view) noexcept {
    if (slot >= _rootDescViews.size()) {
        RADRAY_ERR_LOG("{} {} '{}'", Errors::InvalidOperation, Errors::ArgumentOutOfRange, "slot");
        return;
    }
    _rootDescViews[slot] = view;
}

void RootSignatureBinder::SetDescriptorSet(uint32_t setIndex, DescriptorSet* set) noexcept {
    if (setIndex >= _descSets.size()) {
        RADRAY_ERR_LOG("{} {} '{}'", Errors::InvalidOperation, Errors::ArgumentOutOfRange, "setIndex");
        return;
    }
    _descSets[setIndex].Set = set;
}

void RootSignatureBinder::SetDescriptorSetResource(uint32_t setIndex, uint32_t elementIndex, uint32_t arrayIndex, ResourceView* view) noexcept {
    if (setIndex >= _descSets.size()) {
        RADRAY_ERR_LOG("{} {} '{}'", Errors::InvalidOperation, Errors::ArgumentOutOfRange, "setIndex");
        return;
    }
    auto& record = _descSets[setIndex];
    if (elementIndex >= record.Bindings.size()) {
        RADRAY_ERR_LOG("{} {} '{}'", Errors::InvalidOperation, Errors::ArgumentOutOfRange, "elementIndex");
        return;
    }
    auto& binding = record.Bindings[elementIndex];
    if (arrayIndex >= binding.Views.size()) {
        RADRAY_ERR_LOG("{} {} '{}'", Errors::InvalidOperation, Errors::ArgumentOutOfRange, "arrayIndex");
        return;
    }
    binding.Views[arrayIndex] = view;
}

void RootSignatureBinder::Bind(CommandEncoder* encoder) const noexcept {
    if (!encoder) {
        return;
    }
    if (_cbPushConst != StructuredBufferStorage::InvalidId && _pushConstSize > 0) {
        auto span = _cbStorage.GetSpan(_cbPushConst);
        size_t size = std::min(span.size(), static_cast<size_t>(_pushConstSize));
        if (size > 0) {
            encoder->PushConstant(span.data(), size);
        }
    }

    for (size_t i = 0; i < _rootDescViews.size(); i++) {
        if (_rootDescViews[i]) {
            encoder->BindRootDescriptor(static_cast<uint32_t>(i), _rootDescViews[i]);
        }
    }

    for (size_t si = 0; si < _descSets.size(); si++) {
        const auto& record = _descSets[si];
        if (!record.Set) {
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
                    record.Set->SetResource(static_cast<uint32_t>(ei), static_cast<uint32_t>(ai), view);
                }
            }
        }
        encoder->BindDescriptorSet(static_cast<uint32_t>(si), record.Set);
    }
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

std::optional<RootSignatureDetail> CreateRootSignatureDetail(const HlslShaderDesc& desc) noexcept {
    constexpr uint32_t maxRootDWORD = 64;
    constexpr uint32_t maxRootBYTE = maxRootDWORD * 4;

    enum class HlslRSPlacement {
        Table,
        RootDescriptor,
        RootConstant,
    };

    if (desc.BoundResources.empty()) {
        return RootSignatureDetail{};
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
        vector<vector<RootSignatureDetail::DescSetElem>> descriptors;
        auto buildDescriptors = [&](const decltype(resourceSpace)& splits) noexcept {
            for (auto [space, indices] : splits) {
                auto& elements = descriptors.emplace_back();
                elements.reserve(indices.size());
                std::sort(indices.begin(), indices.end(), cmpResource);
                for (size_t i : indices) {
                    const auto& binding = desc.BoundResources[i];
                    elements.emplace_back(RootSignatureDetail::DescSetElem{
                        {binding.Name,
                         binding.BindPoint,
                         binding.BindCount,
                         binding.Space,
                         binding.Stages},
                        binding.MapResourceBindType()});
                }
            }
        };
        buildDescriptors(resourceSpace);
        buildDescriptors(samplerSpace);
        return descriptors;
    };

    std::optional<RootSignatureDetail::PushConst> rootConstant;
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
        rootConstant = RootSignatureDetail::PushConst{
            {binding.Name,
             binding.BindPoint,
             binding.BindCount,
             binding.Space,
             binding.Stages},
            cbufferData.Size};
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
    vector<vector<RootSignatureDetail::DescSetElem>> tables;
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
    vector<RootSignatureDetail::RootDesc> rootDescs;
    for (size_t i : asRootDesc) {
        const auto& binding = desc.BoundResources[i];
        rootDescs.emplace_back(RootSignatureDetail::RootDesc{
            {binding.Name,
             binding.BindPoint,
             binding.BindCount,
             binding.Space,
             binding.Stages},
            binding.MapResourceBindType()});
    }
    vector<RootSignatureDetail::DescSet> tablesOut;
    tablesOut.reserve(tables.size());
    for (auto& table : tables) {
        RootSignatureDetail::DescSet set{};
        set.Elems = std::move(table);
        tablesOut.emplace_back(std::move(set));
    }
    auto builderOpt = CreateCBufferStorageBuilder(desc);
    if (!builderOpt.has_value()) {
        return std::nullopt;
    }
    return RootSignatureDetail{
        std::move(tablesOut),
        std::move(rootDescs),
        std::move(rootConstant),
        std::move(builderOpt.value())};
}

}  // namespace radray::render
