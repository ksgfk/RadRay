// AssetDatabase: 构造打开 LMDB 工作库、登记与按 GUID 解析。path 是工程相对路径, 只作
// 为 value header 元数据, 无 path→guid 反查。storePath 由调用方传入, 数据持久到下次打开。
// 全程不碰 GPU 与 AssetManager。
//
// 运行时存储 = LMDB assets 表 (ADR-0038), 身份规则 (ADR-0039, 保留自 ADR-0036):
// docs/adr/0039-abandon-bundle-organization-stabilize-asset-system.md。

#include <radray/runtime/asset_database.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <stdexcept>

#include <gtest/gtest.h>

#include <radray/guid.h>
#include <radray/types.h>

namespace radray {
namespace {

class ScopedTempDir {
public:
    ScopedTempDir() {
        static std::atomic<uint64_t> counter{0};
        Path = std::filesystem::temp_directory_path() /
               ("radray_asset_db_test_" + std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(Path);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(Path, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    std::filesystem::path Path;
};

/// 测试内临时切换工作目录, 析构恢复。gtest 用例在进程内串行执行, 切换安全。
class ScopedCwd {
public:
    explicit ScopedCwd(const std::filesystem::path& dir) : Previous(std::filesystem::current_path()) {
        std::filesystem::current_path(dir);
    }

    ~ScopedCwd() {
        std::error_code ec;
        std::filesystem::current_path(Previous, ec);
    }

    ScopedCwd(const ScopedCwd&) = delete;
    ScopedCwd& operator=(const ScopedCwd&) = delete;

    std::filesystem::path Previous;
};

class AssetDatabaseTest : public ::testing::Test {
protected:
    void SetUp() override {
        _dir = std::make_unique<ScopedTempDir>();
    }

    std::filesystem::path StorePath() const { return _dir->Path / "asset.db"; }

    std::unique_ptr<ScopedTempDir> _dir;
};

TEST_F(AssetDatabaseTest, AddEntryResolve) {
    AssetDatabase db(_dir->Path, StorePath());

    std::optional<AssetId> image = db.AddEntry("textures/skybox.png", "image");
    ASSERT_TRUE(image.has_value());

    std::optional<AssetDatabase::ResolvedAsset> resolved = db.Resolve(*image);
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->AbsolutePath, std::filesystem::absolute(_dir->Path) / "textures" / "skybox.png");
    EXPECT_EQ(resolved->Type, "image");
    EXPECT_TRUE(resolved->Data.empty());

    EXPECT_FALSE(db.Resolve(Guid::NewGuid()).has_value());
}

TEST_F(AssetDatabaseTest, AddEntryNormalizesInput) {
    AssetDatabase db(_dir->Path, StorePath());

    std::optional<AssetId> added = db.AddEntry("textures\\wall.png", "image");
    ASSERT_TRUE(added.has_value());
    std::optional<AssetDatabase::ResolvedAsset> resolved = db.Resolve(*added);
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->AbsolutePath, std::filesystem::absolute(_dir->Path) / "textures" / "wall.png");
}

TEST_F(AssetDatabaseTest, AddEntryRejectsBadInput) {
    AssetDatabase db(_dir->Path, StorePath());

    EXPECT_FALSE(db.AddEntry("C:/abs.png", "image").has_value());
    EXPECT_FALSE(db.AddEntry("a/../b.png", "image").has_value());
    EXPECT_FALSE(db.AddEntry("", "image").has_value());
    EXPECT_FALSE(db.AddEntry("/abs.png", "image").has_value());
}

TEST_F(AssetDatabaseTest, ConstructorThrowsOnMissingRoot) {
    EXPECT_THROW(AssetDatabase(_dir->Path / "does_not_exist", StorePath()), std::runtime_error);
}

TEST_F(AssetDatabaseTest, ConstructorNormalizesRelativeRoot) {
    // 相对资产根 (测试根目录名的单段相对路径) 也必须产出绝对 AbsolutePath。
    std::unique_ptr<AssetDatabase> db;
    {
        ScopedCwd cwd(_dir->Path.parent_path());
        db = std::make_unique<AssetDatabase>(_dir->Path.filename(), StorePath());
    }

    std::optional<AssetId> added = db->AddEntry("a.png", "image");
    ASSERT_TRUE(added.has_value());

    std::optional<AssetDatabase::ResolvedAsset> resolved = db->Resolve(*added);
    ASSERT_TRUE(resolved.has_value());
    EXPECT_TRUE(resolved->AbsolutePath.is_absolute());
    EXPECT_EQ(resolved->AbsolutePath, std::filesystem::absolute(_dir->Path) / "a.png");
}

TEST_F(AssetDatabaseTest, StorePersistsAcrossReopen) {
    std::optional<AssetId> added;
    {
        AssetDatabase db(_dir->Path, StorePath());
        added = db.AddEntry("a.png", "image");
        ASSERT_TRUE(added.has_value());
    }

    // storePath 持久: 重开同路径仍能看到已登记条目。
    AssetDatabase reopened(_dir->Path, StorePath());
    std::optional<AssetDatabase::ResolvedAsset> resolved = reopened.Resolve(*added);
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->AbsolutePath, std::filesystem::absolute(_dir->Path) / "a.png");
    EXPECT_EQ(resolved->Type, "image");
}

}  // namespace
}  // namespace radray
