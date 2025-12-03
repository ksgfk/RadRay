#include <radray/render/shader_cbuffer_helper.h>

#include <radray/basic_math.h>

#include <algorithm>
#include <bit>
#include <utility>
#include <cstring>

namespace radray::render {

ShaderCBufferView ShaderCBufferView::GetVar(std::string_view name) const noexcept {
    if (!IsValid()) return {};
    for (const auto& member : _type->GetMembers()) {
        if (member.GetVariable()->GetName() == name) {
            return ShaderCBufferView(_storage, member.GetVariable()->GetType(), _offset + member.GetOffset());
        }
    }
    return {};
}

ShaderCBufferView ShaderCBufferStorage::GetVar(std::string_view name) noexcept {
    for (const auto& binding : _bindings) {
        if (binding.GetVariable()->GetName() == name) {
            return ShaderCBufferView(this, binding.GetVariable()->GetType(), binding.GetOffset());
        }
    }
    return {};
}

void ShaderCBufferStorage::WriteData(size_t offset, const void* data, size_t size) noexcept {
    if (offset + size > _buffer.size()) {
        RADRAY_ERR_LOG("ShaderCBufferStorage::WriteData: offset {} + size {} exceeds buffer size {}", offset, size, _buffer.size());
        return;
    }
    std::memcpy(_buffer.data() + offset, data, size);
}

size_t ShaderCBufferStorage::Builder::AddType(std::string_view name, size_t size) noexcept {
    size_t index = _types.size();
    auto& type = _types.emplace_back();
    type.Name = string{name};
    type.SizeInBytes = size;
    return index;
}

void ShaderCBufferStorage::Builder::AddMemberForType(size_t targetType, size_t memberType, std::string_view name, size_t offset) noexcept {
    RADRAY_ASSERT(targetType < _types.size());
    auto& type = _types[targetType];
    auto& member = type.Members.emplace_back();
    member.Name = string{name};
    member.Offset = offset;
    member.TypeIndex = memberType;
}

void ShaderCBufferStorage::Builder::AddRootType(size_t typeIndex) noexcept {
    _roots.push_back(typeIndex);
}

void ShaderCBufferStorage::Builder::SetAlignment(size_t align) noexcept {
    _align = align;
}

bool ShaderCBufferStorage::Builder::IsValid() const noexcept {
    if (_align > 0 && !std::has_single_bit(_align)) {
        RADRAY_ERR_LOG("{}: {}", "alignment must be power of two", _align);
        return false;
    }
    stack<size_t> s;
    vector<int32_t> visited(_types.size(), 0);
    for (size_t i : _roots) {
        s.push(i);
    }
    while (!s.empty()) {
        size_t typeIndex = s.top();
        s.pop();
        if (typeIndex >= _types.size()) {
            RADRAY_ERR_LOG("{}: {}", "invalid type index", typeIndex);
            return false;
        }
        if (visited[typeIndex]) {
            continue;
        }
        visited[typeIndex] = true;
        const auto& type = _types[typeIndex];
        for (const auto& member : type.Members) {
            if (member.Offset + _types[member.TypeIndex].SizeInBytes > type.SizeInBytes) {
                RADRAY_ERR_LOG("{}: {}", "member exceeds type size", member.Name);
                return false;
            }
            s.push(member.TypeIndex);
        }
    }
    return true;
}

std::optional<ShaderCBufferStorage> ShaderCBufferStorage::Builder::Build() noexcept {
    if (!IsValid()) {
        return std::nullopt;
    }
    vector<size_t> remapping(_types.size());
    map<Type, size_t> uniqueTypes;
    for (size_t i = _types.size(); i-- > 0;) {
        const auto& type = _types[i];
        Type sig = type;
        for (auto& member : sig.Members) {
            member.TypeIndex = remapping[member.TypeIndex];
        }
        if (auto it = uniqueTypes.find(sig); it != uniqueTypes.end()) {
            remapping[i] = it->second;
        } else {
            remapping[i] = i;
            uniqueTypes.emplace(std::move(sig), i);
        }
    }
    ShaderCBufferStorage storage;
    vector<size_t> oldToNewIndex(_types.size(), Invalid);
    size_t totalVariables = 0;
    for (const auto& [sig, index] : uniqueTypes) {
        totalVariables += sig.Members.size();
    }
    totalVariables += _roots.size();
    storage._types.resize(uniqueTypes.size());
    storage._variables.reserve(totalVariables);
    vector<ShaderCBufferType*> typeMap(_types.size(), nullptr);
    size_t typeCounter = 0;
    for (size_t i = 0; i < _types.size(); i++) {
        if (remapping[i] == i) {
            oldToNewIndex[i] = typeCounter;
            typeMap[i] = &storage._types[typeCounter++];
            typeMap[i]->_name = _types[i].Name;
            typeMap[i]->_size = _types[i].SizeInBytes;
        }
    }
    for (size_t i = 0; i < _types.size(); i++) {
        if (remapping[i] == i) {
            auto* type = typeMap[i];
            const auto& oldType = _types[i];
            type->_members.reserve(oldType.Members.size());
            for (const auto& member : oldType.Members) {
                storage._variables.emplace_back();
                auto* var = &storage._variables.back();
                var->_name = member.Name;
                size_t childRep = remapping[member.TypeIndex];
                var->_type = typeMap[childRep];
                auto& newMember = type->_members.emplace_back();
                newMember._offset = member.Offset;
                newMember._variable = var;
            }
        }
    }
    storage._bindings.reserve(_roots.size());
    size_t currentOffset = 0;
    for (size_t rootIndex : _roots) {
        size_t repIndex = remapping[rootIndex];
        auto* type = typeMap[repIndex];
        if (_align > 0) {
            currentOffset = Align(currentOffset, _align);
        }
        storage._variables.emplace_back();
        auto* var = &storage._variables.back();
        var->_name = type->_name;
        var->_type = type;
        auto& binding = storage._bindings.emplace_back();
        binding._variable = var;
        binding._offset = currentOffset;
        currentOffset += type->_size;
    }
    storage._buffer.resize(currentOffset, (byte)0);
    return storage;
}

static void _BuildType(ShaderCBufferStorage::Builder& builder, std::string_view name, const HlslShaderTypeDesc* type, size_t typeSize, size_t fromTypeIndex) noexcept {
    struct BuildItem {
        std::string_view Name;
        const HlslShaderTypeDesc* Type;
        size_t FromTypeIndex;
        std::optional<size_t> ForceSize;
    };
    stack<BuildItem> s;
    s.push({name, type, fromTypeIndex, typeSize});
    while (!s.empty()) {
        auto item = s.top();
        s.pop();
        if (item.Type == nullptr) {
            continue;
        }
        size_t currentTypeSize = item.ForceSize.has_value() ? *item.ForceSize : item.Type->GetSizeInBytes();
        auto typeIndex = builder.AddType(item.Name, currentTypeSize);
        builder.AddMemberForType(item.FromTypeIndex, typeIndex, item.Name, item.Type->Offset);
        const auto& members = item.Type->Members;
        for (size_t i = members.size(); i-- > 0;) {
            const auto& member = members[i];
            std::optional<size_t> memberSizeOpt;
            if (!member.Type->IsPrimitive() && member.Type->GetSizeInBytes() == 0) {
                size_t nextOffset = (i + 1 < members.size()) ? members[i + 1].Type->Offset : currentTypeSize;
                if (nextOffset > member.Type->Offset) {
                    memberSizeOpt = nextOffset - member.Type->Offset;
                }
            }
            s.push({member.Name, member.Type, typeIndex, memberSizeOpt});
        }
    }
}

std::optional<ShaderCBufferStorage> CreateCBufferStorage(std::span<const HlslShaderDesc*> descs) noexcept {
    MergedHlslShaderDesc desc = MergeHlslShaderDesc(descs);
    if (desc.BoundResources.empty()) {
        return std::nullopt;
    }
    ShaderCBufferStorage::Builder builder{};
    for (const auto& binding : desc.BoundResources) {
        if (binding.Type != HlslShaderInputType::CBUFFER) {
            continue;
        }
        auto cbufferDataOpt = desc.FindCBufferByName(binding.Name);
        if (!cbufferDataOpt.has_value()) {
            RADRAY_ERR_LOG("{} {}", "CreateCBufferStorage", "cannot find cbuffer data");
            return std::nullopt;
        }
        const auto& cbufferData = cbufferDataOpt.value().get();
        auto rootTypeIndex = builder.AddType(cbufferData.Name, cbufferData.Size);
        builder.AddRootType(rootTypeIndex);
        for (const auto& cbVar : cbufferData.Variables) {
            auto cbType = cbVar->Type;
            size_t varSize = cbVar->Size;
            _BuildType(builder, cbVar->Name, cbType, varSize, rootTypeIndex);
        }
    }
    return builder.Build();
}

}  // namespace radray::render
