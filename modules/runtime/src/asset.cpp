#include <radray/runtime/asset.h>

#include <array>
#include <system_error>

#include <fmt/format.h>

#include <radray/runtime/render_resource_recycler.h>

namespace radray {

namespace {

/// FNV-1a 64。跨平台跨编译器结果一致 —— AssetId 会进 index、进日志, 换机器不该换值。
/// 刻意不用 radray::HashCode: 它按 size_t 宽度分派, 32/64 位结果不同。
uint64_t StableHash64(std::string_view text) noexcept {
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char ch : text) {
        hash ^= static_cast<uint64_t>(ch);
        hash *= 1099511628211ull;
    }
    return hash;
}

/// 把路径折成用于哈希的规范文本。规则与理由见 MakeAssetIdFromPath 的声明处。
string CanonicalizePathForAssetId(const std::filesystem::path& path) {
    std::error_code error;
    // 【必须先 absolute】: weakly_canonical 只对"已存在的最长前缀"调 canonical, 余下部分
    // 按词法拼回。故传入相对路径而其首段【不存在】时, 返回值仍是相对的 —— 于是同一个
    // 相对路径会因 CWD 不同、甚至因文件是否已存在而算出不同的 id。absolute 只做词法
    // 拼接 (CWD + path), 不要求文件存在, 正好补上这个缺口。
    std::filesystem::path absolute = std::filesystem::absolute(path, error);
    if (error) {
        // CWD 不可读 (被删除 / 权限不足)。此时无从绝对化, 退到词法归一化 —— 结果可能
        // 仍是相对路径, 但至少是确定的。
        error.clear();
        absolute = path;
    }
    std::filesystem::path normalized = std::filesystem::weakly_canonical(absolute, error);
    if (error || normalized.empty()) {
        // 【兜底必须是确定的】: weakly_canonical 会因盘符不可用 / 权限不足而失败, 若此时
        // 直接用原样路径, 同一份文件的 id 就取决于当时的 IO 结果。词法归一化不解 symlink,
        // 但仍消掉 "." 与 ".."。
        normalized = absolute.lexically_normal();
    }
    string text = normalized.generic_string();
#if defined(_WIN32)
    // NTFS 路径大小写不敏感, 而 weakly_canonical 不做这层归一化。POSIX 下不转 —— 那里
    // 大小写是显著的。
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
#endif
    return text;
}

}  // namespace

AssetId MakeAssetIdFromPath(std::string_view namespacePrefix, const std::filesystem::path& path) {
    const string key = fmt::format("{}:{}", namespacePrefix, CanonicalizePathForAssetId(path));
    // 两次哈希取不同盐, 拼成 128 位。单个 64 位哈希填不满 Guid, 补零会让所有 id 的高
    // 64 位相同, 白扔一半空间。
    const uint64_t low = StableHash64(key);
    const uint64_t high = StableHash64(fmt::format("{}:salt", key));
    std::array<uint8_t, Guid::Size> bytes{};
    for (size_t i = 0; i < 8; ++i) {
        bytes[i] = static_cast<uint8_t>((low >> ((7 - i) * 8)) & 0xffu);
        bytes[i + 8] = static_cast<uint8_t>((high >> ((7 - i) * 8)) & 0xffu);
    }
    // 打成 RFC 4122 v4 的版本与 variant 位, 使它在日志与工具里与真 GUID 同形。
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0fu) | 0x40u);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3fu) | 0x80u);
    return AssetId{bytes};
}

}  // namespace radray
