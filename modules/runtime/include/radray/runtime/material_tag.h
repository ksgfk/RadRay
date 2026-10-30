#pragma once

#include <optional>
#include <span>
#include <string_view>

#include <radray/inline_vector.h>
#include <radray/nullable.h>
#include <radray/types.h>

namespace radray {

// Material tag ownership and rendering boundaries: docs/architecture/runtime.md.
// IDs belong to one table, are not persistent identifiers, and use zero for invalid.
struct MaterialTagId {
    uint32_t Value{0};

    friend bool operator==(MaterialTagId, MaterialTagId) = default;
};

struct MaterialTag {
    MaterialTagId Key;
    MaterialTagId Value;

    friend bool operator==(MaterialTag, MaterialTag) = default;
};

// Caller serializes access and keeps the table alive at a stable address for its users.
class MaterialTagTable {
public:
    MaterialTagTable() noexcept = default;
    MaterialTagTable(const MaterialTagTable&) = delete;
    MaterialTagTable& operator=(const MaterialTagTable&) = delete;
    MaterialTagTable(MaterialTagTable&&) = delete;
    MaterialTagTable& operator=(MaterialTagTable&&) = delete;

    // Case-sensitive; empty text is rejected. Existing IDs survive further registration.
    std::optional<MaterialTagId> Intern(std::string_view text);
    std::optional<MaterialTagId> Find(std::string_view text) const noexcept;
    // Returned views must not be retained across Intern calls.
    std::optional<std::string_view> GetText(MaterialTagId id) const noexcept;

private:
    struct TextRange {
        size_t Offset;
        size_t Size;
    };

    size_t LowerBound(std::string_view text) const noexcept;
    std::string_view TextAt(MaterialTagId id) const noexcept;

    vector<char> _text;
    vector<TextRange> _ranges;
    vector<MaterialTagId> _sortedIds;
};

class MaterialTagSet {
public:
    MaterialTagSet() noexcept = default;
    explicit MaterialTagSet(const MaterialTagTable& table) noexcept;

    // IDs must originate from the bound table. Replaces an existing key in place.
    // Unbound sets and zero/out-of-range IDs are rejected without changing the set.
    bool Set(MaterialTagId key, MaterialTagId value);
    std::optional<MaterialTagId> Find(MaterialTagId key) const noexcept;
    bool Matches(MaterialTagId key, MaterialTagId value) const noexcept;
    Nullable<const MaterialTagTable*> GetTable() const noexcept;
    std::span<const MaterialTag> GetTags() const noexcept;

private:
    Nullable<const MaterialTagTable*> _table{nullptr};
    InlineVector<MaterialTag, 4> _tags;
};

}  // namespace radray
