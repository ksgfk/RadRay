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
std::optional<XmlAttribute> FindLeafValue(const XmlElement& node, const char* typeName, std::string_view name) {
    for (const XmlElement& child : node.Children(typeName)) {
        const XmlAttribute nameAttr = child.GetAttributeNode("name");
        if (!nameAttr.IsValid() || nameAttr.Value() != name) {
            continue;
        }
        const XmlAttribute valueAttr = child.GetAttributeNode("value");
        if (!valueAttr.IsValid()) {
            return std::nullopt;
        }
        return valueAttr;
    }
    return std::nullopt;
}

template <class T, class Parse>
std::optional<T> ReadLeafValue(const XmlElement& node, const char* typeName, std::string_view name, Parse parse) {
    std::optional<XmlAttribute> value = FindLeafValue(node, typeName, name);
    if (!value.has_value()) {
        return std::nullopt;
    }
    return parse(value->Value());
}

template <class T, class Parse>
vector<T> ReadLeafValueList(const XmlElement& node, const char* typeName, std::string_view name, Parse parse) {
    vector<T> out;
    for (const XmlElement& child : node.Children(typeName)) {
        const XmlAttribute nameAttr = child.GetAttributeNode("name");
        if (!nameAttr.IsValid() || nameAttr.Value() != name) {
            continue;
        }
        const XmlAttribute valueAttr = child.GetAttributeNode("value");
        if (!valueAttr.IsValid()) {
            continue;
        }
        std::optional<T> parsed = parse(valueAttr.Value());
        if (parsed.has_value()) {
            out.push_back(std::move(*parsed));
        }
    }
    return out;
}

void WriteLeafValue(XmlElement& node, const char* typeName, std::string_view name, std::string_view value) {
    for (XmlElement& child : node.Children(typeName)) {
        const XmlAttribute nameAttr = child.GetAttributeNode("name");
        if (!nameAttr.IsValid() || nameAttr.Value() != name) {
            continue;
        }
        child.SetAttribute("value", value);
        return;
    }

    XmlElement child = node.AppendChild(typeName);
    child.SetAttribute("name", name);
    child.SetAttribute("value", value);
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

    // 解析标志: 保留注释 (merge 友好动机) 与纯空白文本节点 (pugixml 默认丢)。
    // Load 失败时 outError 已含 offset/description。
    if (!_document.Load(path, XmlParseFlag::Comments | XmlParseFlag::WhitespaceText, &outError)) {
        Reset();
        return false;
    }

    const XmlElement root = _document.DocumentElement();
    if (!root.IsValid() || root.Name() != kManifestRootName) {
        outError = fmt::format("{}: root element must be <bundle>", path.string());
        Reset();
        return false;
    }

    const XmlAttribute version = root.GetAttributeNode("version");
    if (!version.IsValid() || version.Value() != kSupportedVersion) {
        outError = fmt::format("{}: <bundle> version must be \"1\"", path.string());
        Reset();
        return false;
    }

    // 身份契约: 只校验条目的元素名、guid、path 三样, 其余属性与子节点原样保留。
    unordered_set<AssetId> seenGuids;
    for (const XmlNode& child : root.ChildNodes()) {
        if (child.NodeType() != XmlNodeType::Element) {
            continue;
        }
        const XmlElement entry{child};

        const std::string_view type = entry.Name();
        const XmlAttribute guidAttr = entry.GetAttributeNode("guid");
        if (!guidAttr.IsValid()) {
            outError = fmt::format("{}: entry <{}>: missing 'guid' attribute", path.string(), type);
            Reset();
            return false;
        }
        const XmlAttribute pathAttr = entry.GetAttributeNode("path");
        if (!pathAttr.IsValid()) {
            outError = fmt::format("{}: entry <{}>: missing 'path' attribute", path.string(), type);
            Reset();
            return false;
        }

        AssetId guid;
        if (!Guid::TryParse(guidAttr.Value(), guid)) {
            outError = fmt::format("{}: entry <{}>: invalid guid '{}'", path.string(), type, guidAttr.Value());
            Reset();
            return false;
        }

        const std::string_view relPath = pathAttr.Value();
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
    if (!_document.Save(_path, XmlFormatFlag::Raw | XmlFormatFlag::NoDeclaration)) {
        outError = fmt::format("{}: failed to save bundle manifest", _path.string());
        return false;
    }
    return true;
}

XmlElement AssetBundleManifest::Root() const noexcept {
    return _document.DocumentElement();
}

std::optional<XmlElement> AssetBundleManifest::FindByGuid(const AssetId& id) const noexcept {
    auto it = _byGuid.find(id);
    if (it == _byGuid.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<XmlElement> AssetBundleManifest::FindByPath(std::string_view relPath) const noexcept {
    auto it = _byPath.find(FoldPathKey(relPath));
    if (it == _byPath.end()) {
        return std::nullopt;
    }
    return it->second;
}

XmlElement AssetBundleManifest::AppendEntry(std::string_view type, const AssetId& guid, std::string_view relPath) {
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
    const string guidText = guid.ToString();
    const string pathText(relPath);

    XmlElement entry = _document.CreateElement(type);
    entry.SetAttribute("guid", guidText);
    entry.SetAttribute("path", pathText);
    _document.DocumentElement().AppendChild(entry);

    _byGuid.emplace(guid, entry);
    _byPath.emplace(FoldPathKey(relPath), entry);
    return entry;
}

void AssetBundleManifest::Reset() noexcept {
    _document.Reset();
    _path.clear();
    _byGuid.clear();
    _byPath.clear();
    _loaded = false;
}

std::optional<string> ReadString(const XmlElement& node, std::string_view name) {
    return ReadLeafValue<string>(node, kLeafTypeString.data(), name, [](std::string_view value) { return std::optional<string>{value}; });
}

std::optional<int64_t> ReadInt(const XmlElement& node, std::string_view name) {
    return ReadLeafValue<int64_t>(node, kLeafTypeInt.data(), name, [](std::string_view value) {
        const std::string_view text = TrimAscii(value);
        int64_t out = 0;
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out, 10);
        if (ec != std::errc() || ptr != text.data() + text.size()) {
            return std::optional<int64_t>{};
        }
        return std::optional<int64_t>{out};
    });
}

std::optional<float> ReadFloat(const XmlElement& node, std::string_view name) {
    return ReadLeafValue<float>(node, kLeafTypeFloat.data(), name, [](std::string_view value) {
        const std::string_view text = TrimAscii(value);
        float out = 0.0f;
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out, std::chars_format::general);
        if (ec != std::errc() || ptr != text.data() + text.size()) {
            return std::optional<float>{};
        }
        return std::optional<float>{out};
    });
}

std::optional<bool> ReadBool(const XmlElement& node, std::string_view name) {
    return ReadLeafValue<bool>(node, kLeafTypeBool.data(), name, [](std::string_view value) {
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

std::optional<Guid> ReadGuid(const XmlElement& node, std::string_view name) {
    return ReadLeafValue<Guid>(node, kLeafTypeGuid.data(), name, [](std::string_view value) {
        Guid out;
        if (!Guid::TryParse(value, out)) {
            return std::optional<Guid>{};
        }
        return std::optional<Guid>{out};
    });
}

vector<string> ReadStringList(const XmlElement& node, std::string_view name) {
    return ReadLeafValueList<string>(node, kLeafTypeString.data(), name, [](std::string_view value) { return std::optional<string>{value}; });
}

vector<int64_t> ReadIntList(const XmlElement& node, std::string_view name) {
    return ReadLeafValueList<int64_t>(node, kLeafTypeInt.data(), name, [](std::string_view value) {
        const std::string_view text = TrimAscii(value);
        int64_t out = 0;
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out, 10);
        if (ec != std::errc() || ptr != text.data() + text.size()) {
            return std::optional<int64_t>{};
        }
        return std::optional<int64_t>{out};
    });
}

vector<float> ReadFloatList(const XmlElement& node, std::string_view name) {
    return ReadLeafValueList<float>(node, kLeafTypeFloat.data(), name, [](std::string_view value) {
        const std::string_view text = TrimAscii(value);
        float out = 0.0f;
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out, std::chars_format::general);
        if (ec != std::errc() || ptr != text.data() + text.size()) {
            return std::optional<float>{};
        }
        return std::optional<float>{out};
    });
}

vector<bool> ReadBoolList(const XmlElement& node, std::string_view name) {
    return ReadLeafValueList<bool>(node, kLeafTypeBool.data(), name, [](std::string_view value) {
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

vector<Guid> ReadGuidList(const XmlElement& node, std::string_view name) {
    return ReadLeafValueList<Guid>(node, kLeafTypeGuid.data(), name, [](std::string_view value) {
        Guid out;
        if (!Guid::TryParse(value, out)) {
            return std::optional<Guid>{};
        }
        return std::optional<Guid>{out};
    });
}

void WriteString(XmlElement& node, std::string_view name, std::string_view value) {
    WriteLeafValue(node, kLeafTypeString.data(), name, value);
}

void WriteInt(XmlElement& node, std::string_view name, int64_t value) {
    WriteLeafValue(node, kLeafTypeInt.data(), name, fmt::format("{}", value));
}

void WriteFloat(XmlElement& node, std::string_view name, float value) {
    WriteLeafValue(node, kLeafTypeFloat.data(), name, fmt::format("{}", value));
}

void WriteBool(XmlElement& node, std::string_view name, bool value) {
    WriteLeafValue(node, kLeafTypeBool.data(), name, value ? "true" : "false");
}

void WriteGuid(XmlElement& node, std::string_view name, const Guid& value) {
    WriteLeafValue(node, kLeafTypeGuid.data(), name, value.ToString());
}

}  // namespace radray
