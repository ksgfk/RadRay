#include <gtest/gtest.h>
#include <radray/inline_vector.h>

#include <memory>
#include <span>
#include <string>

using radray::InlineVector;

TEST(InlineVectorTest, EmptyAndInitializerList) {
    InlineVector<int, 3> empty;
    EXPECT_TRUE(empty.empty());
    EXPECT_EQ(empty.size(), 0u);
    EXPECT_EQ(empty.begin(), empty.end());

    InlineVector<int, 3> three{1, 2, 3};
    EXPECT_EQ(three.size(), 3u);
    EXPECT_EQ(three[0], 1);
    EXPECT_EQ(three.front(), 1);
    EXPECT_EQ(three.back(), 3);
    EXPECT_EQ(three.data(), &three[0]);

    InlineVector<int, 2> spilled{1, 2, 3, 4};
    EXPECT_EQ(spilled.size(), 4u);
    EXPECT_EQ(spilled[0], 1);
    EXPECT_EQ(spilled[3], 4);
}

TEST(InlineVectorTest, SpillAndPopBackPreserveOrder) {
    InlineVector<int, 2> v;
    for (int i = 0; i < 6; ++i) v.push_back(i);
    ASSERT_EQ(v.size(), 6u);
    for (int i = 0; i < 6; ++i) EXPECT_EQ(v[static_cast<size_t>(i)], i);

    // Pop back down across the inline boundary; contents must move back into inline storage.
    v.pop_back();
    v.pop_back();
    v.pop_back();
    v.pop_back();
    ASSERT_EQ(v.size(), 2u);
    EXPECT_EQ(v[0], 0);
    EXPECT_EQ(v[1], 1);
    v.pop_back();
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0], 0);

    // Regrow after shrinking.
    v.push_back(10);
    v.push_back(20);
    ASSERT_EQ(v.size(), 3u);
    EXPECT_EQ(v[0], 0);
    EXPECT_EQ(v[1], 10);
    EXPECT_EQ(v[2], 20);
}

TEST(InlineVectorTest, EmplaceBackAliasingElementSurvivesSpill) {
    InlineVector<std::string, 2> v{"alpha", "beta"};
    v.emplace_back(v[0]);  // argument aliases an inline element that is moved during the spill
    ASSERT_EQ(v.size(), 3u);
    EXPECT_EQ(v[0], "alpha");
    EXPECT_EQ(v[1], "beta");
    EXPECT_EQ(v[2], "alpha");
}

TEST(InlineVectorTest, ClearResetsInlineSlots) {
    InlineVector<std::shared_ptr<int>, 2> v;
    auto tracked = std::make_shared<int>(1);
    v.push_back(tracked);
    EXPECT_EQ(tracked.use_count(), 2);
    v.clear();
    EXPECT_TRUE(v.empty());
    EXPECT_EQ(tracked.use_count(), 1);

    v.push_back(tracked);
    v.push_back(tracked);
    v.push_back(tracked);
    EXPECT_EQ(tracked.use_count(), 4);
    v.clear();
    EXPECT_EQ(tracked.use_count(), 1);
}

TEST(InlineVectorTest, CopyMoveAndEquality) {
    InlineVector<int, 2> a{1, 2, 3};
    InlineVector<int, 2> b = a;
    EXPECT_EQ(a, b);
    b.push_back(4);
    EXPECT_NE(a, b);

    InlineVector<int, 2> c = std::move(b);
    EXPECT_EQ(c.size(), 4u);
    EXPECT_EQ(c[3], 4);
    EXPECT_TRUE(b.empty());  // NOLINT(bugprone-use-after-move)

    InlineVector<int, 2> d;
    d = std::move(c);
    EXPECT_EQ(d.size(), 4u);
    EXPECT_TRUE(c.empty());  // NOLINT(bugprone-use-after-move)

    InlineVector<int, 2> e{1, 2};
    InlineVector<int, 2> f{1, 2};
    EXPECT_EQ(e, f);
    e = {7};
    EXPECT_EQ(e.size(), 1u);
    EXPECT_EQ(e[0], 7);
}

TEST(InlineVectorTest, SpanView) {
    InlineVector<int, 2> v{5, 6, 7};
    std::span<const int> view{v.data(), v.size()};
    ASSERT_EQ(view.size(), 3u);
    EXPECT_EQ(view[2], 7);
    std::span<const int> fromRange{v.begin(), v.end()};
    EXPECT_EQ(fromRange.size(), 3u);
}
