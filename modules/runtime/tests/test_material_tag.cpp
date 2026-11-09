#include <gtest/gtest.h>

#include <limits>

#include <radray/logger.h>
#include <radray/runtime/material_tag.h>

namespace radray {
namespace {

TEST(MaterialTagTableTest, InternReturnsSameIdForSameText) {
    MaterialTagTable table;
    const auto first = table.Intern("Surface");
    ASSERT_TRUE(first);
    EXPECT_EQ(table.Intern("Surface"), first);
    EXPECT_EQ(table.Find("Surface"), first);
    EXPECT_EQ(table.GetText(*first), "Surface");
}

TEST(MaterialTagTableTest, TextIsCaseSensitive) {
    MaterialTagTable table;
    const auto upper = table.Intern("Opaque");
    const auto lower = table.Intern("opaque");
    ASSERT_TRUE(upper && lower);
    EXPECT_NE(*upper, *lower);
}

TEST(MaterialTagTableTest, RejectsEmptyText) {
    MaterialTagTable table;
    EXPECT_FALSE(table.Intern(""));
    EXPECT_FALSE(table.Find(""));
}

TEST(MaterialTagTableTest, GetTextRejectsZeroAndUnknownIds) {
    MaterialTagTable table;
    ASSERT_TRUE(table.Intern("Surface"));
    EXPECT_FALSE(table.GetText({0}));
    EXPECT_FALSE(table.GetText({2}));
    EXPECT_FALSE(table.GetText({std::numeric_limits<uint32_t>::max()}));
}

TEST(MaterialTagTableTest, IdsSurviveTableGrowth) {
    MaterialTagTable table;
    vector<MaterialTagId> ids;
    for (int i = 0; i < 64; ++i) {
        const auto id = table.Intern(radray::format("tag-{:02}-long-enough-to-grow-the-text-pool", 63 - i));
        ASSERT_TRUE(id);
        ids.push_back(*id);
    }
    for (int i = 0; i < 64; ++i) {
        const auto text = radray::format("tag-{:02}-long-enough-to-grow-the-text-pool", 63 - i);
        EXPECT_EQ(table.GetText(ids[i]), text);
        EXPECT_EQ(table.Find(text), ids[i]);
    }
}

TEST(MaterialTagTableTest, InternAcceptsViewOfItsOwnText) {
    MaterialTagTable table;
    const auto whole = table.Intern("SurfaceOpaque");
    ASSERT_TRUE(whole);
    const auto suffix = table.Intern(table.GetText(*whole)->substr(7));
    ASSERT_TRUE(suffix);
    EXPECT_EQ(table.GetText(*suffix), "Opaque");
    EXPECT_EQ(table.GetText(*whole), "SurfaceOpaque");
}

class MaterialTagSetTest : public testing::Test {
protected:
    MaterialTagId Tag(std::string_view text) { return *Table.Intern(text); }

    MaterialTagTable Table;
};

TEST_F(MaterialTagSetTest, DefaultSetIsUnboundAndRejectsWrites) {
    const auto key = Tag("Surface");
    const auto value = Tag("Opaque");
    MaterialTagSet tags;
    EXPECT_FALSE(tags.GetTable());
    EXPECT_FALSE(tags.Set(key, value));
    EXPECT_TRUE(tags.GetTags().empty());
}

TEST_F(MaterialTagSetTest, FindReturnsValueStoredForKey) {
    const auto surface = Tag("Surface");
    const auto opaque = Tag("Opaque");
    MaterialTagSet tags{Table};
    ASSERT_TRUE(tags.Set(surface, opaque));
    EXPECT_EQ(tags.Find(surface), opaque);
    EXPECT_TRUE(tags.Matches(surface, opaque));
    EXPECT_FALSE(tags.Find(opaque));
}

TEST_F(MaterialTagSetTest, SetReplacesExistingKey) {
    const auto surface = Tag("Surface");
    const auto opaque = Tag("Opaque");
    const auto transparent = Tag("Transparent");
    MaterialTagSet tags{Table};
    ASSERT_TRUE(tags.Set(surface, opaque));
    ASSERT_TRUE(tags.Set(surface, transparent));
    EXPECT_EQ(tags.GetTags().size(), 1u);
    EXPECT_TRUE(tags.Matches(surface, transparent));
    EXPECT_FALSE(tags.Matches(surface, opaque));
}

TEST_F(MaterialTagSetTest, SetRejectsZeroAndUnknownIdsWithoutChange) {
    const auto surface = Tag("Surface");
    const auto opaque = Tag("Opaque");
    MaterialTagSet tags{Table};
    ASSERT_TRUE(tags.Set(surface, opaque));
    EXPECT_FALSE(tags.Set({}, opaque));
    EXPECT_FALSE(tags.Set(surface, {}));
    EXPECT_FALSE(tags.Set(surface, {99}));
    EXPECT_EQ(tags.GetTags().size(), 1u);
    EXPECT_TRUE(tags.Matches(surface, opaque));
}

TEST_F(MaterialTagSetTest, CopyBeyondInlineCapacityOwnsIndependentTags) {
    const auto on = Tag("On");
    const auto off = Tag("Off");
    MaterialTagSet original{Table};
    vector<MaterialTagId> keys;
    for (int i = 0; i < 6; ++i) {
        keys.push_back(Tag(radray::format("Key{}", i)));
        ASSERT_TRUE(original.Set(keys.back(), on));
    }
    auto copy = original;
    ASSERT_TRUE(copy.Set(keys[5], off));
    EXPECT_EQ(copy.GetTable().Get(), &Table);
    EXPECT_TRUE(original.Matches(keys[5], on));
    EXPECT_TRUE(copy.Matches(keys[5], off));
    EXPECT_TRUE(copy.Matches(keys[0], on));
}

}  // 匿名命名空间
}  // namespace radray
