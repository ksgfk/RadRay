#include <gtest/gtest.h>

#include <type_traits>
#include <radray/intrusive_ptr.h>
#include <radray/logger.h>

#include <radray/render/pipeline_layout_cache.h>

namespace radray::render {
namespace {

struct LifetimeCounts {
    uint32_t Created{0};
    uint32_t Destroyed{0};
};

class TestCache;
class TestEntry;
void IntrusivePtrAddRef(TestEntry* entry) noexcept;
void IntrusivePtrRelease(TestEntry* entry) noexcept;

class TestEntry final : public CachedPipelineLayout {
public:
    TestEntry(TestCache* cache, uint32_t key, LifetimeCounts* counts)
        : Key(key), Counts(counts), Cache(cache) { ++Counts->Created; }
    ~TestEntry() noexcept override { ++Counts->Destroyed; }
    uint32_t Key;
    LifetimeCounts* Counts;

private:
    friend void IntrusivePtrAddRef(TestEntry* entry) noexcept;
    friend void IntrusivePtrRelease(TestEntry* entry) noexcept;
    TestCache* Cache;
};

class TestCache final : public PipelineLayoutCache {
public:
    explicit TestCache(LifetimeCounts* counts) : Counts(counts) {}
    ~TestCache() noexcept override { Destroy(); }
    size_t GetEntryCount() const noexcept override { return Entries.size(); }
    IntrusivePtr<TestEntry> Get(uint32_t key) {
        if (IsClosed()) return nullptr;
        auto it = Entries.find(key);
        if (it == Entries.end()) it = Entries.emplace(key, make_unique<TestEntry>(this, key, Counts)).first;
        return RetainRef(it->second.get());
    }

private:
    friend void IntrusivePtrRelease(TestEntry* entry) noexcept;
    void Evict(CachedPipelineLayout* entry) noexcept override {
        Entries.erase(static_cast<TestEntry*>(entry)->Key);
    }
    LifetimeCounts* Counts;
    unordered_map<uint32_t, unique_ptr<TestEntry>> Entries;
};

void IntrusivePtrAddRef(TestEntry* entry) noexcept {
    RADRAY_ASSERT(!entry->Cache->IsClosed());
    entry->AddRef();
}

void IntrusivePtrRelease(TestEntry* entry) noexcept {
    if (entry->ReleaseRef()) entry->Cache->Evict(entry);
}

static_assert(std::has_virtual_destructor_v<PipelineLayoutCache>);
static_assert(std::has_virtual_destructor_v<CachedPipelineLayout>);
static_assert(std::has_virtual_destructor_v<TestCache>);
static_assert(std::has_virtual_destructor_v<TestEntry>);

TEST(PipelineLayoutCacheTest, LastReferenceEvictsAndRecreationAllocatesAgain) {
    LifetimeCounts counts;
    TestCache cache{&counts};
    auto first = cache.Get(7);
    auto second = cache.Get(7);
    EXPECT_EQ(first, second);
    EXPECT_EQ(counts.Created, 1u);
    EXPECT_EQ(first->GetRefCount(), 2u);
    auto copy = first;
    auto moved = std::move(copy);
    EXPECT_FALSE(copy);
    EXPECT_EQ(moved->GetRefCount(), 3u);
    first.Reset();
    second.Reset();
    EXPECT_EQ(cache.GetEntryCount(), 1u);
    EXPECT_EQ(counts.Destroyed, 0u);
    moved.Reset();
    EXPECT_EQ(cache.GetEntryCount(), 0u);
    EXPECT_EQ(counts.Destroyed, 1u);
    auto replacement = cache.Get(7);
    EXPECT_EQ(counts.Created, 2u);
    replacement.Reset();
    EXPECT_EQ(counts.Destroyed, 2u);
    cache.Destroy();
    cache.Destroy();
    EXPECT_FALSE(cache.Get(7));
}

TEST(PipelineLayoutCacheTest, EntriesAndCachesHaveIndependentOwnership) {
    LifetimeCounts counts;
    TestCache firstCache{&counts};
    TestCache secondCache{&counts};
    auto first = firstCache.Get(1);
    auto different = firstCache.Get(2);
    auto otherDevice = secondCache.Get(1);
    EXPECT_NE(first, different);
    EXPECT_NE(first, otherDevice);
    first.Reset();
    EXPECT_EQ(firstCache.GetEntryCount(), 1u);
    EXPECT_EQ(secondCache.GetEntryCount(), 1u);
    different.Reset();
    otherDevice.Reset();
    EXPECT_EQ(counts.Created, counts.Destroyed);
}

}  // namespace
}  // namespace radray::render
