// AssetDatabase: Mount 扫描、双索引、登记与落盘。分级:
// 结构性错误 (嵌套 bundle、GUID 跨 bundle 重复、path 重复、坏清单) 硬失败且索引为空;
// 内容性缺损 (未注册 type、文件缺失) warning 放行。全程不碰 GPU 与 AssetManager。
//
// 身份规则与错误分级: docs/adr/0036-per-bundle-manifest-is-asset-identity-authority.md。

#include <radray/runtime/asset_database.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

#include <gtest/gtest.h>

#include <radray/guid.h>
#include <radray/types.h>

namespace radray {
namespace {

constexpr const char* kEnvImageGuid = "8f3c1a2b-4d5e-4f60-9a7b-0c1d2e3f4a5b";
constexpr const char* kEnvRockGuid = "a12b3c4d-5e6f-4a70-8b9c-0d1e2f3a4b5c";
constexpr const char* kPropsMeshGuid = "b23c4d5e-6f70-4a81-9cad-1e2f3a4b5c6d";

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

void WriteTextFile(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

string ReadTextFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

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

Guid ParseGuid(const char* text) {
    Guid guid;
    EXPECT_TRUE(Guid::TryParse(text, guid));
    return guid;
}

constexpr const char* kEnvManifest =
    "<bundle version=\"1\">\n"
    "  <image guid=\"8f3c1a2b-4d5e-4f60-9a7b-0c1d2e3f4a5b\" path=\"skybox.png\"/>\n"
    "  <image guid=\"a12b3c4d-5e6f-4a70-8b9c-0d1e2f3a4b5c\" path=\"terrain/rock.png\"/>\n"
    "</bundle>\n";

constexpr const char* kPropsManifest =
    "<bundle version=\"1\">\n"
    "  <mesh guid=\"b23c4d5e-6f70-4a81-9cad-1e2f3a4b5c6d\" path=\"hero.gltf\"/>\n"
    "</bundle>\n";

class AssetDatabaseTest : public ::testing::Test {
protected:
    void SetUp() override {
        _dir = std::make_unique<ScopedTempDir>();
    }

    /// 标准测试树: env bundle (skybox.png 存在, terrain/rock.png 缺失) +
    /// characters/props bundle (hero.gltf 存在)。bundle 名 = 根下相对目录路径。
    void BuildStandardRoot() {
        WriteTextFile(_dir->Path / "env" / "bundle.xml", kEnvManifest);
        WriteTextFile(_dir->Path / "env" / "skybox.png", "dummy");
        WriteTextFile(_dir->Path / "characters" / "props" / "bundle.xml", kPropsManifest);
        WriteTextFile(_dir->Path / "characters" / "props" / "hero.gltf", "dummy");
    }

    std::unique_ptr<ScopedTempDir> _dir;
};

TEST_F(AssetDatabaseTest, MountBuildsIndexResolveAndFind) {
    BuildStandardRoot();

    AssetDatabase db;
    string error;
    ASSERT_TRUE(db.Mount(_dir->Path, error)) << error;

    const Guid image = ParseGuid(kEnvImageGuid);
    std::optional<AssetDatabase::ResolvedAsset> resolved = db.Resolve(image);
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->AbsolutePath, _dir->Path / "env" / "skybox.png");
    EXPECT_EQ(resolved->Type, "image");
    EXPECT_EQ(resolved->Node.Name(), "image");
    EXPECT_EQ(resolved->Node.GetAttributeNode("guid").Value(), kEnvImageGuid);

    EXPECT_EQ(db.FindByPath("env", "skybox.png"), std::optional<AssetId>(image));
    // 大小写不敏感查表。
    EXPECT_EQ(db.FindByPath("ENV", "SKYBOX.PNG"), std::optional<AssetId>(image));
    // bundle 名 = 根下相对目录路径 (含子目录)。
    EXPECT_EQ(db.FindByPath("characters/props", "hero.gltf"), std::optional<AssetId>(ParseGuid(kPropsMeshGuid)));

    EXPECT_FALSE(db.FindByPath("env", "nope.png").has_value());
    EXPECT_FALSE(db.FindByPath("nope", "skybox.png").has_value());
    EXPECT_FALSE(db.Resolve(Guid::NewGuid()).has_value());
}

TEST_F(AssetDatabaseTest, NestedBundleFailsAndLeavesIndexEmpty) {
    BuildStandardRoot();
    WriteTextFile(_dir->Path / "env" / "sub" / "bundle.xml", kPropsManifest);

    AssetDatabase db;
    string error;
    EXPECT_FALSE(db.Mount(_dir->Path, error));
    EXPECT_NE(error.find("nested"), string::npos);

    // 硬失败后索引为空: 连合法条目也查不到。
    EXPECT_FALSE(db.Resolve(ParseGuid(kEnvImageGuid)).has_value());
    EXPECT_FALSE(db.FindByPath("env", "skybox.png").has_value());
}

TEST_F(AssetDatabaseTest, DuplicateGuidAcrossBundlesFails) {
    BuildStandardRoot();
    // props bundle 复用 env 的 GUID。
    WriteTextFile(_dir->Path / "characters" / "props" / "bundle.xml",
        "<bundle version=\"1\">\n"
        "  <mesh guid=\"8f3c1a2b-4d5e-4f60-9a7b-0c1d2e3f4a5b\" path=\"hero.gltf\"/>\n"
        "</bundle>\n");

    AssetDatabase db;
    string error;
    EXPECT_FALSE(db.Mount(_dir->Path, error));
    EXPECT_NE(error.find("guid"), string::npos);
    EXPECT_FALSE(db.Resolve(ParseGuid(kEnvImageGuid)).has_value());
}

TEST_F(AssetDatabaseTest, DuplicatePathCaseInsensitiveFails) {
    WriteTextFile(_dir->Path / "env" / "bundle.xml",
        "<bundle version=\"1\">\n"
        "  <image guid=\"8f3c1a2b-4d5e-4f60-9a7b-0c1d2e3f4a5b\" path=\"Rock.png\"/>\n"
        "  <image guid=\"a12b3c4d-5e6f-4a70-8b9c-0d1e2f3a4b5c\" path=\"rock.png\"/>\n"
        "</bundle>\n");

    AssetDatabase db;
    string error;
    EXPECT_FALSE(db.Mount(_dir->Path, error));
}

TEST_F(AssetDatabaseTest, MissingFileAndUnregisteredTypeAreWarningsAndStayIndexed) {
    BuildStandardRoot();  // terrain/rock.png 缺失; 未注册任何 loader。

    AssetDatabase db;
    string error;
    ASSERT_TRUE(db.Mount(_dir->Path, error)) << error;

    // 内容性缺损放行: 条目照常进索引。
    const Guid rock = ParseGuid(kEnvRockGuid);
    std::optional<AssetDatabase::ResolvedAsset> resolved = db.Resolve(rock);
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->AbsolutePath, _dir->Path / "env" / "terrain" / "rock.png");
    EXPECT_EQ(db.FindByPath("env", "terrain/rock.png"), std::optional<AssetId>(rock));
}

TEST_F(AssetDatabaseTest, AddEntryNormalizesAssignsGuidAndAppendsAtEnd) {
    BuildStandardRoot();
    AssetDatabase db;
    string error;
    ASSERT_TRUE(db.Mount(_dir->Path, error)) << error;

    std::optional<AssetId> added = db.AddEntry("env", "textures\\wall.png", "image", error);
    ASSERT_TRUE(added.has_value()) << error;
    EXPECT_FALSE(added->IsEmpty());

    // 双索引即时可见; Resolve 拼出绝对路径。
    EXPECT_EQ(db.FindByPath("env", "textures/wall.png"), added);
    std::optional<AssetDatabase::ResolvedAsset> resolved = db.Resolve(*added);
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->AbsolutePath, _dir->Path / "env" / "textures" / "wall.png");
    EXPECT_EQ(resolved->Type, "image");

    ASSERT_TRUE(db.SaveBundle("env", error)) << error;
    const string saved = ReadTextFile(_dir->Path / "env" / "bundle.xml");
    // 新条目追加在最后一个既有条目之后, 既有节点顺序不动。
    EXPECT_LT(saved.find("rock.png"), saved.find("textures/wall.png"));
    EXPECT_LT(saved.find("skybox.png"), saved.find("rock.png"));
    EXPECT_NE(saved.find(added->ToString()), string::npos);
}

TEST_F(AssetDatabaseTest, AddEntryPathCollisionReportsExistingGuid) {
    BuildStandardRoot();
    AssetDatabase db;
    string error;
    ASSERT_TRUE(db.Mount(_dir->Path, error)) << error;

    EXPECT_FALSE(db.AddEntry("env", "skybox.png", "image", error).has_value());
    EXPECT_NE(error.find(kEnvImageGuid), string::npos);

    // 大小写不敏感撞车同样拒绝。
    EXPECT_FALSE(db.AddEntry("env", "SKYBOX.png", "image", error).has_value());
}

TEST_F(AssetDatabaseTest, AddEntryRejectsBadInput) {
    BuildStandardRoot();
    AssetDatabase db;
    string error;
    ASSERT_TRUE(db.Mount(_dir->Path, error)) << error;

    EXPECT_FALSE(db.AddEntry("env", "C:/abs.png", "image", error).has_value());
    EXPECT_FALSE(db.AddEntry("env", "a/../b.png", "image", error).has_value());
    EXPECT_FALSE(db.AddEntry("env", "", "image", error).has_value());
    EXPECT_FALSE(db.AddEntry("nope", "a.png", "image", error).has_value());
    EXPECT_NE(error.find("unknown bundle"), string::npos);
}

TEST_F(AssetDatabaseTest, SaveBundleUnknownBundleFails) {
    BuildStandardRoot();
    AssetDatabase db;
    string error;
    ASSERT_TRUE(db.Mount(_dir->Path, error)) << error;

    EXPECT_FALSE(db.SaveBundle("nope", error));
}

TEST_F(AssetDatabaseTest, SaveThenRemountYieldsSameIndex) {
    BuildStandardRoot();
    AssetDatabase db;
    string error;
    ASSERT_TRUE(db.Mount(_dir->Path, error)) << error;

    std::optional<AssetId> added = db.AddEntry("env", "wall.png", "image", error);
    ASSERT_TRUE(added.has_value());
    ASSERT_TRUE(db.SaveBundle("env", error)) << error;

    AssetDatabase remounted;
    ASSERT_TRUE(remounted.Mount(_dir->Path, error)) << error;
    EXPECT_EQ(remounted.FindByPath("env", "wall.png"), added);

    std::optional<AssetDatabase::ResolvedAsset> resolved = remounted.Resolve(*added);
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->AbsolutePath, _dir->Path / "env" / "wall.png");
}

TEST_F(AssetDatabaseTest, MountMissingRootFails) {
    AssetDatabase db;
    string error;
    EXPECT_FALSE(db.Mount(_dir->Path / "does_not_exist", error));
}

TEST_F(AssetDatabaseTest, MountRelativeRootStillResolvesAbsolutePaths) {
    BuildStandardRoot();
    AssetDatabase db;
    string error;
    {
        // 相对资产根 (测试根目录名的单段相对路径) 也必须产出绝对 AbsolutePath。
        ScopedCwd cwd(_dir->Path.parent_path());
        ASSERT_TRUE(db.Mount(_dir->Path.filename(), error)) << error;
    }

    std::optional<AssetDatabase::ResolvedAsset> resolved = db.Resolve(ParseGuid(kEnvImageGuid));
    ASSERT_TRUE(resolved.has_value());
    EXPECT_TRUE(resolved->AbsolutePath.is_absolute());
    EXPECT_EQ(resolved->AbsolutePath, std::filesystem::absolute(_dir->Path) / "env" / "skybox.png");
    EXPECT_EQ(db.FindByPath("env", "skybox.png"), std::optional<AssetId>(ParseGuid(kEnvImageGuid)));
}

TEST_F(AssetDatabaseTest, RegisterLoaderIsQueryable) {
    BuildStandardRoot();
    AssetDatabase db;
    string error;
    ASSERT_TRUE(db.Mount(_dir->Path, error)) << error;

    // 任意签名一致的 loader 函数即可注册与查询; 本测试不驱动 AssetManager。
    constexpr AssetDatabase::LoaderFn probe = [](const AssetDatabase::ResolvedAsset&) -> task<AssetLoadResult> {
        co_return AssetLoadResult::Failure();
    };
    EXPECT_FALSE(db.FindLoader("image").has_value());
    db.RegisterLoader("image", probe);
    EXPECT_EQ(db.FindLoader("image"), std::optional<AssetDatabase::LoaderFn>(probe));
    EXPECT_FALSE(db.FindLoader("mesh").has_value());
}

}  // namespace
}  // namespace radray
