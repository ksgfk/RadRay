#include <radray/file.h>

#include <filesystem>
#include <system_error>

#include <fmt/format.h>
#include <gtest/gtest.h>

namespace radray {
namespace {

class ScopedTempDirectory {
public:
    explicit ScopedTempDirectory(std::string_view name)
        : _path(std::filesystem::temp_directory_path() / fmt::format("radray_core_file_{}", name)) {
        std::error_code ec;
        std::filesystem::remove_all(_path, ec);
    }
    ScopedTempDirectory(const ScopedTempDirectory&) = delete;
    ScopedTempDirectory& operator=(const ScopedTempDirectory&) = delete;
    ~ScopedTempDirectory() noexcept {
        std::error_code ec;
        std::filesystem::remove_all(_path, ec);
    }

    const std::filesystem::path& Path() const noexcept { return _path; }

private:
    std::filesystem::path _path;
};

TEST(FileTest, TextRoundTripPreservesCrLf) {
    const ScopedTempDirectory dir{"text"};
    const auto path = dir.Path() / "a.txt";

    ASSERT_TRUE(WriteTextFile(path, "line1\r\nline2\n"));

    EXPECT_EQ(ReadTextFile(path), string{"line1\r\nline2\n"});
}

TEST(FileTest, BinaryRoundTripPreservesBytes) {
    const ScopedTempDirectory dir{"binary"};
    const auto path = dir.Path() / "a.bin";
    const vector<byte> data{byte{0x00}, byte{0x0d}, byte{0x0a}, byte{0xff}};

    ASSERT_TRUE(WriteBinaryFile(path, data));

    EXPECT_EQ(ReadBinaryFile(path), data);
}

TEST(FileTest, WriteCreatesParentDirectories) {
    const ScopedTempDirectory dir{"nested"};
    const auto path = dir.Path() / "x" / "y" / "a.txt";

    ASSERT_TRUE(WriteTextFile(path, "ok"));

    EXPECT_EQ(ReadTextFile(path), string{"ok"});
}

TEST(FileTest, WriteOverwritesExistingContent) {
    const ScopedTempDirectory dir{"overwrite"};
    const auto path = dir.Path() / "a.txt";
    ASSERT_TRUE(WriteTextFile(path, "long content"));

    ASSERT_TRUE(WriteTextFile(path, "short"));

    EXPECT_EQ(ReadTextFile(path), string{"short"});
}

TEST(FileTest, EmptyFileReadsAsEmpty) {
    const ScopedTempDirectory dir{"empty"};
    const auto path = dir.Path() / "a.bin";
    ASSERT_TRUE(WriteBinaryFile(path, {}));

    const auto data = ReadBinaryFile(path);

    ASSERT_TRUE(data.has_value());
    EXPECT_TRUE(data->empty());
}

TEST(FileTest, MissingFileReadsAsNullopt) {
    const ScopedTempDirectory dir{"missing"};

    EXPECT_FALSE(ReadTextFile(dir.Path() / "missing.txt").has_value());
    EXPECT_FALSE(ReadBinaryFile(dir.Path() / "missing.bin").has_value());
}

TEST(FileTest, ExecutableDirectoryExists) {
    const std::filesystem::path dir = GetExecutableDirectory();

    ASSERT_FALSE(dir.empty());
    EXPECT_TRUE(std::filesystem::is_directory(dir));
}

}  // namespace
}  // namespace radray
