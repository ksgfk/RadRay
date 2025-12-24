#include <radray/structured_buffer.h>

namespace radray {

StructuredBufferId StructuredBufferStorage::Builder::AddType(std::string_view name, size_t size) noexcept {
    Type t{};
    t.Name = string{name};
    t.SizeInBytes = size;
    _types.push_back(std::move(t));
    return StructuredBufferId{_types.size() - 1};
}

void StructuredBufferStorage::Builder::AddMemberForType(StructuredBufferId targetType, StructuredBufferId memberType, std::string_view name, size_t offset) noexcept {
    const size_t targetTypeIndex = static_cast<size_t>(targetType);
    const size_t memberTypeIndex = static_cast<size_t>(memberType);
    if (targetTypeIndex >= _types.size() || memberTypeIndex >= _types.size()) {
        RADRAY_ERR_LOG("StructuredBufferStorage::Builder::AddMemberForType invalid index target={} member={}", targetTypeIndex, memberTypeIndex);
        return;
    }
    Member m{};
    m.Name = string{name};
    m.Offset = offset;
    m.TypeIndex = memberType;
    _types[targetTypeIndex].Members.push_back(std::move(m));
}

void StructuredBufferStorage::Builder::AddMemberForType(StructuredBufferId targetType, StructuredBufferId memberType, std::string_view name, size_t offset, size_t arraySize) noexcept {
    const size_t targetTypeIndex = static_cast<size_t>(targetType);
    const size_t memberTypeIndex = static_cast<size_t>(memberType);
    if (targetTypeIndex >= _types.size() || memberTypeIndex >= _types.size()) {
        RADRAY_ERR_LOG("StructuredBufferStorage::Builder::AddMemberForType invalid index target={} member={}", targetTypeIndex, memberTypeIndex);
        return;
    }
    Member m{};
    m.Name = string{name};
    m.Offset = offset;
    m.TypeIndex = memberType;
    m.ArraySize = arraySize;
    _types[targetTypeIndex].Members.push_back(std::move(m));
}

StructuredBufferId StructuredBufferStorage::Builder::AddRoot(std::string_view name, StructuredBufferId typeIndex) noexcept {
    const size_t typeIndexValue = static_cast<size_t>(typeIndex);
    if (typeIndexValue >= _types.size()) {
        RADRAY_ERR_LOG("StructuredBufferStorage::Builder::AddRoot invalid typeIndex={}", typeIndexValue);
        return Invalid;
    }
    Root r{};
    r.Name = string{name};
    r.TypeIndex = typeIndex;
    _roots.push_back(std::move(r));
    return StructuredBufferId{_roots.size() - 1};
}

void StructuredBufferStorage::Builder::SetAlignment(size_t align) noexcept {
    _align = align;
}

bool StructuredBufferStorage::Builder::IsValid() const noexcept {
    if (_align != 0 && (_align & (_align - 1)) != 0) {
        return false;
    }
    for (size_t ti = 0; ti < _types.size(); ++ti) {
        const auto& t = _types[ti];
        for (const auto& m : t.Members) {
            if (static_cast<size_t>(m.TypeIndex) >= _types.size()) {
                return false;
            }
            const auto& mt = _types[static_cast<size_t>(m.TypeIndex)];
            if (m.Offset > t.SizeInBytes) {
                return false;
            }
            if (mt.SizeInBytes > t.SizeInBytes) {
                return false;
            }
            if (m.Offset + mt.SizeInBytes > t.SizeInBytes) {
                return false;
            }
        }
    }
    for (const auto& r : _roots) {
        if (static_cast<size_t>(r.TypeIndex) >= _types.size()) {
            return false;
        }
    }
    return true;
}

void StructuredBufferStorage::Builder::BuildMember(StructuredBufferStorage& storage) noexcept {
    storage._globalIndex.clear();
    struct StackItem {
        StructuredBufferId TypeId;
        size_t GlobalOffset;
        StructuredBufferId ParentTypeId;
        size_t MemberIndexInType;
    };
    stack<StackItem> s;
    for (const auto& root : storage._rootVarIds) {
        s.push({root._typeId, root._offset, StructuredBufferId::Invalid, StructuredBufferId::Invalid});
    }
    while (!s.empty()) {
        auto item = s.top();
        s.pop();
        StructuredBufferStorage::GlobalVarIndexer idx{};
        idx.MemberIndexInType = item.MemberIndexInType;
        idx.GlobalOffset = item.GlobalOffset;
        idx.ParentTypeId = item.ParentTypeId;
        storage._globalIndex.push_back(idx);
        const auto& type = storage._types[item.TypeId];
        for (size_t mi = 0; mi < type._members.size(); ++mi) {
            const auto& mem = type._members[mi];
            s.push({mem._typeId, item.GlobalOffset + mem._offset, item.TypeId, mi});
        }
    }
    storage._globalIndex.shrink_to_fit();
    for (size_t i = 0; i < storage._rootVarIds.size(); i++) {
        storage._rootVarIds[i]._globalId = i;
        storage._globalIndex[i].MemberIndexInType = i;
    }
    for (size_t i = 0; i < storage._globalIndex.size(); i++) {
        const auto& indexer = storage._globalIndex[i];
        if (indexer.ParentTypeId == StructuredBufferId::Invalid) {
            continue;
        }
        auto& parentType = storage._types[indexer.ParentTypeId];
        auto& memberInType = parentType._members[indexer.MemberIndexInType];
        memberInType._globalId = i;
    }
}

std::optional<StructuredBufferStorage> StructuredBufferStorage::Builder::Build() noexcept {
    if (!IsValid()) {
        RADRAY_ERR_LOG("invalid StructuredBufferStorage::Builder state");
        return std::nullopt;
    }
    StructuredBufferStorage storage{};
    // Phase 1: build the full builder type graph (canonicalized member order)
    struct CanonMemberBuilder {
        std::string_view Name;
        size_t Offset;
        StructuredBufferId TypeIndex;
        size_t ArraySize{0};
    };
    vector<vector<CanonMemberBuilder>> canonMembers;
    canonMembers.resize(_types.size());
    for (size_t ti = 0; ti < _types.size(); ++ti) {
        const auto& t = _types[ti];
        auto& out = canonMembers[ti];
        out.reserve(t.Members.size());
        for (const auto& m : t.Members) {
            out.push_back({m.Name, m.Offset, m.TypeIndex, m.ArraySize});
        }
        std::sort(out.begin(), out.end(), [](const CanonMemberBuilder& a, const CanonMemberBuilder& b) {
            if (a.Offset != b.Offset) {
                return a.Offset < b.Offset;
            }
            if (a.Name != b.Name) {
                return a.Name < b.Name;
            }
            return a.TypeIndex < b.TypeIndex;
        });
    }
    // Phase 2: merge graph nodes via recursive structural equality (no string signatures, no hash map)
    // 0 = unknown, 1 = in-progress, 2 = equal, 3 = not-equal
    const size_t nTypes = _types.size();
    vector<uint8_t> eqMemo;
    eqMemo.resize(nTypes * nTypes, 0);
    auto eqCell = [&](size_t a, size_t b) -> uint8_t& {
        return eqMemo[a * nTypes + b];
    };
    const std::function<bool(size_t, size_t)> equalBuilderTypes = [&](size_t a, size_t b) -> bool {
        if (a == b) {
            return true;
        }
        if (a >= _types.size() || b >= _types.size()) {
            return false;
        }
        // normalize key so (a,b) and (b,a) share memo
        const size_t x = std::min(a, b);
        const size_t y = std::max(a, b);
        uint8_t& state = eqCell(x, y);
        if (state == 2) {
            return true;
        }
        if (state == 3) {
            return false;
        }
        if (state == 1) {
            // in-progress: break recursion loops conservatively
            return true;
        }
        state = 1;
        const auto& ta = _types[a];
        const auto& tb = _types[b];
        if (ta.Name != tb.Name || ta.SizeInBytes != tb.SizeInBytes) {
            state = 3;
            return false;
        }
        const auto& ma = canonMembers[a];
        const auto& mb = canonMembers[b];
        if (ma.size() != mb.size()) {
            state = 3;
            return false;
        }
        for (size_t i = 0; i < ma.size(); ++i) {
            if (ma[i].Offset != mb[i].Offset || ma[i].Name != mb[i].Name) {
                state = 3;
                return false;
            }
            if (!equalBuilderTypes(ma[i].TypeIndex, mb[i].TypeIndex)) {
                state = 3;
                return false;
            }
        }
        state = 2;
        return true;
    };
    // union-find over builder type indices
    vector<size_t> parent(_types.size());
    vector<uint32_t> rank(_types.size(), 0);
    for (size_t i = 0; i < parent.size(); ++i) {
        parent[i] = i;
    }
    const std::function<size_t(size_t)> ufFind = [&](size_t x) -> size_t {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };
    auto ufUnion = [&](size_t a, size_t b) {
        a = ufFind(a);
        b = ufFind(b);
        if (a == b) {
            return;
        }
        if (rank[a] < rank[b]) {
            std::swap(a, b);
        }
        parent[b] = a;
        if (rank[a] == rank[b]) {
            rank[a]++;
        }
    };
    for (size_t i = 0; i < _types.size(); ++i) {
        for (size_t j = 0; j < i; ++j) {
            if (ufFind(i) == ufFind(j)) {
                continue;
            }
            if (equalBuilderTypes(i, j)) {
                ufUnion(i, j);
            }
        }
    }
    // map union representative -> new deduped type id
    vector<size_t> repToNewId(_types.size(), StructuredBufferId::Invalid);
    vector<StructuredBufferId> builderToNewId(_types.size(), StructuredBufferId{StructuredBufferId::Invalid});
    vector<size_t> newIdToRepBuilder;
    newIdToRepBuilder.reserve(_types.size());
    for (size_t i = 0; i < _types.size(); ++i) {
        size_t rep = ufFind(i);
        if (repToNewId[rep] == StructuredBufferId::Invalid) {
            repToNewId[rep] = newIdToRepBuilder.size();
            newIdToRepBuilder.push_back(rep);
        }
        builderToNewId[i] = StructuredBufferId{repToNewId[rep]};
    }
    storage._types.clear();
    storage._types.reserve(newIdToRepBuilder.size());
    for (size_t newId = 0; newId < newIdToRepBuilder.size(); ++newId) {
        size_t repBuilder = newIdToRepBuilder[newId];
        const auto& t = _types[repBuilder];

        storage._types.emplace_back(t.Name, StructuredBufferId{newId});
        auto& newType = storage._types.back();
        newType._size = t.SizeInBytes;

        const auto& members = canonMembers[repBuilder];
        newType._members.reserve(members.size());
        for (const auto& m : members) {
            StructuredBufferVariable v{string{m.Name}, builderToNewId[m.TypeIndex]};
            v._offset = m.Offset;
            v._arraySize = m.ArraySize;
            newType._members.push_back(std::move(v));
        }
    }
    // Roots and buffer layout
    size_t totalSize = 0;
    storage._rootVarIds.reserve(_roots.size());
    for (const auto& r : _roots) {
        if (static_cast<size_t>(r.TypeIndex) >= _types.size()) {
            return std::nullopt;
        }
        const StructuredBufferId typeId = builderToNewId[r.TypeIndex];
        if (_align != 0) {
            totalSize = static_cast<size_t>(radray::Align(static_cast<uint64_t>(totalSize), static_cast<uint64_t>(_align)));
        }
        StructuredBufferVariable rootVar{r.Name, typeId};
        rootVar._offset = totalSize;
        storage._rootVarIds.push_back(std::move(rootVar));
        totalSize += storage._types[typeId]._size;
    }
    if (_align != 0) {
        totalSize = static_cast<size_t>(radray::Align(static_cast<uint64_t>(totalSize), static_cast<uint64_t>(_align)));
    }
    storage._buffer.resize(totalSize);
    BuildMember(storage);
    return storage;
}

StructuredBufferView StructuredBufferStorage::GetVar(std::string_view name) noexcept {
    for (const auto& root : _rootVarIds) {
        if (root._name == name) {
            return StructuredBufferView{this, root._globalId};
        }
    }
    return StructuredBufferView{};
}

void StructuredBufferStorage::WriteData(size_t offset, std::span<const byte> data) noexcept {
    if (data.empty()) {
        return;
    }
    if (offset > _buffer.size() || data.size() > _buffer.size() || offset + data.size() > _buffer.size()) {
        RADRAY_ERR_LOG("StructuredBufferStorage::WriteData out of range offset={} size={} buffer={}", offset, data.size(), _buffer.size());
        return;
    }
    std::memcpy(_buffer.data() + offset, data.data(), data.size());
}

std::span<const byte> StructuredBufferStorage::GetSpan(size_t offset, size_t size) const noexcept {
    if (offset > _buffer.size() || size > _buffer.size() || offset + size > _buffer.size()) {
        return {};
    }
    return std::span<const byte>{_buffer.data() + offset, size};
}

std::span<const byte> StructuredBufferStorage::GetSpan(StructuredBufferId globalId) const noexcept {
    RADRAY_ASSERT(globalId.Value < _globalIndex.size());
    const auto& indexer = _globalIndex[globalId];
    if (indexer.ParentTypeId == StructuredBufferId::Invalid) {
        const auto& rootVar = _rootVarIds[globalId];
        const auto& type = _types[rootVar.GetTypeId()];
        return GetSpan(rootVar._offset, type._size);
    } else {
        const auto& parentType = _types[indexer.ParentTypeId];
        const auto& memberInType = parentType._members[indexer.MemberIndexInType];
        const auto& type = _types[memberInType.GetTypeId()];
        return GetSpan(indexer.GlobalOffset, type._size);
    }
}

StructuredBufferView StructuredBufferView::GetVar(std::string_view name) noexcept {
    RADRAY_ASSERT(this->IsValid());
    const auto& type = GetType();
    for (const auto& mem : type.GetMembers()) {
        if (mem.GetName() == name) {
            return StructuredBufferView{_storage, mem.GetGlobalId()};
        }
    }
    return {};
}

size_t StructuredBufferView::GetOffset() const noexcept {
    RADRAY_ASSERT(this->IsValid());
    return _storage->_globalIndex[_globalId].GlobalOffset;
}

const StructuredBufferType& StructuredBufferView::GetType() const noexcept {
    RADRAY_ASSERT(this->IsValid());
    const auto& indexer = _storage->_globalIndex[_globalId];
    if (indexer.ParentTypeId == StructuredBufferId::Invalid) {
        return _storage->_types[_storage->_rootVarIds[_globalId].GetTypeId()];
    } else {
        const auto& parentType = _storage->_types[indexer.ParentTypeId];
        const auto& memberInType = parentType.GetMembers()[indexer.MemberIndexInType];
        return _storage->_types[memberInType.GetTypeId().Value];
    }
}

const StructuredBufferVariable& StructuredBufferView::GetSelf() const noexcept {
    const auto& indexer = _storage->_globalIndex[_globalId];
    const auto& parentType = _storage->_types[indexer.ParentTypeId];
    return parentType.GetMembers()[indexer.MemberIndexInType];
}

}  // namespace radray
