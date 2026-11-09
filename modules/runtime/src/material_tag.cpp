#include <radray/runtime/material_tag.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace radray {

std::optional<MaterialTagId> MaterialTagTable::Intern(std::string_view text) {
    if (text.empty()) return std::nullopt;
    const size_t position = LowerBound(text);
    if (position < _sortedIds.size() && TextAt(_sortedIds[position]) == text) return _sortedIds[position];
    if (_ranges.size() >= std::numeric_limits<uint32_t>::max() || text.size() > _text.max_size() - _text.size()) {
        RADRAY_ERR_LOG("Material tag table capacity exceeded");
        return std::nullopt;
    }

    const size_t offset = _text.size();
    const size_t size = text.size();
    const size_t required = offset + size;
    if (required > _text.capacity()) {
        // 复制期间必须保持旧文本池存活；text 可能是 GetText 返回的子串。
        vector<char> replacement;
        const size_t growth = std::min(_text.capacity(), _text.max_size() - _text.capacity());
        replacement.reserve(std::max(required, _text.capacity() + growth));
        replacement.resize(required);
        if (offset) std::memcpy(replacement.data(), _text.data(), offset);
        std::memcpy(replacement.data() + offset, text.data(), size);
        _text.swap(replacement);
    } else {
        _text.resize(required);
        std::memmove(_text.data() + offset, text.data(), size);
    }
    const MaterialTagId id{static_cast<uint32_t>(_ranges.size() + 1)};
    _ranges.push_back({offset, size});
    _sortedIds.insert(_sortedIds.begin() + static_cast<std::ptrdiff_t>(position), id);
    return id;
}

std::optional<MaterialTagId> MaterialTagTable::Find(std::string_view text) const noexcept {
    if (text.empty()) return std::nullopt;
    const size_t position = LowerBound(text);
    if (position < _sortedIds.size() && TextAt(_sortedIds[position]) == text) return _sortedIds[position];
    return std::nullopt;
}

std::optional<std::string_view> MaterialTagTable::GetText(MaterialTagId id) const noexcept {
    if (!id.Value || id.Value > _ranges.size()) return std::nullopt;
    return TextAt(id);
}

size_t MaterialTagTable::LowerBound(std::string_view text) const noexcept {
    const auto found = std::lower_bound(_sortedIds.begin(), _sortedIds.end(), text,
                                        [this](MaterialTagId id, std::string_view value) { return TextAt(id) < value; });
    return static_cast<size_t>(found - _sortedIds.begin());
}

std::string_view MaterialTagTable::TextAt(MaterialTagId id) const noexcept {
    const auto& range = _ranges[id.Value - 1];
    return {_text.data() + range.Offset, range.Size};
}

MaterialTagSet::MaterialTagSet(const MaterialTagTable& table) noexcept : _table(&table) {}

bool MaterialTagSet::Set(MaterialTagId key, MaterialTagId value) {
    if (!_table || !_table->GetText(key) || !_table->GetText(value)) return false;
    for (auto& tag : _tags) {
        if (tag.Key == key) {
            tag.Value = value;
            return true;
        }
    }
    _tags.push_back({key, value});
    return true;
}

std::optional<MaterialTagId> MaterialTagSet::Find(MaterialTagId key) const noexcept {
    for (const auto& tag : _tags) {
        if (tag.Key == key) return tag.Value;
    }
    return std::nullopt;
}

bool MaterialTagSet::Matches(MaterialTagId key, MaterialTagId value) const noexcept {
    const auto found = Find(key);
    return found && *found == value;
}

Nullable<const MaterialTagTable*> MaterialTagSet::GetTable() const noexcept { return _table; }

std::span<const MaterialTag> MaterialTagSet::GetTags() const noexcept { return {_tags.data(), _tags.size()}; }

}  // namespace radray
