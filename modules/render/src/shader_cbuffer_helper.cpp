#include <radray/render/shader_cbuffer_helper.h>

#include <radray/basic_math.h>

#include <algorithm>
#include <bit>
#include <utility>
#include <cstring>
#include <unordered_map>

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

size_t ShaderCBufferStorage::Builder::AddRoot(std::string_view name, size_t typeIndex, size_t offset) noexcept {
    size_t index = _roots.size();
    _roots.emplace_back(string{name}, typeIndex, offset);
    return index;
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
    vector<size_t> rootOffsets(_roots.size());
    for (size_t i = 0; i < _roots.size(); ++i) {
        const auto& root = _roots[i];
        size_t repIndex = remapping[root.TypeIndex];
        if (root.Offset != Invalid) {
            currentOffset = root.Offset;
        } else if (_align > 0) {
            currentOffset = Align(currentOffset, _align);
        }
        rootOffsets[i] = currentOffset;
        VariableView sig{root.Name, repIndex};
        size_t varIndex = uniqueVariables.at(sig);
        storage._bindings.emplace_back(varPtrs[varIndex], currentOffset);
        if (root.Offset == Invalid) {
            currentOffset += typeMap[repIndex]->_size;
        }
    }
    if (_align > 0) {
        currentOffset = Align(currentOffset, _align);
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

std::optional<ShaderCBufferStorage> CreateCBufferStorage(const MergedHlslShaderDesc& desc) noexcept {
    return std::nullopt;
}

std::optional<ShaderCBufferStorage> CreateCBufferStorage(const SpirvShaderDesc& desc) noexcept {
    return std::nullopt;
}

}  // namespace radray::render
