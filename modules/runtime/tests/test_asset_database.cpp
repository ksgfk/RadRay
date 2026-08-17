#include <radray/runtime/asset_database.h>

#include <array>
#include <filesystem>
#include <string_view>
#include <system_error>

#include <gtest/gtest.h>

#include <fmt/format.h>

#include <radray/file.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/texture_asset.h>

namespace radray {
namespace {
class TestImportSettings;
class OtherImportSettings;
}  // namespace

template <>
struct RuntimeTypeTrait<TestImportSettings> {
    static constexpr RuntimeTypeId value{0xcf96688c, 0xc3de, 0x4c3f, 0xb2, 0xa5, 0xa6, 0x2d, 0xa3, 0x7c, 0x10, 0x11};
    using Bases = std::tuple<>;
};

template <>
struct RuntimeTypeTrait<OtherImportSettings> {
    static constexpr RuntimeTypeId value{0x95363339, 0xc182, 0x42af, 0x89, 0x35, 0xce, 0xa2, 0x52, 0xc4, 0x63, 0xa8};
    using Bases = std::tuple<>;
};

namespace {

class TestImportSettings final : public AssetImportSettings {
public:
    const RuntimeTypeInfo& GetTypeInfo() const noexcept override { return runtime_type_info_v<TestImportSettings>; }

    bool Deserialize(const JsonValue& json) override {
        JsonObjectReader object{json};
        if (!object.IsValid()) {
            return false;
        }
        const size_t knownMemberCount = static_cast<size_t>(object.Has("enabled")) +
                                        static_cast<size_t>(object.Has("scale"));
        if (json.Size() != knownMemberCount) {
            return false;
        }
        TestImportSettings decoded;
        if (!object.MemberIfPresent("enabled", decoded.Enabled) ||
            !object.MemberIfPresent("scale", decoded.Scale)) {
            return false;
        }
        *this = decoded;
        return true;
    }

    bool Serialize(JsonWriteContext& context) const noexcept override {
        JsonObjectWriter object = context.BeginObject();
        return object.IsValid() &&
               object.Member("enabled", Enabled) &&
               object.Member("scale", Scale);
    }

    bool Enabled{true};
    uint32_t Scale{1};
};

class OtherImportSettings final : public AssetImportSettings {
public:
    const RuntimeTypeInfo& GetTypeInfo() const noexcept override { return runtime_type_info_v<OtherImportSettings>; }
    bool Deserialize(const JsonValue&) override { return true; }
    bool Serialize(JsonWriteContext& context) const noexcept override { return context.Null(); }
};

class TestImporter final : public TypedAssetImporter<TestImportSettings> {
public:
    std::string_view GetTypeName() const noexcept override { return "test"; }

    std::span<const std::string_view> GetFileExtensions() const noexcept override {
        static constexpr std::array<std::string_view, 1> extensions{".test"};
        return extensions;
    }

protected:
    task<AssetLoadResult> LoadTyped(std::filesystem::path path, TestImportSettings settings) override {
        (void)path;
        (void)settings;
        co_return AssetLoadResult::Failure("test importer has no asset payload");
    }
};

struct SnapshotProbe {
    std::filesystem::path Path;
    uint32_t Scale{0};
};

class SnapshotImporter final : public TypedAssetImporter<TestImportSettings> {
public:
    explicit SnapshotImporter(shared_ptr<SnapshotProbe> probe) noexcept
        : _probe(std::move(probe)) {}

    std::string_view GetTypeName() const noexcept override { return "snapshot"; }

protected:
    task<AssetLoadResult> LoadTyped(std::filesystem::path path, TestImportSettings settings) override {
        _probe->Path = std::move(path);
        _probe->Scale = settings.Scale;
        co_return AssetLoadResult::Failure("snapshot captured");
    }

private:
    shared_ptr<SnapshotProbe> _probe;
};

task<void> AwaitLoadResult(
    task<AssetLoadResult> loadTask,
    std::optional<AssetLoadResult>* result) {
    *result = co_await std::move(loadTask);
}

vector<unique_ptr<AssetImporter>> MakeImporters() {
    vector<unique_ptr<AssetImporter>> importers;
    importers.push_back(make_unique<TestImporter>());
    return importers;
}

class ScopedDirectory {
public:
    ScopedDirectory() {
        std::error_code error;
        _path = std::filesystem::temp_directory_path(error) /
                fmt::format("radray_asset_database_{}", Guid::NewGuid());
        if (!error) {
            std::filesystem::create_directories(_path, error);
        }
        _valid = !error;
    }

    ~ScopedDirectory() noexcept {
        std::error_code error;
        std::filesystem::remove_all(_path, error);
    }

    bool IsValid() const noexcept { return _valid; }
    const std::filesystem::path& Path() const noexcept { return _path; }

    bool Write(std::string_view relPath, std::string_view contents) const noexcept {
        return WriteTextFile(_path / std::filesystem::path{relPath}, contents);
    }

private:
    std::filesystem::path _path;
    bool _valid{false};
};

class AssetDatabaseTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(_directory.IsValid()); }

    unique_ptr<AssetDatabase> Open(string& error) const {
        return AssetDatabase::Open(_directory.Path(), MakeImporters(), error);
    }

    bool WriteManifest(std::string_view text) const noexcept {
        return _directory.Write("assets.json", text);
    }

    const std::filesystem::path& Root() const noexcept { return _directory.Path(); }

    ScopedDirectory _directory;
};

TEST_F(AssetDatabaseTest, MissingManifestOpensAsAnEmptyMutableDatabase) {
    string error;
    unique_ptr<AssetDatabase> database = Open(error);
    ASSERT_NE(database, nullptr) << error;
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(database->Find(Guid::NewGuid()), nullptr);
    EXPECT_EQ(database->Find("missing.test"), nullptr);

    const std::optional<AssetId> first = database->AddEntry("textures\\.\\wall.test", "test", error);
    ASSERT_TRUE(first.has_value()) << error;
    EXPECT_FALSE(first->IsEmpty());
    const AssetEntry* entry = database->Find(first.value());
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->Path, "textures/wall.test");
    EXPECT_EQ(database->Find("TEXTURES/WALL.TEST"), entry);
    EXPECT_EQ(database->ResolveId("textures\\wall.test"), first);
    EXPECT_EQ(database->ResolvePath(*entry), (Root() / "textures" / "wall.test").lexically_normal());

    const TestImportSettings* settings = GetSettings<TestImportSettings>(*entry);
    ASSERT_NE(settings, nullptr);
    EXPECT_TRUE(settings->Enabled);
    EXPECT_EQ(settings->Scale, 1u);
    EXPECT_EQ(GetSettings<OtherImportSettings>(*entry), nullptr);

    const std::optional<AssetId> second = database->AddEntry("other.test", "test", error);
    ASSERT_TRUE(second.has_value()) << error;
    EXPECT_NE(first, second);

    const std::optional<AssetId> duplicate = database->AddEntry("Textures/WALL.test", "test", error);
    EXPECT_FALSE(duplicate.has_value());
    EXPECT_NE(error.find(first->ToString()), string::npos);

    TestImportSettings* mutableSettings = database->MutableSettings<TestImportSettings>(first.value());
    ASSERT_NE(mutableSettings, nullptr);
    mutableSettings->Enabled = false;
    mutableSettings->Scale = 7;

    ASSERT_TRUE(database->SetPath(first.value(), "moved//./wall.test", error)) << error;
    EXPECT_EQ(database->Find(first.value())->Path, "moved/wall.test");
    EXPECT_EQ(database->Find("textures/wall.test"), nullptr);
    EXPECT_EQ(database->Find("MOVED/WALL.TEST")->Guid, first.value());

    ASSERT_TRUE(database->RemoveEntry(second.value()));
    EXPECT_EQ(database->Find(second.value()), nullptr);
    EXPECT_FALSE(database->RemoveEntry(second.value()));

    ASSERT_TRUE(database->Save(error)) << error;
    unique_ptr<AssetDatabase> reopened = Open(error);
    ASSERT_NE(reopened, nullptr) << error;
    const AssetEntry* reopenedEntry = reopened->Find(first.value());
    ASSERT_NE(reopenedEntry, nullptr);
    EXPECT_EQ(reopenedEntry->Path, "moved/wall.test");
    const TestImportSettings* reopenedSettings = GetSettings<TestImportSettings>(*reopenedEntry);
    ASSERT_NE(reopenedSettings, nullptr);
    EXPECT_FALSE(reopenedSettings->Enabled);
    EXPECT_EQ(reopenedSettings->Scale, 7u);
}

TEST_F(AssetDatabaseTest, StructuralManifestErrorsFailOpen) {
    const string guid = "8f3c1a2b-4d5e-4f60-9a7b-0c1d2e3f4a5b";
    const vector<string> invalidManifests{
        "not json",
        R"({"version":2,"assets":[]})",
        R"({"version":1,"assets":[],"extra":true})",
        R"({"version":1,"assets":[{"path":"a.test","type":"test"}]})",
        R"({"version":1,"assets":[{"guid":"bad","path":"a.test","type":"test"}]})",
        R"({"version":1,"assets":[{"guid":"00000000-0000-0000-0000-000000000000","path":"a.test","type":"test"}]})",
        fmt::format(R"({{"version":1,"assets":[{{"guid":"{}","path":"a.test","type":"test"}},{{"guid":"{}","path":"b.test","type":"test"}}]}})", guid, guid),
        fmt::format(R"({{"version":1,"assets":[{{"guid":"{}","path":"A.test","type":"test"}},{{"guid":"a12b3c4d-5e6f-4a70-8b9c-0d1e2f3a4b5c","path":"a.TEST","type":"test"}}]}})", guid),
        fmt::format(R"({{"version":1,"assets":[{{"guid":"{}","path":"bad\\path.test","type":"test"}}]}})", guid),
        fmt::format(R"({{"version":1,"assets":[{{"guid":"{}","path":"/absolute.test","type":"test"}}]}})", guid),
        fmt::format(R"({{"version":1,"assets":[{{"guid":"{}","path":"C:/drive.test","type":"test"}}]}})", guid),
        fmt::format(R"({{"version":1,"assets":[{{"guid":"{}","path":"a/../b.test","type":"test"}}]}})", guid),
        fmt::format(R"({{"version":1,"assets":[{{"guid":"{}","path":"a/./b.test","type":"test"}}]}})", guid),
        fmt::format(R"({{"version":1,"assets":[{{"guid":"{}","path":"a//b.test","type":"test"}}]}})", guid),
        fmt::format(R"({{"version":1,"assets":[{{"guid":"{}","path":"folder/","type":"test"}}]}})", guid),
        fmt::format(R"({{"version":1,"assets":[{{"guid":"{}","guid":"a12b3c4d-5e6f-4a70-8b9c-0d1e2f3a4b5c","path":"a.test","type":"test"}}]}})", guid),
        fmt::format(R"({{"version":1,"assets":[{{"guid":"{}","path":"a.test","path":"b.test","type":"test"}}]}})", guid),
        fmt::format(R"({{"version":1,"assets":[{{"guid":"{}","path":"a.test","type":"test","type":"future"}}]}})", guid),
        fmt::format(R"({{"version":1,"assets":[{{"guid":"{}","path":"a.test","type":"test","settings":{{}},"settings":{{}}}}]}})", guid),
    };

    for (size_t index = 0; index < invalidManifests.size(); ++index) {
        ASSERT_TRUE(WriteManifest(invalidManifests[index])) << "case " << index;
        string error;
        unique_ptr<AssetDatabase> database = Open(error);
        EXPECT_EQ(database, nullptr) << "case " << index << ": " << invalidManifests[index];
        EXPECT_FALSE(error.empty()) << "case " << index;
    }
}

TEST_F(AssetDatabaseTest, GuidTextFormsAreAcceptedAndSavedAsLowercaseDFormat) {
    ASSERT_TRUE(WriteManifest(R"json({
  "version": 1,
  "assets": [
    {"guid":"8F3C1A2B4D5E4F609A7B0C1D2E3F4A5B","path":"n.test","type":"test"},
    {"guid":"{A12B3C4D-5E6F-4A70-8B9C-0D1E2F3A4B5C}","path":"b.test","type":"test"},
    {"guid":"(B23C4D5E-6F70-4A81-9C0D-1E2F3A4B5C6D)","path":"p.test","type":"test"}
  ]
})json"));

    string error;
    unique_ptr<AssetDatabase> database = Open(error);
    ASSERT_NE(database, nullptr) << error;
    ASSERT_TRUE(database->Save(error)) << error;
    EXPECT_FALSE(std::filesystem::exists(Root() / "assets.json.tmp"));

    const std::optional<string> saved = ReadTextFile(Root() / "assets.json");
    ASSERT_TRUE(saved.has_value());
    EXPECT_NE(saved->find("8f3c1a2b-4d5e-4f60-9a7b-0c1d2e3f4a5b"), string::npos);
    EXPECT_NE(saved->find("a12b3c4d-5e6f-4a70-8b9c-0d1e2f3a4b5c"), string::npos);
    EXPECT_NE(saved->find("b23c4d5e-6f70-4a81-9c0d-1e2f3a4b5c6d"), string::npos);
    EXPECT_EQ(saved->find("8F3C1A2B"), string::npos);
}

TEST_F(AssetDatabaseTest, RegisteredSettingsWithUnknownFieldsFallBackToRawText) {
    constexpr std::string_view rawSettings =
        R"({"srgb":true,"generateMips":true,"futureCompression":"new"})";
    const string manifest = fmt::format(R"({{
  "version": 1,
  "assets": [
    {{"guid":"8f3c1a2b-4d5e-4f60-9a7b-0c1d2e3f4a5b","path":"future.png","type":"texture","settings":{}}}
  ]
}})",
                                        rawSettings);
    ASSERT_TRUE(WriteManifest(manifest));

    FrameUploadScheduler frameUploads;
    vector<unique_ptr<AssetImporter>> importers;
    importers.push_back(make_unique<TextureImporter>(frameUploads));
    string error;
    unique_ptr<AssetDatabase> database = AssetDatabase::Open(
        Root(),
        std::move(importers),
        error);
    ASSERT_NE(database, nullptr) << error;
    const AssetEntry* entry = database->Find("future.png");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->Settings, nullptr);
    EXPECT_EQ(entry->RawSettings, rawSettings);

    ASSERT_TRUE(database->Save(error)) << error;
    const std::optional<string> saved = ReadTextFile(Root() / "assets.json");
    ASSERT_TRUE(saved.has_value());
    EXPECT_NE(saved->find(rawSettings), string::npos);
}

TEST_F(AssetDatabaseTest, LoadTaskOwnsAPathAndSettingsSnapshot) {
    shared_ptr<SnapshotProbe> probe = make_shared<SnapshotProbe>();
    vector<unique_ptr<AssetImporter>> importers;
    importers.push_back(make_unique<SnapshotImporter>(probe));

    string error;
    unique_ptr<AssetDatabase> database = AssetDatabase::Open(
        Root(),
        std::move(importers),
        error);
    ASSERT_NE(database, nullptr) << error;
    const std::optional<AssetId> id = database->AddEntry("before.snapshot", "snapshot", error);
    ASSERT_TRUE(id.has_value()) << error;
    TestImportSettings* settings = database->MutableSettings<TestImportSettings>(id.value());
    ASSERT_NE(settings, nullptr);
    settings->Scale = 3;

    std::optional<task<AssetLoadResult>> loadTask = database->CreateLoadTask(id.value());
    ASSERT_TRUE(loadTask.has_value());
    ASSERT_TRUE(database->SetPath(id.value(), "after.snapshot", error)) << error;
    settings = database->MutableSettings<TestImportSettings>(id.value());
    ASSERT_NE(settings, nullptr);
    settings->Scale = 9;
    ASSERT_TRUE(database->RemoveEntry(id.value()));

    std::optional<AssetLoadResult> result;
    TaskScope scope;
    scope.Spawn(AwaitLoadResult(std::move(loadTask.value()), &result));
    scope.WaitUntilEmpty();
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->IsSuccess());
    EXPECT_EQ(probe->Path, (Root() / "before.snapshot").lexically_normal());
    EXPECT_EQ(probe->Scale, 3u);
}

TEST_F(AssetDatabaseTest, UnknownAndInvalidSettingsKeepTheirOriginalJsonText) {
    constexpr std::string_view unknownRaw = R"({ "future" : [1, 2.00e+1], "nested": {"x" : true} })";
    constexpr std::string_view invalidRaw = R"({"enabled" : "not-a-bool", "scale": 9})";
    const string manifest = fmt::format(R"({{
  "version": 1,
  "assets": [
    {{"guid":"8f3c1a2b-4d5e-4f60-9a7b-0c1d2e3f4a5b","path":"unknown.bin","type":"future","settings":{}}},
    {{"guid":"a12b3c4d-5e6f-4a70-8b9c-0d1e2f3a4b5c","path":"invalid.test","type":"test","settings":{}}}
  ]
}})",
                                        unknownRaw, invalidRaw);
    ASSERT_TRUE(WriteManifest(manifest));

    string error;
    unique_ptr<AssetDatabase> database = Open(error);
    ASSERT_NE(database, nullptr) << error;
    const AssetEntry* unknown = database->Find("unknown.bin");
    const AssetEntry* invalid = database->Find("invalid.test");
    ASSERT_NE(unknown, nullptr);
    ASSERT_NE(invalid, nullptr);
    EXPECT_EQ(unknown->Settings, nullptr);
    EXPECT_EQ(unknown->RawSettings, unknownRaw);
    EXPECT_EQ(invalid->Settings, nullptr);
    EXPECT_EQ(invalid->RawSettings, invalidRaw);

    ASSERT_TRUE(database->Save(error)) << error;
    const std::optional<string> saved = ReadTextFile(Root() / "assets.json");
    ASSERT_TRUE(saved.has_value());
    EXPECT_NE(saved->find(unknownRaw), string::npos);
    EXPECT_NE(saved->find(invalidRaw), string::npos);

    unique_ptr<AssetDatabase> reopened = Open(error);
    ASSERT_NE(reopened, nullptr) << error;
    EXPECT_EQ(reopened->Find("unknown.bin")->RawSettings, unknownRaw);
    EXPECT_EQ(reopened->Find("invalid.test")->RawSettings, invalidRaw);
}

/// 清单读取必须按二进制进行。文本模式会把 CRLF 折成 LF，跨行的 settings 原文因此被改写并
/// 以 LF 存回，在 CRLF 工作树里表现为整段 settings 的 diff 噪声。
TEST_F(AssetDatabaseTest, CrlfSettingsTextSurvivesReadAndSaveVerbatim) {
    const string rawSettings = "{\r\n      \"future\" : [1, 2.00e+1],\r\n      \"nested\": {\"x\" : true}\r\n    }";
    const string manifest = fmt::format(
        "{{\r\n"
        "  \"version\": 1,\r\n"
        "  \"assets\": [\r\n"
        "    {{\"guid\":\"8f3c1a2b-4d5e-4f60-9a7b-0c1d2e3f4a5b\",\r\n"
        "     \"path\":\"crlf.bin\",\r\n"
        "     \"type\":\"future\",\r\n"
        "     \"settings\":{}}}\r\n"
        "  ]\r\n"
        "}}\r\n",
        rawSettings);
    ASSERT_TRUE(WriteManifest(manifest));

    string error;
    unique_ptr<AssetDatabase> database = Open(error);
    ASSERT_NE(database, nullptr) << error;
    const AssetEntry* entry = database->Find("crlf.bin");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->RawSettings, rawSettings);

    ASSERT_TRUE(database->Save(error)) << error;
    const std::optional<string> saved = ReadTextFile(Root() / "assets.json");
    ASSERT_TRUE(saved.has_value());
    EXPECT_NE(saved->find(rawSettings), string::npos);
}

TEST_F(AssetDatabaseTest, SaveSortsEntriesByPath) {
    string error;
    unique_ptr<AssetDatabase> database = Open(error);
    ASSERT_NE(database, nullptr) << error;
    ASSERT_TRUE(database->AddEntry("z.test", "test", error).has_value()) << error;
    ASSERT_TRUE(database->AddEntry("a.test", "test", error).has_value()) << error;
    ASSERT_TRUE(database->AddEntry("middle.test", "test", error).has_value()) << error;
    ASSERT_TRUE(database->Save(error)) << error;

    const std::optional<string> saved = ReadTextFile(Root() / "assets.json");
    ASSERT_TRUE(saved.has_value());
    const size_t a = saved->find("\"path\": \"a.test\"");
    const size_t middle = saved->find("\"path\": \"middle.test\"");
    const size_t z = saved->find("\"path\": \"z.test\"");
    ASSERT_NE(a, string::npos);
    ASSERT_NE(middle, string::npos);
    ASSERT_NE(z, string::npos);
    EXPECT_LT(a, middle);
    EXPECT_LT(middle, z);
}

TEST_F(AssetDatabaseTest, RefreshDiscoversClaimedFilesAndPreservesGuids) {
    ASSERT_TRUE(_directory.Write("new/first.test", "first"));
    ASSERT_TRUE(_directory.Write("second.TEST", "second"));
    ASSERT_TRUE(_directory.Write("ignored.txt", "ignored"));

    string error;
    unique_ptr<AssetDatabase> database = Open(error);
    ASSERT_NE(database, nullptr) << error;
    ASSERT_TRUE(database->Refresh(error)) << error;
    const AssetEntry* first = database->Find("new/first.test");
    const AssetEntry* second = database->Find("SECOND.test");
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(database->Find("ignored.txt"), nullptr);
    const AssetId firstGuid = first->Guid;
    const AssetId secondGuid = second->Guid;

    ASSERT_TRUE(database->Refresh(error)) << error;
    EXPECT_EQ(database->Find("new/first.test")->Guid, firstGuid);
    EXPECT_EQ(database->Find("second.test")->Guid, secondGuid);

    std::error_code removeError;
    ASSERT_TRUE(std::filesystem::remove(Root() / "new" / "first.test", removeError));
    ASSERT_FALSE(removeError);
    ASSERT_TRUE(database->Refresh(error)) << error;
    ASSERT_NE(database->Find(firstGuid), nullptr);
    EXPECT_EQ(database->Find(firstGuid)->Guid, firstGuid);
}

TEST_F(AssetDatabaseTest, AddAndSetPathRejectEscapesWithoutChangingIdentity) {
    string error;
    unique_ptr<AssetDatabase> database = Open(error);
    ASSERT_NE(database, nullptr) << error;

    for (std::string_view path : {"", "../escape.test", "folder/../escape.test", "/absolute.test", "C:/drive.test", "\\\\server\\share.test"}) {
        EXPECT_FALSE(database->AddEntry(path, "test", error).has_value()) << path;
        EXPECT_FALSE(error.empty()) << path;
    }
    string invalidUtf8 = "invalid_";
    invalidUtf8.push_back(static_cast<char>(0xff));
    invalidUtf8 += ".test";
    EXPECT_FALSE(database->AddEntry(invalidUtf8, "test", error).has_value());

#if defined(RADRAY_PLATFORM_WINDOWS)
    const string uppercaseUnicode = "unicode/\xc3\x84.test";
    const string lowercaseUnicode = "unicode/\xc3\xa4.test";
    const std::optional<AssetId> unicodeId = database->AddEntry(uppercaseUnicode, "test", error);
    ASSERT_TRUE(unicodeId.has_value()) << error;
    EXPECT_FALSE(database->AddEntry(lowercaseUnicode, "test", error).has_value());
    ASSERT_NE(database->Find(lowercaseUnicode), nullptr);
    EXPECT_EQ(database->Find(lowercaseUnicode)->Guid, unicodeId.value());
#endif

    const std::optional<AssetId> id = database->AddEntry("safe.test", "test", error);
    ASSERT_TRUE(id.has_value()) << error;
    EXPECT_FALSE(database->SetPath(id.value(), "../escape.test", error));
    ASSERT_NE(database->Find(id.value()), nullptr);
    EXPECT_EQ(database->Find(id.value())->Path, "safe.test");
    EXPECT_EQ(database->Find(id.value())->Guid, id.value());
}

}  // namespace
}  // namespace radray
