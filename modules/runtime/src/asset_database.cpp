#include <radray/runtime/asset_database.h>

#include <stdexcept>
#include <string_view>

#include <fmt/format.h>

#include <radray/binary_io.h>
#include <radray/guid.h>
#include <radray/logger.h>

namespace radray {
namespace {

constexpr uint64_t kStoreMapSize = 64ull * 1024ull * 1024ull;
constexpr std::string_view kAssetsTableName = "assets";

/// 条目 value 的编解码。布局 (ADR-0038):
///   header = [u32 headerLen][u32 typeLen][type][u32 pathLen][path][u32 dataLen]
///   value  = header + data 段
/// headerLen 含自身; 四个长度字段均小端 u32。
vector<byte> EncodeEntryValue(std::string_view type, std::string_view path, std::string_view data) {
    const uint32_t headerLen = 16u + static_cast<uint32_t>(type.size()) + static_cast<uint32_t>(path.size());
    BinaryWriter writer;
    writer.U32(headerLen);
    writer.String(type);
    writer.String(path);
    writer.U32(static_cast<uint32_t>(data.size()));
    writer.Bytes(std::as_bytes(std::span{data.data(), data.size()}));
    return std::move(writer).TakeData();
}

struct DecodedEntry {
    std::string_view Type;
    std::string_view Path;
    std::string_view Data;
};

bool DecodeEntryValue(std::span<const byte> value, DecodedEntry& out) {
    BinaryReader reader(value);
    uint32_t headerLen = 0;
    std::string_view type;
    std::string_view path;
    uint32_t dataLen = 0;
    if (!reader.U32(headerLen) || !reader.String(type) || !reader.String(path) || !reader.U32(dataLen)) {
        return false;
    }
    if (headerLen != 16u + type.size() + path.size()) {
        return false;
    }
    std::span<const byte> data;
    if (!reader.Bytes(dataLen, data) || !reader.AtEnd()) {
        return false;
    }
    out.Type = type;
    out.Path = path;
    out.Data = std::string_view{reinterpret_cast<const char*>(data.data()), data.size()};
    return true;
}

/// AddEntry 输入路径的规范化: 宽容接受 \ 与 / 混用、重复分隔符与 "." 段, 输出 '/'
/// 分隔的存储形态。词法拆分与规范化交给 std::filesystem::path (Windows 下 / 与 \ 天然
/// 同义); 绝对路径、盘符 (根名) 与 ".." 段一律拒绝。
std::optional<string> NormalizeEntryPath(std::string_view input) {
    if (input.empty()) {
        return std::nullopt;
    }

    std::filesystem::path p(input.begin(), input.end());

    // 绝对路径: 有根目录 (开头 / 或 \)。Windows 下 is_absolute 需根名+根目录齐备,
    // 故用 has_root_directory 单独判根目录。
    if (p.has_root_directory()) {
        return std::nullopt;
    }
    // 盘符 / 其他根名 (含 ':')。
    if (p.has_root_name()) {
        return std::nullopt;
    }
    // 上跳段硬拒绝 (lexically_normal 会保留而非消除 ..)。
    for (const std::filesystem::path& part : p) {
        if (part == "..") {
            return std::nullopt;
        }
    }

    // 词法规范化去掉 "." 段、合并重复分隔符、去尾斜杠, 再转 '/' 分隔。
    const string out = p.lexically_normal().generic_string();
    if (out.empty() || out == ".") {
        return std::nullopt;
    }
    return out;
}

}  // namespace

AssetDatabase::AssetDatabase(const std::filesystem::path& assetRoot, const std::filesystem::path& storePath) {
    std::error_code ec;
    // 归一为绝对路径: 否则 ResolvedAsset::AbsolutePath 在调用方传相对根时也跟着变相对,
    // 违反字段契约。absolute 不做存在性检查, 失配目录留给 is_directory 报。
    const std::filesystem::path absRoot = std::filesystem::absolute(assetRoot, ec);
    const std::filesystem::path& root = ec ? assetRoot : absRoot;
    if (!std::filesystem::is_directory(root, ec)) {
        throw std::runtime_error(fmt::format("asset root '{}' is not a directory", root.string()));
    }
    _assetRoot = root;

    string error;
    if (!_env.Open(storePath, kStoreMapSize, /*readOnly=*/false, &error)) {
        throw std::runtime_error(fmt::format("failed to open asset store '{}': {}", storePath.string(), error));
    }

    LmdbTransaction txn;
    if (!txn.Begin(_env, /*readOnly=*/false, &error)) {
        throw std::runtime_error(fmt::format("failed to begin transaction: {}", error));
    }
    if (!txn.OpenDatabase(kAssetsTableName, /*create=*/true, _assetsDbi, &error)) {
        throw std::runtime_error(fmt::format("failed to open '{}' database: {}", kAssetsTableName, error));
    }
    if (!txn.Commit(&error)) {
        throw std::runtime_error(fmt::format("failed to commit: {}", error));
    }
}

std::optional<AssetDatabase::ResolvedAsset> AssetDatabase::Resolve(const AssetId& id) {
    string error;
    LmdbTransaction txn;
    if (!txn.Begin(_env, /*readOnly=*/true, &error)) {
        RADRAY_ERR_LOG("AssetDatabase: failed to open read transaction: {}", error);
        return std::nullopt;
    }

    vector<byte> raw;
    const LmdbResult result = txn.Get(_assetsDbi, LmdbValue{&id, sizeof(id)}, raw, &error);
    if (result == LmdbResult::NotFound) {
        return std::nullopt;
    }
    if (result != LmdbResult::Ok) {
        RADRAY_ERR_LOG("AssetDatabase: failed to read asset {}: {}", id, error);
        return std::nullopt;
    }

    DecodedEntry decoded;
    if (!DecodeEntryValue(raw, decoded)) {
        RADRAY_ERR_LOG("AssetDatabase: corrupt value for asset {}", id);
        return std::nullopt;
    }

    ResolvedAsset resolved;
    resolved.AbsolutePath = _assetRoot / std::filesystem::path(string(decoded.Path));
    resolved.Type = string(decoded.Type);
    resolved.Data.assign(reinterpret_cast<const byte*>(decoded.Data.data()), reinterpret_cast<const byte*>(decoded.Data.data()) + decoded.Data.size());
    return resolved;
}

std::optional<AssetId> AssetDatabase::AddEntry(std::string_view relPath, std::string_view type) {
    std::optional<string> normalized = NormalizeEntryPath(relPath);
    if (!normalized.has_value()) {
        RADRAY_ERR_LOG("AssetDatabase::AddEntry: invalid asset path '{}': must be a project-relative path (no absolute paths, drive letters or '..')", relPath);
        return std::nullopt;
    }

    // 入库资产的 GUID 由 NewGuid 一次分配、永不改变; 移动/重命名 = 人改 path, 不存在
    // 自动改 GUID 的代码路径 (ADR-0039, 身份规则保留自 ADR-0036)。本轮无 XML 落盘, data 段为空。
    const AssetId guid = Guid::NewGuid();
    const vector<byte> value = EncodeEntryValue(type, *normalized, {});

    string error;
    LmdbTransaction txn;
    if (!txn.Begin(_env, /*readOnly=*/false, &error)) {
        RADRAY_ERR_LOG("AssetDatabase::AddEntry: failed to begin transaction: {}", error);
        return std::nullopt;
    }
    if (!txn.Put(_assetsDbi, LmdbValue{&guid, sizeof(guid)}, LmdbValue{value}, &error)) {
        RADRAY_ERR_LOG("AssetDatabase::AddEntry: failed to put asset {}: {}", guid, error);
        return std::nullopt;
    }
    if (!txn.Commit(&error)) {
        RADRAY_ERR_LOG("AssetDatabase::AddEntry: failed to commit asset {}: {}", guid, error);
        return std::nullopt;
    }

    return guid;
}

}  // namespace radray
