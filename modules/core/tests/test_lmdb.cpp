#include <radray/lmdb.h>

#include <atomic>
#include <filesystem>
#include <string_view>

#include <gtest/gtest.h>

#include <radray/guid.h>

namespace radray {
namespace {

class ScopedTempDir {
public:
    ScopedTempDir() {
        static std::atomic<uint64_t> counter{0};
        Path = std::filesystem::temp_directory_path() /
               ("radray_lmdb_test_" + std::to_string(counter.fetch_add(1)));
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

constexpr uint64_t kMapSize = 64ull * 1024ull * 1024ull;

void PutValue(LmdbTransaction& txn, LmdbDatabase db, std::string_view key, std::string_view value, string& error) {
    ASSERT_TRUE(txn.Put(db, LmdbValue{key}, LmdbValue{value}, &error)) << error;
}

vector<byte> GetValue(LmdbTransaction& txn, LmdbDatabase db, std::string_view key, string& error) {
    vector<byte> out;
    LmdbResult result = txn.Get(db, LmdbValue{key}, out, &error);
    EXPECT_EQ(result, LmdbResult::Ok) << error;
    return out;
}

string ToString(const vector<byte>& bytes) {
    return string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

class LmdbTest : public ::testing::Test {
protected:
    void SetUp() override {
        _dir = std::make_unique<ScopedTempDir>();
    }

    /// 打开一个可写环境并创建 assets 表, 返回打开的写事务 (含 assets dbi)。
    void OpenWrite(std::string_view dbName, LmdbEnvironment& env, LmdbTransaction& txn, LmdbDatabase& db, string& error) {
        ASSERT_TRUE(env.Open(_dir->Path / "test.mdb", kMapSize, false, &error)) << error;
        ASSERT_TRUE(txn.Begin(env, false, &error)) << error;
        ASSERT_TRUE(txn.OpenDatabase(dbName, true, db, &error)) << error;
    }

    std::unique_ptr<ScopedTempDir> _dir;
};

TEST_F(LmdbTest, PutGetRoundtripIncludingBinaryAndEmpty) {
    LmdbEnvironment env;
    LmdbTransaction txn;
    LmdbDatabase db = 0;
    string error;
    OpenWrite("assets", env, txn, db, error);

    // 字符串值、含 NUL 的二进制值、空值。
    PutValue(txn, db, "str", "hello", error);
    const std::string binary("\x00\x01\x02\xff", 4);
    ASSERT_TRUE(txn.Put(db, LmdbValue{"bin"}, LmdbValue{binary}, &error)) << error;
    PutValue(txn, db, "empty", "", error);
    ASSERT_TRUE(txn.Commit(&error)) << error;

    LmdbTransaction read;
    ASSERT_TRUE(read.Begin(env, true, &error)) << error;
    EXPECT_EQ(ToString(GetValue(read, db, "str", error)), "hello");
    EXPECT_EQ(ToString(GetValue(read, db, "bin", error)), binary);
    EXPECT_TRUE(GetValue(read, db, "empty", error).empty());
    read.Abort();
}

TEST_F(LmdbTest, GetMissingKeyIsNotFound) {
    LmdbEnvironment env;
    LmdbTransaction txn;
    LmdbDatabase db = 0;
    string error;
    OpenWrite("assets", env, txn, db, error);
    ASSERT_TRUE(txn.Commit(&error)) << error;

    LmdbTransaction read;
    ASSERT_TRUE(read.Begin(env, true, &error)) << error;
    vector<byte> out;
    EXPECT_EQ(read.Get(db, LmdbValue{"missing"}, out, &error), LmdbResult::NotFound);
}

TEST_F(LmdbTest, PutNoOverwriteDistinguishesNewFromExisting) {
    LmdbEnvironment env;
    LmdbTransaction txn;
    LmdbDatabase db = 0;
    string error;
    OpenWrite("assets", env, txn, db, error);

    EXPECT_EQ(txn.PutNoOverwrite(db, LmdbValue{"k"}, LmdbValue{"v1"}, &error), LmdbResult::Ok) << error;
    EXPECT_EQ(txn.PutNoOverwrite(db, LmdbValue{"k"}, LmdbValue{"v2"}, &error), LmdbResult::NotFound);
    ASSERT_TRUE(txn.Commit(&error)) << error;

    // 第一次写入的值未被覆盖。
    LmdbTransaction read;
    ASSERT_TRUE(read.Begin(env, true, &error)) << error;
    EXPECT_EQ(ToString(GetValue(read, db, "k", error)), "v1");
}

TEST_F(LmdbTest, DeleteReportsNotFoundForMissingKey) {
    LmdbEnvironment env;
    LmdbTransaction txn;
    LmdbDatabase db = 0;
    string error;
    OpenWrite("assets", env, txn, db, error);

    PutValue(txn, db, "k", "v", error);
    EXPECT_EQ(txn.Delete(db, LmdbValue{"k"}, &error), LmdbResult::Ok) << error;
    EXPECT_EQ(txn.Delete(db, LmdbValue{"k"}, &error), LmdbResult::NotFound);
    ASSERT_TRUE(txn.Commit(&error)) << error;
}

TEST_F(LmdbTest, CursorIteratesInKeyOrder) {
    LmdbEnvironment env;
    LmdbTransaction txn;
    LmdbDatabase db = 0;
    string error;
    OpenWrite("assets", env, txn, db, error);

    // 乱序写入, 游标按 key 字节序返回。
    PutValue(txn, db, "c", "3", error);
    PutValue(txn, db, "a", "1", error);
    PutValue(txn, db, "b", "2", error);
    ASSERT_TRUE(txn.Commit(&error)) << error;

    LmdbTransaction read;
    ASSERT_TRUE(read.Begin(env, true, &error)) << error;
    LmdbCursor cursor;
    ASSERT_TRUE(cursor.Open(read, db, &error)) << error;

    LmdbValue key;
    vector<byte> value;
    EXPECT_EQ(cursor.First(key, value, &error), LmdbResult::Ok) << error;
    EXPECT_EQ(ToString(value), "1");
    EXPECT_EQ(cursor.Next(key, value, &error), LmdbResult::Ok) << error;
    EXPECT_EQ(ToString(value), "2");
    EXPECT_EQ(cursor.Next(key, value, &error), LmdbResult::Ok) << error;
    EXPECT_EQ(ToString(value), "3");
    EXPECT_EQ(cursor.Next(key, value, &error), LmdbResult::NotFound);
}

TEST_F(LmdbTest, PersistsAcrossCloseAndReopen) {
    LmdbEnvironment env;
    LmdbTransaction txn;
    LmdbDatabase db = 0;
    string error;
    OpenWrite("assets", env, txn, db, error);
    PutValue(txn, db, "k", "v", error);
    ASSERT_TRUE(txn.Commit(&error)) << error;
    env.Close();

    ASSERT_TRUE(env.Open(_dir->Path / "test.mdb", kMapSize, false, &error)) << error;
    LmdbTransaction read;
    ASSERT_TRUE(read.Begin(env, true, &error)) << error;
    ASSERT_TRUE(read.OpenDatabase("assets", false, db, &error)) << error;
    EXPECT_EQ(ToString(GetValue(read, db, "k", error)), "v");
}

TEST_F(LmdbTest, GuidKeyRoundtrip) {
    LmdbEnvironment env;
    LmdbTransaction txn;
    LmdbDatabase db = 0;
    string error;
    OpenWrite("assets", env, txn, db, error);

    const Guid guid = Guid::NewGuid();
    ASSERT_TRUE(txn.Put(db, LmdbValue{&guid, sizeof(guid)}, LmdbValue{"payload"}, &error)) << error;
    ASSERT_TRUE(txn.Commit(&error)) << error;

    LmdbTransaction read;
    ASSERT_TRUE(read.Begin(env, true, &error)) << error;
    vector<byte> out;
    EXPECT_EQ(read.Get(db, LmdbValue{&guid, sizeof(guid)}, out, &error), LmdbResult::Ok) << error;
    EXPECT_EQ(ToString(out), "payload");
}

}  // namespace
}  // namespace radray
