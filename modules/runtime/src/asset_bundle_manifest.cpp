#include <radray/runtime/asset_bundle_manifest.h>

#include <charconv>
#include <string_view>
#include <system_error>

#include <fmt/format.h>

#include <radray/guid.h>
#include <radray/logger.h>

namespace radray {
namespace {

constexpr std::string_view kManifestRootName = "bundle";
constexpr std::string_view kSupportedVersion = "1";

std::string_view TrimAscii(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\n' || value.front() == '\r')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\n' || value.back() == '\r')) {
        value.remove_suffix(1);
    }
    return value;
}

/// path 唯一性与查表用的小写折叠 (纯 ASCII: 清单 path 由文件路径构成, 不需要 Unicode
/// 大小写折叠)。写入口径与读入口径必须同一套折叠, 否则 AddEntry 拦住的重复会在
/// FindByPath 漏出。
string FoldPathKey(std::string_view value) {
    string out(value);
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch + ('a' - 'A'));
        }
    }
    return out;
}

/// 在 node 的直接子节点里找叶子: 元素名 = typeName、name 属性 = name。命中但缺 value
/// 属性视为坏值, 与未命中同样返回 nullopt。
std::optional<pugi::xml_attribute> FindLeafValue(const pugi::xml_node& node, const char* typeName, std::string_view name) {
    for (pugi::xml_node child : node.children(typeName)) {
        const pugi::xml_attribute nameAttr = child.attribute("name");
        if (!nameAttr || std::string_view(nameAttr.value()) != name) {
            continue;
        }
        const pugi::xml_attribute valueAttr = child.attribute("value");
        if (!valueAttr) {
            return std::nullopt;
        }
        return valueAttr;
    }
    return std::nullopt;
}

template <class T, class Parse>
std::optional<T> ReadLeafValue(const pugi::xml_node& node, const char* typeName, std::string_view name, Parse parse) {
    std::optional<pugi::xml_attribute> value = FindLeafValue(node, typeName, name);
    if (!value.has_value()) {
        return std::nullopt;
    }
    return parse(value->value());
}

template <class T, class Parse>
vector<T> ReadLeafValueList(const pugi::xml_node& node, const char* typeName, std::string_view name, Parse parse) {
    vector<T> out;
    for (pugi::xml_node child : node.children(typeName)) {
        const pugi::xml_attribute nameAttr = child.attribute("name");
        if (!nameAttr || std::string_view(nameAttr.value()) != name) {
            continue;
        }
        const pugi::xml_attribute valueAttr = child.attribute("value");
        if (!valueAttr) {
            continue;
        }
        std::optional<T> parsed = parse(valueAttr.value());
        if (parsed.has_value()) {
            out.push_back(std::move(*parsed));
        }
    }
    return out;
}

void WriteLeafValue(pugi::xml_node& node, const char* typeName, std::string_view name, std::string_view value) {
    for (pugi::xml_node child : node.children(typeName)) {
        const pugi::xml_attribute nameAttr = child.attribute("name");
        if (!nameAttr || std::string_view(nameAttr.value()) != name) {
            continue;
        }
        pugi::xml_attribute valueAttr = child.attribute("value");
        if (!valueAttr) {
            valueAttr = child.append_attribute("value");
        }
        valueAttr.set_value(string(value).c_str());
        return;
    }

    pugi::xml_node child = node.append_child(typeName);
    child.append_attribute("name").set_value(string(name).c_str());
    child.append_attribute("value").set_value(string(value).c_str());
}

}  // namespace

bool IsValidStoredPath(std::string_view path) noexcept {
    if (path.empty()) {
        return false;
    }
    for (char ch : path) {
        if (ch == '\\' || ch == ':') {
            return false;
        }
    }
    if (path.front() == '/') {
        return false;
    }
    size_t start = 0;
    for (;;) {
        const size_t end = path.find('/', start);
        const std::string_view segment = path.substr(start, end - start);
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return true;
}

std::optional<string> NormalizeEntryPath(std::string_view input) noexcept {
    if (input.empty() || input.front() == '/' || input.front() == '\\') {
        return std::nullopt;
    }
    for (char ch : input) {
        if (ch == ':') {
            return std::nullopt;
        }
    }

    string out;
    out.reserve(input.size());
    size_t start = 0;
    for (;;) {
        const size_t end = input.find_first_of("/\\", start);
        const std::string_view segment = input.substr(start, end - start);
        if (segment == "..") {
            return std::nullopt;
        }
        if (!segment.empty() && segment != ".") {
            if (!out.empty()) {
                out.push_back('/');
            }
            out.append(segment);
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    if (out.empty()) {
        return std::nullopt;
    }
    return out;
}

bool AssetBundleManifest::LoadFromFile(const std::filesystem::path& path, string& outError) {
    Reset();
    _path = path;

    // path.c_str() 在 Windows 上是 wchar_t*, 命中 pugixml 的宽字符重载 (内部转 UTF-8
    // 打开), 避免窄字符重载的 ANSI 代码页路径丢失; POSIX 上命中 char 重载。
    const pugi::xml_parse_result result = _document.load_file(path.c_str(),
        pugi::parse_default | pugi::parse_comments | pugi::parse_ws_pcdata, pugi::encoding_auto);
    if (!result) {
        outError = fmt::format("{}: XML parse failed at offset {}: {}", path.string(), result.offset, result.description());
        Reset();
        return false;
    }

    const pugi::xml_node root = _document.document_element();
    if (!root || std::string_view(root.name()) != kManifestRootName) {
        outError = fmt::format("{}: root element must be <bundle>", path.string());
        Reset();
        return false;
    }

    const pugi::xml_attribute version = root.attribute("version");
    if (!version || std::string_view(version.value()) != kSupportedVersion) {
        outError = fmt::format("{}: <bundle> version must be \"1\"", path.string());
        Reset();
        return false;
    }

    // 身份契约: 只校验条目的元素名、guid、path 三样, 其余属性与子节点原样保留。
    unordered_set<AssetId> seenGuids;
    for (pugi::xml_node entry : root.children()) {
        if (entry.type() != pugi::node_element) {
            continue;
        }

        const std::string_view type = entry.name();
        const pugi::xml_attribute guidAttr = entry.attribute("guid");
        if (!guidAttr) {
            outError = fmt::format("{}: entry <{}>: missing 'guid' attribute", path.string(), type);
            Reset();
            return false;
        }
        const pugi::xml_attribute pathAttr = entry.attribute("path");
        if (!pathAttr) {
            outError = fmt::format("{}: entry <{}>: missing 'path' attribute", path.string(), type);
            Reset();
            return false;
        }

        AssetId guid;
        if (!Guid::TryParse(guidAttr.value(), guid)) {
            outError = fmt::format("{}: entry <{}>: invalid guid '{}'", path.string(), type, guidAttr.value());
            Reset();
            return false;
        }

        const std::string_view relPath = pathAttr.value();
        if (!IsValidStoredPath(relPath)) {
            outError = fmt::format("{}: entry <{}>: invalid path '{}' (must be a '/'-separated bundle-relative path)",
                path.string(), type, relPath);
            Reset();
            return false;
        }

        if (seenGuids.contains(guid)) {
            outError = fmt::format("{}: duplicate guid {} (entry <{}>)", path.string(), guid, type);
            Reset();
            return false;
        }

        const string pathKey = FoldPathKey(relPath);
        if (_byPath.contains(pathKey)) {
            outError = fmt::format("{}: duplicate path '{}' (case-insensitive)", path.string(), relPath);
            Reset();
            return false;
        }

        seenGuids.insert(guid);
        _byGuid.emplace(guid, entry);
        _byPath.emplace(pathKey, entry);
    }

    _loaded = true;
    return true;
}

bool AssetBundleManifest::Save(string& outError) const {
    if (!_loaded) {
        outError = "AssetBundleManifest: Save before a successful LoadFromFile";
        return false;
    }

    // format_raw: 不注入任何缩进, 文本逐字写出 —— 未触碰节点与注释逐字节保持; 空元素统一
    // 自闭合为 "/>" (pugixml 序列化语义), 新追加条目以紧凑形式落在 <bundle> 末尾
    // (docs/adr/0037-manifest-dom-is-backing-store.md)。format_no_declaration: 原文件无
    // XML 声明时不凭空补一份 (已有声明节点按原样写出)。根元素之后的尾随空白不在 DOM 内,
    // 写回不保留。
    if (!_document.save_file(_path.c_str(), "  ", pugi::format_raw | pugi::format_no_declaration, pugi::encoding_auto)) {
        outError = fmt::format("{}: failed to save bundle manifest", _path.string());
        return false;
    }
    return true;
}

pugi::xml_node AssetBundleManifest::Root() const noexcept {
    return _document.document_element();
}

std::optional<pugi::xml_node> AssetBundleManifest::FindByGuid(const AssetId& id) const noexcept {
    auto it = _byGuid.find(id);
    if (it == _byGuid.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<pugi::xml_node> AssetBundleManifest::FindByPath(std::string_view relPath) const noexcept {
    auto it = _byPath.find(FoldPathKey(relPath));
    if (it == _byPath.end()) {
        return std::nullopt;
    }
    return it->second;
}

pugi::xml_node AssetBundleManifest::AppendEntry(std::string_view type, const AssetId& guid, std::string_view relPath) {
    if (!_loaded) {
        RADRAY_ABORT("AssetBundleManifest::AppendEntry: manifest is not loaded");
    }
    if (type.empty()) {
        RADRAY_ABORT("AssetBundleManifest::AppendEntry: type must be non-empty (entry element name is the asset type)");
    }
    if (!IsValidStoredPath(relPath)) {
        RADRAY_ABORT("AssetBundleManifest::AppendEntry: path '{}' is not storage-normalized", relPath);
    }
    if (_byGuid.contains(guid)) {
        RADRAY_ABORT("AssetBundleManifest::AppendEntry: guid {} already exists in this bundle", guid);
    }

    // 恒定追加到 <bundle> 末尾, 不做排序插入 —— 对既有节点零扰动, 两个分支同时追加时的
    // merge 冲突是"两侧都保留"的平凡形态。
    const string typeName(type);
    const string guidText = guid.ToString();
    const string pathText(relPath);

    pugi::xml_node entry = _document.document_element().append_child(pugi::node_element);
    entry.set_name(typeName.c_str());
    entry.append_attribute("guid").set_value(guidText.c_str());
    entry.append_attribute("path").set_value(pathText.c_str());

    _byGuid.emplace(guid, entry);
    _byPath.emplace(FoldPathKey(relPath), entry);
    return entry;
}

void AssetBundleManifest::Reset() noexcept {
    _document.reset();
    _path.clear();
    _byGuid.clear();
    _byPath.clear();
    _loaded = false;
}

std::optional<string> ReadString(const pugi::xml_node& node, std::string_view name) {
    return ReadLeafValue<string>(node, kLeafTypeString.data(), name, [](const char* value) { return std::optional<string>{value}; });
}

std::optional<int64_t> ReadInt(const pugi::xml_node& node, std::string_view name) {
    return ReadLeafValue<int64_t>(node, kLeafTypeInt.data(), name, [](const char* value) {
        const std::string_view text = TrimAscii(value);
        int64_t out = 0;
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out, 10);
        if (ec != std::errc() || ptr != text.data() + text.size()) {
            return std::optional<int64_t>{};
        }
        return std::optional<int64_t>{out};
    });
}

std::optional<float> ReadFloat(const pugi::xml_node& node, std::string_view name) {
    return ReadLeafValue<float>(node, kLeafTypeFloat.data(), name, [](const char* value) {
        const std::string_view text = TrimAscii(value);
        float out = 0.0f;
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out, std::chars_format::general);
        if (ec != std::errc() || ptr != text.data() + text.size()) {
            return std::optional<float>{};
        }
        return std::optional<float>{out};
    });
}

std::optional<bool> ReadBool(const pugi::xml_node& node, std::string_view name) {
    return ReadLeafValue<bool>(node, kLeafTypeBool.data(), name, [](const char* value) {
        const std::string_view text = TrimAscii(value);
        if (text == "true") {
            return std::optional<bool>{true};
        }
        if (text == "false") {
            return std::optional<bool>{false};
        }
        return std::optional<bool>{};
    });
}

std::optional<Guid> ReadGuid(const pugi::xml_node& node, std::string_view name) {
    return ReadLeafValue<Guid>(node, kLeafTypeGuid.data(), name, [](const char* value) {
        Guid out;
        if (!Guid::TryParse(value, out)) {
            return std::optional<Guid>{};
        }
        return std::optional<Guid>{out};
    });
}

vector<string> ReadStringList(const pugi::xml_node& node, std::string_view name) {
    return ReadLeafValueList<string>(node, kLeafTypeString.data(), name, [](const char* value) { return std::optional<string>{value}; });
}

vector<int64_t> ReadIntList(const pugi::xml_node& node, std::string_view name) {
    return ReadLeafValueList<int64_t>(node, kLeafTypeInt.data(), name, [](const char* value) {
        const std::string_view text = TrimAscii(value);
        int64_t out = 0;
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out, 10);
        if (ec != std::errc() || ptr != text.data() + text.size()) {
            return std::optional<int64_t>{};
        }
        return std::optional<int64_t>{out};
    });
}

vector<float> ReadFloatList(const pugi::xml_node& node, std::string_view name) {
    return ReadLeafValueList<float>(node, kLeafTypeFloat.data(), name, [](const char* value) {
        const std::string_view text = TrimAscii(value);
        float out = 0.0f;
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out, std::chars_format::general);
        if (ec != std::errc() || ptr != text.data() + text.size()) {
            return std::optional<float>{};
        }
        return std::optional<float>{out};
    });
}

vector<bool> ReadBoolList(const pugi::xml_node& node, std::string_view name) {
    return ReadLeafValueList<bool>(node, kLeafTypeBool.data(), name, [](const char* value) {
        const std::string_view text = TrimAscii(value);
        if (text == "true") {
            return std::optional<bool>{true};
        }
        if (text == "false") {
            return std::optional<bool>{false};
        }
        return std::optional<bool>{};
    });
}

vector<Guid> ReadGuidList(const pugi::xml_node& node, std::string_view name) {
    return ReadLeafValueList<Guid>(node, kLeafTypeGuid.data(), name, [](const char* value) {
        Guid out;
        if (!Guid::TryParse(value, out)) {
            return std::optional<Guid>{};
        }
        return std::optional<Guid>{out};
    });
}

void WriteString(pugi::xml_node& node, std::string_view name, std::string_view value) {
    WriteLeafValue(node, kLeafTypeString.data(), name, value);
}

void WriteInt(pugi::xml_node& node, std::string_view name, int64_t value) {
    WriteLeafValue(node, kLeafTypeInt.data(), name, fmt::format("{}", value));
}

void WriteFloat(pugi::xml_node& node, std::string_view name, float value) {
    WriteLeafValue(node, kLeafTypeFloat.data(), name, fmt::format("{}", value));
}

void WriteBool(pugi::xml_node& node, std::string_view name, bool value) {
    WriteLeafValue(node, kLeafTypeBool.data(), name, value ? "true" : "false");
}

void WriteGuid(pugi::xml_node& node, std::string_view name, const Guid& value) {
    WriteLeafValue(node, kLeafTypeGuid.data(), name, value.ToString());
}

}  // namespace radray
