#include <radray/structured_buffer.h>

namespace radray {
namespace {

struct CanonMemberBuilder {
    std::string_view Name;
    size_t Offset;
    StructuredBufferId TypeIndex;
    size_t ArraySize{0};
};

enum class VisitState : uint8_t {
    Unvisited = 0,
    Visiting = 1,
    Done = 2,
};

enum class EqState : uint8_t {
    Unknown = 0,
    InProgress = 1,
    Equal = 2,
    NotEqual = 3,
};

static bool HasByValueCycleDfs(size_t typeIndex, const vector<StructuredBufferStorage::Builder::Type>& types, vector<VisitState>& visitState) noexcept {
    VisitState& state = visitState[typeIndex];
    if (state == VisitState::Visiting) {
        return true;
    }
    if (state == VisitState::Done) {
        return false;
    }
    state = VisitState::Visiting;
    const auto& t = types[typeIndex];
    for (const auto& m : t.Members) {
        const size_t childType = static_cast<size_t>(m.TypeIndex);
        if (childType >= types.size()) {
            continue;
        }
        if (HasByValueCycleDfs(childType, types, visitState)) {
            return true;
        }
    }
    state = VisitState::Done;
    return false;
}

static bool HasByValueCycle(const vector<StructuredBufferStorage::Builder::Type>& types) noexcept {
    vector<VisitState> visitState(types.size(), VisitState::Unvisited);
    for (size_t i = 0; i < types.size(); ++i) {
        if (visitState[i] == VisitState::Unvisited && HasByValueCycleDfs(i, types, visitState)) {
            return true;
        }
    }
    return false;
}

static bool EqualBuilderTypes(
    size_t a,
    size_t b,
    const vector<StructuredBufferStorage::Builder::Type>& types,
    const vector<vector<CanonMemberBuilder>>& canonMembers,
    vector<EqState>& eqMemo,
    size_t nTypes) noexcept {
    if (a == b) {
        return true;
    }
    if (a >= types.size() || b >= types.size()) {
        return false;
    }

    const size_t x = std::min(a, b);
    const size_t y = std::max(a, b);
    EqState& state = eqMemo[x * nTypes + y];
    if (state == EqState::Equal) {
        return true;
    }
    if (state == EqState::NotEqual) {
        return false;
    }
    if (state == EqState::InProgress) {
        // A re-entry here indicates recursive equivalence expansion.
        // Treat as non-equal to avoid accidental type merging.
        RADRAY_ASSERT(false);
        state = EqState::NotEqual;
        return false;
    }

    state = EqState::InProgress;
    const auto& ta = types[a];
    const auto& tb = types[b];
    if (ta.Name != tb.Name || ta.SizeInBytes != tb.SizeInBytes) {
        state = EqState::NotEqual;
        return false;
    }

    const auto& ma = canonMembers[a];
    const auto& mb = canonMembers[b];
    if (ma.size() != mb.size()) {
        state = EqState::NotEqual;
        return false;
    }

    for (size_t i = 0; i < ma.size(); ++i) {
        if (ma[i].Offset != mb[i].Offset || ma[i].Name != mb[i].Name || ma[i].ArraySize != mb[i].ArraySize) {
            state = EqState::NotEqual;
            return false;
        }
        if (!EqualBuilderTypes(ma[i].TypeIndex, mb[i].TypeIndex, types, canonMembers, eqMemo, nTypes)) {
            state = EqState::NotEqual;
            return false;
        }
    }

    state = EqState::Equal;
    return true;
}

static size_t UfFind(size_t x, vector<size_t>& parent) noexcept {
    while (parent[x] != x) {
        parent[x] = parent[parent[x]];
        x = parent[x];
    }
    return x;
}

static void UfUnion(size_t a, size_t b, vector<size_t>& parent, vector<uint32_t>& rank) noexcept {
    a = UfFind(a, parent);
    b = UfFind(b, parent);
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
}

}  // namespace

StructuredBufferId StructuredBufferStorage::Builder::AddType(std::string_view name, size_t size) noexcept {
    Type t{};
    t.Name = string{name};
    t.SizeInBytes = size;
    _types.emplace_back(std::move(t));
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
    _types[targetTypeIndex].Members.emplace_back(std::move(m));
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
    _types[targetTypeIndex].Members.emplace_back(std::move(m));
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
    _roots.emplace_back(std::move(r));
    return StructuredBufferId{_roots.size() - 1};
}

StructuredBufferId StructuredBufferStorage::Builder::AddRoot(std::string_view name, StructuredBufferId typeIndex, size_t arraySize) noexcept {
    const size_t typeIndexValue = static_cast<size_t>(typeIndex);
    if (typeIndexValue >= _types.size()) {
        RADRAY_ERR_LOG("StructuredBufferStorage::Builder::AddRoot invalid typeIndex={}", typeIndexValue);
        return Invalid;
    }
    Root r{};
    r.Name = string{name};
    r.TypeIndex = typeIndex;
    r.ArraySize = arraySize;
    _roots.emplace_back(std::move(r));
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
            size_t memberSizeInBytes = mt.SizeInBytes;
            if (m.ArraySize != 0) {
                memberSizeInBytes = mt.SizeInBytes * m.ArraySize;
            }
            if (memberSizeInBytes > t.SizeInBytes) {
                return false;
            }
            if (memberSizeInBytes > t.SizeInBytes - m.Offset) {
                return false;
            }
        }
    }
    for (const auto& r : _roots) {
        if (r.TypeIndex.Value >= _types.size()) {
            return false;
        }
    }
    if (HasByValueCycle(_types)) {
        return false;
    }
    return true;
}

void StructuredBufferStorage::Builder::BuildMember(StructuredBufferStorage& storage) const noexcept {
    storage._globalIndex.clear();
    struct StackItem {
        size_t GlobalOffset;
        StructuredBufferId ParentTypeId;
        size_t MemberIndexInType;
    };
    stack<StackItem> s;
    for (size_t i = 0; i < storage._rootVarIds.size(); i++) {
        const auto& root = storage._rootVarIds[i];
        s.push({root._offset, StructuredBufferId::Invalid(), i});
    }
    while (!s.empty()) {
        auto item = s.top();
        s.pop();
        StructuredBufferStorage::GlobalVarIndexer idx{};
        idx.MemberIndexInType = item.MemberIndexInType;
        idx.GlobalOffset = item.GlobalOffset;
        idx.ParentTypeId = item.ParentTypeId;
        StructuredBufferId newGlobalId = storage._globalIndex.size();
        storage._globalIndex.emplace_back(idx);
        auto& var = (idx.ParentTypeId == StructuredBufferId::Invalid())
                        ? storage._rootVarIds[idx.MemberIndexInType]
                        : storage._types[idx.ParentTypeId]._members[idx.MemberIndexInType];
        var._globalId = newGlobalId;
        const auto& type = storage._types[var.GetTypeId()];
        for (size_t mi = 0; mi < type._members.size(); mi++) {
            const auto& mem = type._members[mi];
            s.push({item.GlobalOffset + mem._offset, type._id, mi});
        }
    }
    storage._globalIndex.shrink_to_fit();
}

std::optional<StructuredBufferStorage> StructuredBufferStorage::Builder::Build() const noexcept {
    if (!IsValid()) {
        RADRAY_ERR_LOG("invalid StructuredBufferStorage::Builder state");
        return std::nullopt;
    }
    StructuredBufferStorage storage{};
    // 构建类型图 (canonicalized member order)
    vector<vector<CanonMemberBuilder>> canonMembers;
    canonMembers.resize(_types.size());
    for (size_t ti = 0; ti < _types.size(); ++ti) {
        const auto& t = _types[ti];
        auto& out = canonMembers[ti];
        out.reserve(t.Members.size());
        for (const auto& m : t.Members) {
            out.emplace_back(CanonMemberBuilder{m.Name, m.Offset, m.TypeIndex, m.ArraySize});
        }
        std::sort(out.begin(), out.end(), [](const CanonMemberBuilder& a, const CanonMemberBuilder& b) {
            if (a.Offset != b.Offset) {
                return a.Offset < b.Offset;
            }
            if (a.Name != b.Name) {
                return a.Name < b.Name;
            }
            if (a.TypeIndex != b.TypeIndex) {
                return a.TypeIndex < b.TypeIndex;
            }
            return a.ArraySize < b.ArraySize;
        });
    }
    // 通过递归合并等价图节点
    const size_t nTypes = _types.size();
    vector<EqState> eqMemo;
    eqMemo.resize(nTypes * nTypes, EqState::Unknown);
    // 并查集合并节点
    vector<size_t> parent(_types.size());
    vector<uint32_t> rank(_types.size(), 0);
    for (size_t i = 0; i < parent.size(); ++i) {
        parent[i] = i;
    }
    for (size_t i = 0; i < _types.size(); ++i) {
        for (size_t j = 0; j < i; ++j) {
            if (UfFind(i, parent) == UfFind(j, parent)) {
                continue;
            }
            if (EqualBuilderTypes(i, j, _types, canonMembers, eqMemo, nTypes)) {
                UfUnion(i, j, parent, rank);
            }
        }
    }
    vector<size_t> repToNewId(_types.size(), StructuredBufferId::Invalid());
    vector<StructuredBufferId> builderToNewId(_types.size(), StructuredBufferId{StructuredBufferId::Invalid()});
    vector<size_t> newIdToRepBuilder;
    newIdToRepBuilder.reserve(_types.size());
    for (size_t i = 0; i < _types.size(); ++i) {
        size_t rep = UfFind(i, parent);
        if (repToNewId[rep] == StructuredBufferId::Invalid().Value) {
            repToNewId[rep] = newIdToRepBuilder.size();
            newIdToRepBuilder.emplace_back(rep);
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
            newType._members.emplace_back(std::move(v));
        }
    }

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
        auto& rootVar = storage._rootVarIds.emplace_back(StructuredBufferVariable{r.Name, typeId});
        rootVar._offset = totalSize;
        rootVar._arraySize = r.ArraySize;
        const auto& type = storage._types[typeId];
        if (rootVar._arraySize == 0) {
            totalSize += type._size;
        } else {
            totalSize += type._size * rootVar._arraySize;
        }
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

StructuredBufferReadOnlyView StructuredBufferStorage::GetVar(std::string_view name) const noexcept {
    for (const auto& root : _rootVarIds) {
        if (root._name == name) {
            return StructuredBufferReadOnlyView{this, root._globalId};
        }
    }
    return StructuredBufferReadOnlyView{};
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

std::span<const byte> StructuredBufferStorage::GetGlobalSpan(size_t offset, size_t size) const noexcept {
    if (offset > _buffer.size() || size > _buffer.size() || offset + size > _buffer.size()) {
        return {};
    }
    return std::span<const byte>{_buffer.data() + offset, size};
}

std::span<const byte> StructuredBufferStorage::GetSpan(StructuredBufferId globalId) const noexcept {
    RADRAY_ASSERT(globalId.Value < _globalIndex.size());
    const auto& indexer = _globalIndex[globalId];
    const auto& var = (indexer.ParentTypeId == StructuredBufferId::Invalid())
                          ? _rootVarIds[indexer.MemberIndexInType]
                          : _types[indexer.ParentTypeId]._members[indexer.MemberIndexInType];
    const auto& type = _types[var.GetTypeId()];
    return GetGlobalSpan(indexer.GlobalOffset, type._size);
}

std::span<const byte> StructuredBufferStorage::GetSpan(StructuredBufferId globalId, size_t arrayIndex) const noexcept {
    RADRAY_ASSERT(globalId.Value < _globalIndex.size());
    const auto& indexer = _globalIndex[globalId];
    const auto& var = (indexer.ParentTypeId == StructuredBufferId::Invalid())
                          ? _rootVarIds[indexer.MemberIndexInType]
                          : _types[indexer.ParentTypeId]._members[indexer.MemberIndexInType];
    const auto& type = _types[var.GetTypeId()];
    if (var.GetArraySize() == 0 || arrayIndex >= var.GetArraySize()) {
        return this->GetGlobalSpan(indexer.GlobalOffset, type._size);
    }
    size_t offset = indexer.GlobalOffset + arrayIndex * type._size;
    return this->GetGlobalSpan(offset, type._size);
}

template class BasicStructuredBufferView<StructuredBufferStorage>;
template class BasicStructuredBufferView<const StructuredBufferStorage>;

}  // namespace radray
