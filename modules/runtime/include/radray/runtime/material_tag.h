#pragma once

// 材质标签所有权与渲染边界见 docs/architecture/runtime.md。

#include <optional>
#include <span>
#include <string_view>

#include <radray/inline_vector.h>
#include <radray/nullable.h>
#include <radray/types.h>

namespace radray {

// ID 仅属于一张表，不是持久标识符；零表示无效。
class MaterialTagId {
public:
    uint32_t Value{0};

    friend bool operator==(MaterialTagId, MaterialTagId) = default;
};

class MaterialTag {
public:
    MaterialTagId Key;
    MaterialTagId Value;

    friend bool operator==(MaterialTag, MaterialTag) = default;
};

// 调用方须串行组织访问，并在使用期间保持表存活、地址稳定。
class MaterialTagTable {
public:
    MaterialTagTable() noexcept = default;
    MaterialTagTable(const MaterialTagTable&) = delete;
    MaterialTagTable& operator=(const MaterialTagTable&) = delete;
    MaterialTagTable(MaterialTagTable&&) = delete;
    MaterialTagTable& operator=(MaterialTagTable&&) = delete;

    // 区分大小写，拒绝空文本；继续登记不会使已有 ID 失效。
    std::optional<MaterialTagId> Intern(std::string_view text);
    std::optional<MaterialTagId> Find(std::string_view text) const noexcept;
    // 返回的视图不得跨 Intern 调用保存。
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

    // ID 必须来自绑定的表；已有键的值会被原位替换。
    // 集合未绑定表，或 ID 为零、越界时拒绝操作，集合保持不变。
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
