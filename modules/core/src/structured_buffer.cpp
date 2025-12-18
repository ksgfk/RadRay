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
    storage._globalIndex.reserve(256);
    struct StackItem {
        StructuredBufferId TypeId;
        size_t GlobalOffset;
        size_t MemberIndexInType;
    };
    stack<StackItem> s;
    for (const auto& root : storage._rootVarIds) {
        s.push({root._typeId, root._offset, StructuredBufferId::Invalid});
    }
    while (!s.empty()) {
        auto item = s.top();
        s.pop();
        StructuredBufferStorage::GlobalVarIndexer idx{};
        idx.TypeId = item.TypeId;
        idx.MemberIndexInType = item.MemberIndexInType;
        idx.GlobalOffset = item.GlobalOffset;
        storage._globalIndex.push_back(idx);
        const auto& type = storage._types[item.TypeId];
        for (size_t mi = 0; mi < type._members.size(); ++mi) {
            const auto& mem = type._members[mi];
            s.push({mem._typeId, item.GlobalOffset + mem._offset, mi});
        }
    }
    storage._globalIndex.shrink_to_fit();
}

std::optional<StructuredBufferStorage> StructuredBufferStorage::Builder::Build() noexcept {
    if (!IsValid()) {
        return std::nullopt;
    }
    StructuredBufferStorage storage{};
    // Phase 1: build the full builder type graph (canonicalized member order)
    struct CanonMemberBuilder {
        std::string_view Name;
        size_t Offset;
        StructuredBufferId TypeIndex;
    };
    vector<vector<CanonMemberBuilder>> canonMembers;
    canonMembers.resize(_types.size());
    for (size_t ti = 0; ti < _types.size(); ++ti) {
        const auto& t = _types[ti];
        auto& out = canonMembers[ti];
        out.reserve(t.Members.size());
        for (const auto& m : t.Members) {
            out.push_back({m.Name, m.Offset, m.TypeIndex});
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
            for (size_t i = 0; i < _globalIndex.size(); ++i) {
                const auto& gi = _globalIndex[i];
                if (gi.MemberIndexInType == StructuredBufferId::Invalid && gi.TypeId == root._typeId && gi.GlobalOffset == root._offset) {
                    return StructuredBufferView{this, StructuredBufferId{i}};
                }
            }
            return StructuredBufferView{};
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

std::span<const byte> StructuredBufferStorage::GetSpan(StructuredBufferId memberId) const noexcept {
    const size_t memberIdValue = static_cast<size_t>(memberId);
    if (memberIdValue >= _globalIndex.size()) {
        return {};
    }
    const auto& gi = _globalIndex[memberIdValue];
    if (static_cast<size_t>(gi.TypeId) >= _types.size()) {
        return {};
    }
    return GetSpan(gi.GlobalOffset, _types[gi.TypeId]._size);
}

StructuredBufferView StructuredBufferView::GetVar(std::string_view name) noexcept {
    if (!IsValid()) {
        return {};
    }
    auto type = GetType();
    if (type == nullptr) {
        return {};
    }
    const size_t baseOffset = GetOffset();
    for (const auto& mem : type->GetMembers()) {
        if (mem.GetName() == name) {
            const size_t childOffset = baseOffset + mem.GetOffset();
            for (size_t i = 0; i < _storage->_globalIndex.size(); ++i) {
                const auto& gi = _storage->_globalIndex[i];
                if (gi.TypeId == mem.GetTypeId() && gi.GlobalOffset == childOffset) {
                    return StructuredBufferView{_storage, StructuredBufferId{i}};
                }
            }
            return {};
        }
    }
    return {};
}

size_t StructuredBufferView::GetOffset() const noexcept {
    const size_t memberIdValue = static_cast<size_t>(_memberId);
    if (!IsValid() || memberIdValue >= _storage->_globalIndex.size()) {
        return 0;
    }
    return _storage->_globalIndex[memberIdValue].GlobalOffset;
}

Nullable<StructuredBufferType*> StructuredBufferView::GetType() noexcept {
    const size_t memberIdValue = static_cast<size_t>(_memberId);
    if (!IsValid() || memberIdValue >= _storage->_globalIndex.size()) {
        return nullptr;
    }
    const auto typeId = _storage->_globalIndex[memberIdValue].TypeId;
    if (static_cast<size_t>(typeId) >= _storage->_types.size()) {
        return nullptr;
    }
    return &_storage->_types[typeId];
}

Nullable<const StructuredBufferType*> StructuredBufferView::GetType() const noexcept {
    const size_t memberIdValue = static_cast<size_t>(_memberId);
    if (!IsValid() || memberIdValue >= _storage->_globalIndex.size()) {
        return nullptr;
    }
    const auto typeId = _storage->_globalIndex[memberIdValue].TypeId;
    if (static_cast<size_t>(typeId) >= _storage->_types.size()) {
        return nullptr;
    }
    return &_storage->_types[typeId];
}

}  // namespace radray
