#include <radray/render/shader_cbuffer_helper.h>

#include <algorithm>
#include <bit>
#include <utility>
#include <cstring>

#include <radray/basic_math.h>

namespace radray::render {

namespace {

constexpr size_t kInvalid = ShaderCBufferStorageInvalidId;

}  // namespace

size_t ShaderCBufferStorage::Builder::AddType(std::string_view name, size_t size) noexcept {
    Type t{};
    t.Name = string{name};
    t.SizeInBytes = size;
    _types.push_back(std::move(t));
    return _types.size() - 1;
}

void ShaderCBufferStorage::Builder::AddMemberForType(size_t targetType, size_t memberType, std::string_view name, size_t offset) noexcept {
    if (targetType >= _types.size() || memberType >= _types.size()) {
        RADRAY_ERR_LOG("ShaderCBufferStorage::Builder::AddMemberForType invalid index target={} member={}", targetType, memberType);
        return;
    }
    Member m{};
    m.Name = string{name};
    m.Offset = offset;
    m.TypeIndex = memberType;
    _types[targetType].Members.push_back(std::move(m));
}

size_t ShaderCBufferStorage::Builder::AddRoot(std::string_view name, size_t typeIndex) noexcept {
    if (typeIndex >= _types.size()) {
        RADRAY_ERR_LOG("ShaderCBufferStorage::Builder::AddRoot invalid typeIndex={}", typeIndex);
        return Invalid;
    }
    Root r{};
    r.Name = string{name};
    r.TypeIndex = typeIndex;
    _roots.push_back(std::move(r));
    return _roots.size() - 1;
}

void ShaderCBufferStorage::Builder::SetAlignment(size_t align) noexcept {
    _align = align;
}

bool ShaderCBufferStorage::Builder::IsValid() const noexcept {
    if (_align != 0 && (_align & (_align - 1)) != 0) {
        return false;
    }
    for (size_t ti = 0; ti < _types.size(); ++ti) {
        const auto& t = _types[ti];
        for (const auto& m : t.Members) {
            if (m.TypeIndex >= _types.size()) {
                return false;
            }
            const auto& mt = _types[m.TypeIndex];
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
        if (r.TypeIndex >= _types.size()) {
            return false;
        }
    }
    return true;
}

void ShaderCBufferStorage::Builder::BuildMember(ShaderCBufferStorage& storage) noexcept {
    storage._globalIndex.clear();
    storage._globalIndex.reserve(256);
    struct StackItem {
        size_t TypeId;
        size_t GlobalOffset;
        size_t MemberIndexInType;
    };
    stack<StackItem> s;
    for (const auto& root : storage._rootVarIds) {
        s.push({root._typeId, root._offset, kInvalid});
    }
    while (!s.empty()) {
        auto item = s.top();
        s.pop();
        ShaderCBufferStorage::GlobalVarIndexer idx{};
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

std::optional<ShaderCBufferStorage> ShaderCBufferStorage::Builder::Build() noexcept {
    if (!IsValid()) {
        return std::nullopt;
    }
    ShaderCBufferStorage storage{};
    // Phase 1: build the full builder type graph (canonicalized member order)
    struct CanonMemberBuilder {
        std::string_view Name;
        size_t Offset;
        size_t TypeIndex;
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
    auto equalBuilderTypes = [&](auto&& self, size_t a, size_t b) -> bool {
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
            if (!self(self, ma[i].TypeIndex, mb[i].TypeIndex)) {
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
    auto ufFind = [&](auto&& self, size_t x) -> size_t {
        if (parent[x] == x) {
            return x;
        }
        parent[x] = self(self, parent[x]);
        return parent[x];
    };
    auto ufUnion = [&](size_t a, size_t b) {
        a = ufFind(ufFind, a);
        b = ufFind(ufFind, b);
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
            if (ufFind(ufFind, i) == ufFind(ufFind, j)) {
                continue;
            }
            if (equalBuilderTypes(equalBuilderTypes, i, j)) {
                ufUnion(i, j);
            }
        }
    }
    // map union representative -> new deduped type id
    vector<size_t> repToNewId(_types.size(), kInvalid);
    vector<size_t> builderToNewId(_types.size(), kInvalid);
    vector<size_t> newIdToRepBuilder;
    newIdToRepBuilder.reserve(_types.size());
    for (size_t i = 0; i < _types.size(); ++i) {
        size_t rep = ufFind(ufFind, i);
        if (repToNewId[rep] == kInvalid) {
            repToNewId[rep] = newIdToRepBuilder.size();
            newIdToRepBuilder.push_back(rep);
        }
        builderToNewId[i] = repToNewId[rep];
    }
    storage._types.clear();
    storage._types.reserve(newIdToRepBuilder.size());
    for (size_t newId = 0; newId < newIdToRepBuilder.size(); ++newId) {
        size_t repBuilder = newIdToRepBuilder[newId];
        const auto& t = _types[repBuilder];

        storage._types.emplace_back(t.Name, newId);
        auto& newType = storage._types.back();
        newType._size = t.SizeInBytes;

        const auto& members = canonMembers[repBuilder];
        newType._members.reserve(members.size());
        for (const auto& m : members) {
            ShaderCBufferVariable v{string{m.Name}, builderToNewId[m.TypeIndex]};
            v._offset = m.Offset;
            newType._members.push_back(std::move(v));
        }
    }
    // Roots and buffer layout
    size_t totalSize = 0;
    storage._rootVarIds.reserve(_roots.size());
    for (const auto& r : _roots) {
        if (r.TypeIndex >= _types.size()) {
            return std::nullopt;
        }
        const size_t typeId = builderToNewId[r.TypeIndex];
        if (_align != 0) {
            totalSize = static_cast<size_t>(radray::Align(static_cast<uint64_t>(totalSize), static_cast<uint64_t>(_align)));
        }
        ShaderCBufferVariable rootVar{r.Name, typeId};
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

ShaderCBufferView ShaderCBufferStorage::GetVar(std::string_view name) noexcept {
    for (const auto& root : _rootVarIds) {
        if (root._name == name) {
            for (size_t i = 0; i < _globalIndex.size(); ++i) {
                const auto& gi = _globalIndex[i];
                if (gi.MemberIndexInType == kInvalid && gi.TypeId == root._typeId && gi.GlobalOffset == root._offset) {
                    return ShaderCBufferView{this, i};
                }
            }
            return ShaderCBufferView{};
        }
    }
    return ShaderCBufferView{};
}

void ShaderCBufferStorage::WriteData(size_t offset, std::span<const byte> data) noexcept {
    if (data.empty()) {
        return;
    }
    if (offset > _buffer.size() || data.size() > _buffer.size() || offset + data.size() > _buffer.size()) {
        RADRAY_ERR_LOG("ShaderCBufferStorage::WriteData out of range offset={} size={} buffer={}", offset, data.size(), _buffer.size());
        return;
    }
    std::memcpy(_buffer.data() + offset, data.data(), data.size());
}

std::span<const byte> ShaderCBufferStorage::GetSpan(size_t offset, size_t size) const noexcept {
    if (offset > _buffer.size() || size > _buffer.size() || offset + size > _buffer.size()) {
        return {};
    }
    return std::span<const byte>{_buffer.data() + offset, size};
}

std::span<const byte> ShaderCBufferStorage::GetSpan(size_t memberId) const noexcept {
    if (memberId >= _globalIndex.size()) {
        return {};
    }
    const auto& gi = _globalIndex[memberId];
    if (gi.TypeId >= _types.size()) {
        return {};
    }
    return GetSpan(gi.GlobalOffset, _types[gi.TypeId]._size);
}

ShaderCBufferView ShaderCBufferView::GetVar(std::string_view name) noexcept {
    if (!IsValid()) {
        return {};
    }
    auto* type = GetType();
    if (type == nullptr) {
        return {};
    }

    const size_t baseOffset = GetOffset();
    for (size_t mi = 0; mi < type->_members.size(); ++mi) {
        const auto& mem = type->_members[mi];
        if (mem._name == name) {
            const size_t childOffset = baseOffset + mem._offset;
            for (size_t i = 0; i < _storage->_globalIndex.size(); ++i) {
                const auto& gi = _storage->_globalIndex[i];
                if (gi.TypeId == mem._typeId && gi.GlobalOffset == childOffset) {
                    return ShaderCBufferView{_storage, i};
                }
            }
            return {};
        }
    }
    return {};
}

size_t ShaderCBufferView::GetOffset() const noexcept {
    if (!IsValid() || _memberId >= _storage->_globalIndex.size()) {
        return 0;
    }
    return _storage->_globalIndex[_memberId].GlobalOffset;
}

ShaderCBufferType* ShaderCBufferView::GetType() noexcept {
    if (!IsValid() || _memberId >= _storage->_globalIndex.size()) {
        return nullptr;
    }
    const auto typeId = _storage->_globalIndex[_memberId].TypeId;
    if (typeId >= _storage->_types.size()) {
        return nullptr;
    }
    return &_storage->_types[typeId];
}

const ShaderCBufferType* ShaderCBufferView::GetType() const noexcept {
    if (!IsValid() || _memberId >= _storage->_globalIndex.size()) {
        return nullptr;
    }
    const auto typeId = _storage->_globalIndex[_memberId].TypeId;
    if (typeId >= _storage->_types.size()) {
        return nullptr;
    }
    return &_storage->_types[typeId];
}

std::optional<ShaderCBufferStorage> CreateCBufferStorage(const MergedHlslShaderDesc& desc) noexcept {
    ShaderCBufferStorage::Builder builder{};
    builder.SetAlignment(0);
    auto createType = [&](size_t parent, size_t bdType, size_t size) {
        struct TypeCreateCtx {
            size_t par, bd, s;
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
                }
                auto childBdIdx = builder.AddType(memberType.Name, sizeInBytes);
                builder.AddMemberForType(ctx.bd, childBdIdx, member.Name, memberType.Offset);
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
        for (size_t i = 0; i < cb.Variables.size(); i++) {
            size_t varIdx = cb.Variables[i];
            RADRAY_ASSERT(varIdx < desc.Variables.size());
            const auto& var = desc.Variables[varIdx];
            RADRAY_ASSERT(var.Type < desc.Types.size());
            const auto& type = desc.Types[var.Type];
            size_t sizeInBytes = cb.IsViewInHlsl ? cb.Size : var.Size;
            size_t bdTypeIdx = builder.AddType(type.Name, sizeInBytes);
            builder.AddRoot(var.Name, bdTypeIdx);
            createType(var.Type, bdTypeIdx, sizeInBytes);
        }
    }
    return builder.Build();
}

std::optional<ShaderCBufferStorage> CreateCBufferStorage(const SpirvShaderDesc& desc) noexcept {
    ShaderCBufferStorage::Builder builder{};
    builder.SetAlignment(0);
    auto createType = [&](size_t parent, size_t bdType, size_t size) {
        struct TypeCreateCtx {
            size_t par, bd, s;
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
                size_t sizeInBytes = memberType.Size;
                auto childBdIdx = builder.AddType(memberType.Name, sizeInBytes);
                builder.AddMemberForType(ctx.bd, childBdIdx, member.Name, member.Offset);
                s.push({member.TypeIndex, childBdIdx, sizeInBytes});
            }
        }
    };

    for (const auto& res : desc.PushConstants) {
        RADRAY_ASSERT(res.TypeIndex < desc.Types.size());
        auto type = desc.Types[res.TypeIndex];
        size_t bdTypeIdx = builder.AddType(type.Name, type.Size);
        builder.AddRoot(res.Name, bdTypeIdx);
        createType(res.TypeIndex, bdTypeIdx, type.Size);
    }
    for (const auto& res : desc.ResourceBindings) {
        if (res.Kind != SpirvResourceKind::UniformBuffer) {
            continue;
        }
        RADRAY_ASSERT(res.TypeIndex < desc.Types.size());
        auto type = desc.Types[res.TypeIndex];
        if (res.IsViewInHlsl) {
            size_t bdTypeIdx = builder.AddType(type.Name, type.Size);
            builder.AddRoot(res.Name, bdTypeIdx);
            createType(res.TypeIndex, bdTypeIdx, type.Size);
        } else {
            for (auto member : type.Members) {
                RADRAY_ASSERT(member.TypeIndex < desc.Types.size());
                auto memberType = desc.Types[member.TypeIndex];
                size_t bdTypeIdx = builder.AddType(memberType.Name, memberType.Size);
                builder.AddRoot(member.Name, bdTypeIdx);
                createType(member.TypeIndex, bdTypeIdx, memberType.Size);
            }
        }
    }
    return builder.Build();
}

}  // namespace radray::render
