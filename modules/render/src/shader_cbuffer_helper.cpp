#include <radray/render/shader_cbuffer_helper.h>

#include <radray/basic_math.h>

#include <algorithm>
#include <bit>
#include <utility>
#include <cstring>

namespace radray::render {

ShaderCBufferView ShaderCBufferView::GetVar(std::string_view name) noexcept {
    if (!IsValid()) return {};
    for (const auto& member : _type->GetMembers()) {
        if (member.GetVariable()->GetName() == name) {
            return ShaderCBufferView{_storage, member.GetVariable()->GetType(), _offset + member.GetOffset(), member.GetId()};
        }
    }
    return {};
}

ShaderCBufferView ShaderCBufferStorage::GetVar(std::string_view name) noexcept {
    for (const auto& binding : _bindings) {
        if (binding.GetVariable()->GetName() == name) {
            return ShaderCBufferView{this, binding.GetVariable()->GetType(), binding.GetOffset(), binding.GetId()};
        }
    }
    return {};
}

ShaderCBufferView ShaderCBufferStorage::GetVar(size_t id) noexcept {
    RADRAY_ASSERT(id <= _idMap.size());
    auto mem = _idMap[id];
    return ShaderCBufferView{this, mem->GetVariable()->GetType(), mem->GetGlobalOffset(), mem->GetId()};
}

void ShaderCBufferStorage::WriteData(size_t offset, const void* data, size_t size) noexcept {
    RADRAY_ASSERT(offset + size <= _buffer.size());
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

void ShaderCBufferStorage::Builder::AddRoot(std::string_view name, size_t typeIndex) noexcept {
    _roots.emplace_back(string{name}, typeIndex);
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
    for (const auto& root : _roots) {
        s.push(root.TypeIndex);
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
    struct VariableView {
        std::string_view Name;
        size_t TypeIndex;
        auto operator<=>(const VariableView&) const = default;
    };
    vector<size_t> remapping(_types.size());
    map<Type, size_t> uniqueTypes;
    map<VariableView, size_t> uniqueVariables;
    vector<VariableView> variableList;
    auto ensureVariable = [&](std::string_view name, size_t typeIndex) {
        VariableView sig{name, typeIndex};
        if (uniqueVariables.find(sig) == uniqueVariables.end()) {
            uniqueVariables.emplace(sig, variableList.size());
            variableList.push_back(sig);
        }
    };
    for (size_t i = _types.size(); i-- > 0;) {
        const auto& type = _types[i];
        Type sig = type;
        for (auto& member : sig.Members) {
            member.TypeIndex = remapping[member.TypeIndex];
            ensureVariable(member.Name, member.TypeIndex);
        }
        if (auto it = uniqueTypes.find(sig); it != uniqueTypes.end()) {
            remapping[i] = it->second;
        } else {
            remapping[i] = i;
            uniqueTypes.emplace(std::move(sig), i);
        }
    }
    for (const auto& root : _roots) {
        ensureVariable(root.Name, remapping[root.TypeIndex]);
    }
    ShaderCBufferStorage storage;
    storage._types.resize(uniqueTypes.size());
    storage._variables.resize(variableList.size());
    vector<ShaderCBufferType*> typeMap(_types.size(), nullptr);
    size_t typeCounter = 0;
    for (size_t i = 0; i < _types.size(); i++) {
        if (remapping[i] == i) {
            typeMap[i] = &storage._types[typeCounter++];
            typeMap[i]->_name = _types[i].Name;
            typeMap[i]->_size = _types[i].SizeInBytes;
        }
    }
    vector<ShaderCBufferVariable*> varPtrs(variableList.size());
    for (size_t i = 0; i < variableList.size(); ++i) {
        const auto& sig = variableList[i];
        storage._variables[i] = ShaderCBufferVariable(string(sig.Name), typeMap[sig.TypeIndex]);
        varPtrs[i] = &storage._variables[i];
    }
    for (size_t i = 0; i < _types.size(); i++) {
        if (remapping[i] == i) {
            auto* type = typeMap[i];
            type->_members.reserve(_types[i].Members.size());
            for (const auto& member : _types[i].Members) {
                VariableView sig{member.Name, remapping[member.TypeIndex]};
                size_t varIndex = uniqueVariables.at(sig);
                type->_members.emplace_back(varPtrs[varIndex], member.Offset);
            }
        }
    }
    storage._bindings.reserve(_roots.size());
    size_t currentOffset = 0;
    for (const auto& root : _roots) {
        size_t repIndex = remapping[root.TypeIndex];
        if (_align > 0) {
            currentOffset = Align(currentOffset, _align);
        }
        VariableView sig{root.Name, repIndex};
        size_t varIndex = uniqueVariables.at(sig);
        storage._bindings.emplace_back(varPtrs[varIndex], currentOffset);
        currentOffset += typeMap[repIndex]->_size;
    }
    storage._buffer.resize(currentOffset, (byte)0);
    BuildMember(storage);
    return storage;
}

void ShaderCBufferStorage::Builder::BuildMember(ShaderCBufferStorage& storage) noexcept {
    struct BuildMemberItem {
        ShaderCBufferMember* Member;
        size_t GlobalOffset;
    };
    stack<BuildMemberItem> s;
    for (auto& binding : storage._bindings) {
        s.push({&binding, binding._offset});
    }
    while (!s.empty()) {
        auto v = s.top();
        s.pop();
        v.Member->_id = storage._idMap.size();
        storage._idMap.push_back(v.Member);
        v.Member->_globalOffset = v.GlobalOffset;
        for (auto& member : v.Member->_variable->_type->_members) {
            s.push({&member, v.GlobalOffset + member._offset});
        }
    }
    storage._idMap.shrink_to_fit();
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
        auto typeIndex = builder.AddType(item.Type->Name, currentTypeSize);
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
        for (const auto& cbVar : cbufferData.Variables) {
            auto cbType = cbVar->Type;
            size_t currentTypeSize = cbVar->Size;
            const auto& members = cbType->Members;
            auto rootTypeIndex = builder.AddType(cbType->Name, cbVar->Size);
            builder.AddRoot(cbVar->Name, rootTypeIndex);
            for (size_t i = 0; i < members.size(); ++i) {
                const auto& member = members[i];
                size_t memberSize = member.Type->GetSizeInBytes();
                if (!member.Type->IsPrimitive() && memberSize == 0) {
                    size_t nextOffset = (i + 1 < members.size()) ? members[i + 1].Type->Offset : currentTypeSize;
                    if (nextOffset > member.Type->Offset) {
                        memberSize = nextOffset - member.Type->Offset;
                    }
                }
                _BuildType(builder, member.Name, member.Type, memberSize, rootTypeIndex);
            }
        }
    }
    return builder.Build();
}

}  // namespace radray::render
