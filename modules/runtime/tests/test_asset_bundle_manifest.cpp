// AssetBundleManifest: bundle.xml 的 DOM 常驻读写与校验。核心保证:
// (1) 未触碰节点的顺序、内容与注释在写回后逐字节不变 (format_raw 落盘);
// (2) 身份契约 (元素名 / guid / path) 之外的任何内容 Mount 层不校验、不重写;
// (3) D10 基础值 helper 坏值按"读不到"处理, 不令清单拒载。
//
// 决策与口径: docs/adr/0036-per-bundle-manifest-is-asset-identity-authority.md,
// docs/adr/0037-manifest-dom-is-backing-store.md。

#include <radray/runtime/asset_bundle_manifest.h>

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

constexpr const char* kImageGuid = "8f3c1a2b-4d5e-4f60-9a7b-0c1d2e3f4a5b";
constexpr const char* kMeshGuid = "b23c4d5e-6f70-4a81-9cad-1e2f3a4b5c6d";

// 带注释、未知属性与 loader 自定义子节点的代表性清单。注意结尾无换行: 根元素之后的
// 尾随空白不在 DOM 内, 写回不保留 (asset_bundle_manifest.cpp 的 Save)。
constexpr const char* kCanonicalManifest =
    "<bundle version=\"1\">\n"
    "  <!-- a comment that must survive -->\n"
    "  <image guid=\"8f3c1a2b-4d5e-4f60-9a7b-0c1d2e3f4a5b\" path=\"skybox.png\" tool=\"paint\">\n"
    "    <bool name=\"srgb\" value=\"true\"/>\n"
    "    <setting>\n"
    "      <int name=\"lodBias\" value=\"1\"/>\n"
    "    </setting>\n"
    "  </image>\n"
    "  <mesh guid=\"b23c4d5e-6f70-4a81-9cad-1e2f3a4b5c6d\" path=\"hero.gltf\"/>\n"
    "</bundle>";

class ScopedTempDir {
public:
    ScopedTempDir() {
        static std::atomic<uint64_t> counter{0};
        Path = std::filesystem::temp_directory_path() /
               ("radray_manifest_test_" + std::to_string(counter.fetch_add(1)));
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
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

string ReadTextFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

Guid ParseGuid(const char* text) {
    Guid guid;
    EXPECT_TRUE(Guid::TryParse(text, guid));
    return guid;
}

string ManifestXml(std::string_view inner) {
    return string("<bundle version=\"1\">\n") + string(inner) + "</bundle>\n";
}

class AssetBundleManifestTest : public ::testing::Test {
protected:
    void SetUp() override {
        _dir = std::make_unique<ScopedTempDir>();
    }

    std::filesystem::path ManifestPath() const { return _dir->Path / "bundle.xml"; }

    std::unique_ptr<ScopedTempDir> _dir;
};

TEST_F(AssetBundleManifestTest, RoundTripPreservesUntouchedNodesByteForByte) {
    WriteTextFile(ManifestPath(), kCanonicalManifest);

    AssetBundleManifest manifest;
    string error;
    ASSERT_TRUE(manifest.LoadFromFile(ManifestPath(), error)) << error;
    ASSERT_TRUE(manifest.Save(error)) << error;

    EXPECT_EQ(ReadTextFile(ManifestPath()), string(kCanonicalManifest));
}

TEST_F(AssetBundleManifestTest, AppendEntryLandsAtEndWithoutDisturbingExistingNodes) {
    WriteTextFile(ManifestPath(), kCanonicalManifest);

    AssetBundleManifest manifest;
    string error;
    ASSERT_TRUE(manifest.LoadFromFile(ManifestPath(), error)) << error;

    const Guid newGuid = ParseGuid("c34d5e6f-7081-4a92-8abd-2e3f4a5b6c7d");
    manifest.AppendEntry("image", newGuid, "wall.png");
    ASSERT_TRUE(manifest.Save(error)) << error;

    const string saved = ReadTextFile(ManifestPath());
    // 新条目排在最后一个既有条目 (hero.gltf) 之后。
    EXPECT_LT(saved.find("hero.gltf"), saved.find("wall.png"));
    // 既有节点序列不动。
    EXPECT_LT(saved.find("skybox.png"), saved.find("hero.gltf"));
    EXPECT_NE(saved.find("a comment that must survive"), string::npos);
    EXPECT_NE(saved.find("tool=\"paint\""), string::npos);
    EXPECT_NE(saved.find("<int name=\"lodBias\" value=\"1\"/>"), string::npos);
    // 新条目带 D 格式小写 GUID。
    EXPECT_NE(saved.find(newGuid.ToString()), string::npos);
}

TEST_F(AssetBundleManifestTest, RejectsBadXml) {
    WriteTextFile(ManifestPath(), "<bundle version=\"1\"><image guid=");

    AssetBundleManifest manifest;
    string error;
    EXPECT_FALSE(manifest.LoadFromFile(ManifestPath(), error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(manifest.IsLoaded());
}

TEST_F(AssetBundleManifestTest, RejectsUnsupportedOrMissingVersion) {
    WriteTextFile(ManifestPath(), "<bundle version=\"2\"><image guid=\"" + string(kImageGuid) + "\" path=\"a.png\"/></bundle>");

    AssetBundleManifest manifest;
    string error;
    EXPECT_FALSE(manifest.LoadFromFile(ManifestPath(), error));

    WriteTextFile(ManifestPath(), "<bundle><image guid=\"" + string(kImageGuid) + "\" path=\"a.png\"/></bundle>");
    AssetBundleManifest missing;
    EXPECT_FALSE(missing.LoadFromFile(ManifestPath(), error));
}

TEST_F(AssetBundleManifestTest, RejectsMissingGuidOrPath) {
    WriteTextFile(ManifestPath(), ManifestXml("<image path=\"a.png\"/>"));
    AssetBundleManifest manifest;
    string error;
    EXPECT_FALSE(manifest.LoadFromFile(ManifestPath(), error));
    EXPECT_NE(error.find("guid"), string::npos);

    WriteTextFile(ManifestPath(), ManifestXml("<image guid=\"" + string(kImageGuid) + "\"/>"));
    EXPECT_FALSE(manifest.LoadFromFile(ManifestPath(), error));
    EXPECT_NE(error.find("path"), string::npos);
}

TEST_F(AssetBundleManifestTest, RejectsInvalidGuidText) {
    WriteTextFile(ManifestPath(), ManifestXml("<image guid=\"not-a-guid\" path=\"a.png\"/>"));
    AssetBundleManifest manifest;
    string error;
    EXPECT_FALSE(manifest.LoadFromFile(ManifestPath(), error));
}

TEST_F(AssetBundleManifestTest, GuidTolerantReadVerbatimPreserve) {
    // N 格式 (32 hex) 与 B 格式 ({...}) 都能读 (TryParse 宽容 N/D/B/P); 未被触碰的条目
    // guid 文本按 DOM 常驻原样保留 —— 系统写出的 guid (AddEntry 条目 / WriteGuid helper)
    // 才恒为 D 格式小写。
    const string authored =
        "<bundle version=\"1\">\n"
        "  <image guid=\"8f3c1a2b4d5e4f609a7b0c1d2e3f4a5b\" path=\"a.png\"/>\n"
        "  <mesh guid=\"{b23c4d5e-6f70-4a81-9cad-1e2f3a4b5c6d}\" path=\"b.gltf\"/>\n"
        "</bundle>";
    WriteTextFile(ManifestPath(), authored);

    AssetBundleManifest manifest;
    string error;
    ASSERT_TRUE(manifest.LoadFromFile(ManifestPath(), error)) << error;

    std::optional<XmlElement> image = manifest.FindByGuid(ParseGuid(kImageGuid));
    ASSERT_TRUE(image.has_value());
    EXPECT_EQ(image->Name(), "image");
    EXPECT_TRUE(manifest.FindByGuid(ParseGuid(kMeshGuid)).has_value());

    ASSERT_TRUE(manifest.Save(error)) << error;
    EXPECT_EQ(ReadTextFile(ManifestPath()), authored);
}

TEST_F(AssetBundleManifestTest, RejectsInvalidStoredPaths) {
    const std::initializer_list<std::string_view> badPaths = {
        "tex\\rock.png",  // 反斜杠
        "/abs.png",       // 绝对路径
        "C:/drive.png",   // 盘符
        "a/../b.png",     // 上跳段
        "./a.png",        // 开头 ./
        "a//b.png",       // 空段
        "a/./b.png",      // 中间 . 段
        "a/",             // 尾斜杠
        "",               // 空
        "..",             // 纯上跳
    };
    for (std::string_view bad : badPaths) {
        SCOPED_TRACE(bad);
        WriteTextFile(ManifestPath(), ManifestXml("<image guid=\"" + string(kImageGuid) + "\" path=\"" + string(bad) + "\"/>"));
        AssetBundleManifest manifest;
        string error;
        EXPECT_FALSE(manifest.LoadFromFile(ManifestPath(), error)) << error;
    }
}

TEST_F(AssetBundleManifestTest, RejectsDuplicatePathCaseInsensitive) {
    WriteTextFile(ManifestPath(), ManifestXml(
        "<image guid=\"8f3c1a2b-4d5e-4f60-9a7b-0c1d2e3f4a5b\" path=\"Rock.png\"/>\n"
        "<image guid=\"a12b3c4d-5e6f-4a70-8b9c-0d1e2f3a4b5c\" path=\"rock.png\"/>\n"));
    AssetBundleManifest manifest;
    string error;
    EXPECT_FALSE(manifest.LoadFromFile(ManifestPath(), error));
}

TEST_F(AssetBundleManifestTest, RejectsDuplicateGuid) {
    WriteTextFile(ManifestPath(), ManifestXml(
        "<image guid=\"8f3c1a2b-4d5e-4f60-9a7b-0c1d2e3f4a5b\" path=\"a.png\"/>\n"
        "<mesh guid=\"8f3c1a2b-4d5e-4f60-9a7b-0c1d2e3f4a5b\" path=\"b.gltf\"/>\n"));
    AssetBundleManifest manifest;
    string error;
    EXPECT_FALSE(manifest.LoadFromFile(ManifestPath(), error));
}

TEST_F(AssetBundleManifestTest, FindByGuidAndPathCaseInsensitive) {
    WriteTextFile(ManifestPath(), kCanonicalManifest);
    AssetBundleManifest manifest;
    string error;
    ASSERT_TRUE(manifest.LoadFromFile(ManifestPath(), error)) << error;

    std::optional<XmlElement> node = manifest.FindByGuid(ParseGuid(kMeshGuid));
    ASSERT_TRUE(node.has_value());
    EXPECT_EQ(node->Name(), "mesh");

    node = manifest.FindByPath("skybox.png");
    ASSERT_TRUE(node.has_value());
    EXPECT_EQ(node->Name(), "image");

    node = manifest.FindByPath("SKYBOX.PNG");
    ASSERT_TRUE(node.has_value());

    EXPECT_FALSE(manifest.FindByPath("nope.png").has_value());
    EXPECT_FALSE(manifest.FindByGuid(Guid::NewGuid()).has_value());
}

TEST_F(AssetBundleManifestTest, LeafHelperRoundTrip) {
    WriteTextFile(ManifestPath(), kCanonicalManifest);
    AssetBundleManifest manifest;
    string error;
    ASSERT_TRUE(manifest.LoadFromFile(ManifestPath(), error)) << error;

    std::optional<XmlElement> image = manifest.FindByGuid(ParseGuid(kImageGuid));
    ASSERT_TRUE(image.has_value());
    XmlElement node = *image;

    // 读既有叶子与非叶子内嵌叶子。
    EXPECT_EQ(ReadBool(node, "srgb"), std::optional<bool>(true));
    EXPECT_EQ(ReadInt(node.Child("setting"), "lodBias"), std::optional<int64_t>(1));
    EXPECT_FALSE(ReadBool(node, "missing").has_value());

    // 写: 追加后原位改写, 不产生重复元素。
    WriteInt(node, "foo", 42);
    EXPECT_EQ(ReadInt(node, "foo"), std::optional<int64_t>(42));
    WriteInt(node, "foo", 7);
    EXPECT_EQ(ReadInt(node, "foo"), std::optional<int64_t>(7));
    int count = 0;
    for (const XmlElement& child : node.Children("int")) {
        if (child.GetAttributeNode("name").Value() == "foo") {
            ++count;
        }
    }
    EXPECT_EQ(count, 1);

    WriteString(node, "label", "hello world");
    EXPECT_EQ(ReadString(node, "label"), std::optional<string>("hello world"));

    WriteFloat(node, "lod", 0.1f);
    EXPECT_EQ(ReadFloat(node, "lod"), std::optional<float>(0.1f));

    WriteBool(node, "srgb", false);
    EXPECT_EQ(ReadBool(node, "srgb"), std::optional<bool>(false));

    const Guid ref = Guid::NewGuid();
    WriteGuid(node, "ref", ref);
    EXPECT_EQ(ReadGuid(node, "ref"), std::optional<Guid>(ref));

    // 写读往返走真实落盘。系统写出的 guid 值落盘为 D 格式小写。
    ASSERT_TRUE(manifest.Save(error)) << error;
    const string saved = ReadTextFile(ManifestPath());
    EXPECT_NE(saved.find(ref.ToString()), string::npos);

    AssetBundleManifest reloaded;
    ASSERT_TRUE(reloaded.LoadFromFile(ManifestPath(), error)) << error;
    std::optional<XmlElement> reloadedNode = reloaded.FindByGuid(ParseGuid(kImageGuid));
    ASSERT_TRUE(reloadedNode.has_value());
    EXPECT_EQ(ReadInt(*reloadedNode, "foo"), std::optional<int64_t>(7));
    EXPECT_EQ(ReadFloat(*reloadedNode, "lod"), std::optional<float>(0.1f));
    EXPECT_EQ(ReadString(*reloadedNode, "label"), std::optional<string>("hello world"));
}

TEST_F(AssetBundleManifestTest, GuidValueTolerantRead) {
    WriteTextFile(ManifestPath(), ManifestXml(
        "<image guid=\"" + string(kImageGuid) + "\" path=\"a.png\">\n"
        "  <guid name=\"ref\" value=\"{8f3c1a2b-4d5e-4f60-9a7b-0c1d2e3f4a5b}\"/>\n"
        "  <guid name=\"refN\" value=\"8f3c1a2b4d5e4f609a7b0c1d2e3f4a5b\"/>\n"
        "</image>\n"));
    AssetBundleManifest manifest;
    string error;
    ASSERT_TRUE(manifest.LoadFromFile(ManifestPath(), error)) << error;

    std::optional<XmlElement> node = manifest.FindByGuid(ParseGuid(kImageGuid));
    ASSERT_TRUE(node.has_value());
    EXPECT_EQ(ReadGuid(*node, "ref"), std::optional<Guid>(ParseGuid(kImageGuid)));
    EXPECT_EQ(ReadGuid(*node, "refN"), std::optional<Guid>(ParseGuid(kImageGuid)));
    EXPECT_FALSE(ReadGuid(*node, "none").has_value());
}

TEST_F(AssetBundleManifestTest, MalformedLeafValuesDoNotRejectManifest) {
    // 子节点属于内容而非身份: 坏值由 loader 在加载时报错, 清单层照常加载。
    WriteTextFile(ManifestPath(), ManifestXml(
        "<image guid=\"" + string(kImageGuid) + "\" path=\"a.png\">\n"
        "  <int name=\"bad\" value=\"abc\"/>\n"
        "  <bool name=\"bad2\" value=\"TRUE\"/>\n"
        "</image>\n"));
    AssetBundleManifest manifest;
    string error;
    ASSERT_TRUE(manifest.LoadFromFile(ManifestPath(), error)) << error;

    std::optional<XmlElement> node = manifest.FindByGuid(ParseGuid(kImageGuid));
    ASSERT_TRUE(node.has_value());
    EXPECT_FALSE(ReadInt(*node, "bad").has_value());
    EXPECT_FALSE(ReadBool(*node, "bad2").has_value());
}

TEST_F(AssetBundleManifestTest, ListReadsAllSameNameLeaves) {
    WriteTextFile(ManifestPath(), ManifestXml(
        "<image guid=\"" + string(kImageGuid) + "\" path=\"a.png\">\n"
        "  <int name=\"items\" value=\"1\"/>\n"
        "  <int name=\"items\" value=\"2\"/>\n"
        "  <int name=\"items\" value=\"oops\"/>\n"
        "  <int name=\"other\" value=\"9\"/>\n"
        "</image>\n"));
    AssetBundleManifest manifest;
    string error;
    ASSERT_TRUE(manifest.LoadFromFile(ManifestPath(), error)) << error;

    std::optional<XmlElement> node = manifest.FindByGuid(ParseGuid(kImageGuid));
    ASSERT_TRUE(node.has_value());
    // 同名全部收进列表, 坏值跳过, 异名不串。
    const vector<int64_t> items = ReadIntList(*node, "items");
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0], 1);
    EXPECT_EQ(items[1], 2);
    EXPECT_TRUE(ReadIntList(*node, "none").empty());
}

TEST(EntryPathNormalization, TolerantInputBecomesStorageForm) {
    EXPECT_EQ(NormalizeEntryPath("textures\\wall.png"), std::optional<string>("textures/wall.png"));
    EXPECT_EQ(NormalizeEntryPath("./a.png"), std::optional<string>("a.png"));
    EXPECT_EQ(NormalizeEntryPath("a//b/"), std::optional<string>("a/b"));
    EXPECT_EQ(NormalizeEntryPath("a\\b/c"), std::optional<string>("a/b/c"));
    EXPECT_EQ(NormalizeEntryPath("a/./b"), std::optional<string>("a/b"));
    EXPECT_EQ(NormalizeEntryPath("hero.gltf"), std::optional<string>("hero.gltf"));

    EXPECT_FALSE(NormalizeEntryPath("").has_value());
    EXPECT_FALSE(NormalizeEntryPath("/abs.png").has_value());
    EXPECT_FALSE(NormalizeEntryPath("\\abs.png").has_value());
    EXPECT_FALSE(NormalizeEntryPath("C:/drive.png").has_value());
    EXPECT_FALSE(NormalizeEntryPath("a/../b.png").has_value());
    EXPECT_FALSE(NormalizeEntryPath("..").has_value());
    EXPECT_FALSE(NormalizeEntryPath("./").has_value());
}

}  // namespace
}  // namespace radray
