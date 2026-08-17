#include <radray/runtime/asset_database.h>

#include <algorithm>
#include <fstream>
#include <system_error>

#include <fmt/format.h>

#include <radray/file.h>
#include <radray/logger.h>
#include <radray/text_encoding.h>

#if defined(RADRAY_PLATFORM_WINDOWS)
#include <radray/platform/win32_headers.h>
#endif

namespace radray {
namespace {

constexpr std::string_view kManifestFileName = "assets.json";
constexpr uint32_t kManifestVersion = 1;

char LowerAscii(char value) noexcept {
    return value >= 'A' && value <= 'Z'
               ? static_cast<char>(value - 'A' + 'a')
               : value;
}

string LowerAscii(std::string_view value) {
    string result{value};
    std::transform(result.begin(), result.end(), result.begin(), [](char character) {
        return LowerAscii(character);
    });
    return result;
}

bool IsValidUtf8(std::string_view value) noexcept {
    size_t index = 0;
    while (index < value.size()) {
        const uint8_t first = static_cast<uint8_t>(value[index++]);
        if (first <= 0x7f) {
            continue;
        }

        size_t continuationCount = 0;
        uint8_t secondMinimum = 0x80;
        uint8_t secondMaximum = 0xbf;
        if (first >= 0xc2 && first <= 0xdf) {
            continuationCount = 1;
        } else if (first >= 0xe0 && first <= 0xef) {
            continuationCount = 2;
            if (first == 0xe0) {
                secondMinimum = 0xa0;
            } else if (first == 0xed) {
                secondMaximum = 0x9f;
            }
        } else if (first >= 0xf0 && first <= 0xf4) {
            continuationCount = 3;
            if (first == 0xf0) {
                secondMinimum = 0x90;
            } else if (first == 0xf4) {
                secondMaximum = 0x8f;
            }
        } else {
            return false;
        }

        if (value.size() - index < continuationCount) {
            return false;
        }
        const uint8_t second = static_cast<uint8_t>(value[index++]);
        if (second < secondMinimum || second > secondMaximum) {
            return false;
        }
        for (size_t continuation = 1; continuation < continuationCount; ++continuation) {
            const uint8_t byte = static_cast<uint8_t>(value[index++]);
            if (byte < 0x80 || byte > 0xbf) {
                return false;
            }
        }
    }
    return true;
}

std::optional<string> NormalizeEntryPath(std::string_view input) {
    if (input.empty() || !IsValidUtf8(input) || input.front() == '/' || input.front() == '\\') {
        return std::nullopt;
    }

    vector<string> segments;
    string segment;
    auto finishSegment = [&]() -> bool {
        if (segment.empty() || segment == ".") {
            segment.clear();
            return true;
        }
        if (segment == "..") {
            return false;
        }
        segments.push_back(std::move(segment));
        segment.clear();
        return true;
    };

    for (char value : input) {
        if (value == '\0' || value == ':') {
            return std::nullopt;
        }
        if (value == '/' || value == '\\') {
            if (!finishSegment()) {
                return std::nullopt;
            }
            continue;
        }
        segment.push_back(value);
    }
    if (!finishSegment() || segments.empty()) {
        return std::nullopt;
    }

    string result;
    size_t resultSize = segments.size() - 1;
    for (const string& part : segments) {
        resultSize += part.size();
    }
    result.reserve(resultSize);
    for (size_t i = 0; i < segments.size(); ++i) {
        if (i != 0) {
            result.push_back('/');
        }
        result += segments[i];
    }
    return result;
}

bool IsValidStoredPath(std::string_view path) noexcept {
    if (path.empty() || !IsValidUtf8(path) || path.front() == '/' || path.back() == '/' ||
        path.find('\\') != std::string_view::npos ||
        path.find(':') != std::string_view::npos ||
        path.find('\0') != std::string_view::npos) {
        return false;
    }

    size_t start = 0;
    while (start < path.size()) {
        const size_t separator = path.find('/', start);
        const size_t end = separator == std::string_view::npos ? path.size() : separator;
        const std::string_view segment = path.substr(start, end - start);
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        if (separator == std::string_view::npos) {
            break;
        }
        start = separator + 1;
    }
    return true;
}

string MakePathKey(std::string_view normalizedPath) {
#if defined(RADRAY_PLATFORM_WINDOWS)
    const std::optional<wstring> widePath = ToWideChar(normalizedPath);
    if (widePath.has_value()) {
        const int required = LCMapStringEx(
            LOCALE_NAME_INVARIANT,
            LCMAP_LOWERCASE,
            widePath->data(),
            static_cast<int>(widePath->size()),
            nullptr,
            0,
            nullptr,
            nullptr,
            0);
        if (required > 0) {
            wstring lowerPath(static_cast<size_t>(required), L'\0');
            if (LCMapStringEx(
                    LOCALE_NAME_INVARIANT,
                    LCMAP_LOWERCASE,
                    widePath->data(),
                    static_cast<int>(widePath->size()),
                    lowerPath.data(),
                    required,
                    nullptr,
                    nullptr,
                    0) == required) {
                const std::optional<string> utf8Path = ToMultiByte(lowerPath);
                if (utf8Path.has_value()) {
                    return utf8Path.value();
                }
            }
        }
    }
#endif
    return LowerAscii(normalizedPath);
}

std::filesystem::path PathFromUtf8(std::string_view value) {
    const auto* data = reinterpret_cast<const char8_t*>(value.data());
    return std::filesystem::path{std::u8string_view{data, value.size()}};
}

string PathToUtf8(const std::filesystem::path& value) {
    const u8string encoded = value.generic_u8string();
    return string{
        reinterpret_cast<const char*>(encoded.data()),
        encoded.size()};
}

/// 源文件缺损只记 warning（内容性错误，不阻止条目进索引）。
/// 【`exists` 的 error_code 重载在文件不存在时返回 false 并清空 ec】所以"不存在"与
/// "查不了"必须分开判定，`!exists(p, ec) || ec` 那种写法里的 `|| ec` 永远为假。
void WarnIfSourceFileUnavailable(const AssetId& guid, const std::filesystem::path& path) {
    std::error_code error;
    if (std::filesystem::exists(path, error)) {
        return;
    }
    if (error) {
        RADRAY_WARN_LOG(
            "AssetDatabase: cannot determine whether the source file for asset {} exists ({}): {}",
            guid,
            path.string(),
            error.message());
        return;
    }
    RADRAY_WARN_LOG("AssetDatabase: source file for asset {} is missing: {}", guid, path.string());
}

bool IsValidImporterExtension(std::string_view extension) noexcept {
    return extension.size() > 1 && extension.front() == '.' &&
           extension.find('/') == std::string_view::npos &&
           extension.find('\\') == std::string_view::npos;
}

/// 定位每个 entry 的 settings JSON 值在原清单中的精确字节片段。yyjson 的 JsonValue 是解析后
/// 的 view，不能恢复原始空白与数字拼写，因此这里只做语法跳读，不承担结构解码。
class SettingsSourceScanner {
public:
    explicit SettingsSourceScanner(std::string_view source) noexcept : _source(source) {}

    bool Scan(vector<std::optional<string>>& settings) {
        SkipWhitespace();
        if (!Consume('{')) {
            return false;
        }
        SkipWhitespace();
        if (Consume('}')) {
            return false;
        }

        bool foundAssets = false;
        unordered_set<string> keys;
        for (;;) {
            string key;
            if (!ParseString(&key)) {
                return false;
            }
            if ((key == "version" || key == "assets") && !keys.insert(key).second) {
                return false;
            }
            SkipWhitespace();
            if (!Consume(':')) {
                return false;
            }
            SkipWhitespace();
            if (key == "assets") {
                if (foundAssets || !ParseAssets(settings)) {
                    return false;
                }
                foundAssets = true;
            } else if (!SkipValue()) {
                return false;
            }
            SkipWhitespace();
            if (Consume('}')) {
                break;
            }
            if (!Consume(',')) {
                return false;
            }
            SkipWhitespace();
        }
        SkipWhitespace();
        return foundAssets && _position == _source.size();
    }

private:
    void SkipWhitespace() noexcept {
        while (_position < _source.size()) {
            const char value = _source[_position];
            if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
                break;
            }
            ++_position;
        }
    }

    bool Consume(char expected) noexcept {
        if (_position >= _source.size() || _source[_position] != expected) {
            return false;
        }
        ++_position;
        return true;
    }

    static int HexValue(char value) noexcept {
        if (value >= '0' && value <= '9') {
            return value - '0';
        }
        if (value >= 'a' && value <= 'f') {
            return value - 'a' + 10;
        }
        if (value >= 'A' && value <= 'F') {
            return value - 'A' + 10;
        }
        return -1;
    }

    bool ParseString(string* decoded = nullptr) {
        if (!Consume('"')) {
            return false;
        }
        while (_position < _source.size()) {
            const char value = _source[_position++];
            if (value == '"') {
                return true;
            }
            if (static_cast<unsigned char>(value) < 0x20) {
                return false;
            }
            if (value != '\\') {
                if (decoded != nullptr) {
                    decoded->push_back(value);
                }
                continue;
            }
            if (_position >= _source.size()) {
                return false;
            }
            const char escape = _source[_position++];
            char decodedEscape = '\0';
            switch (escape) {
                case '"': decodedEscape = '"'; break;
                case '\\': decodedEscape = '\\'; break;
                case '/': decodedEscape = '/'; break;
                case 'b': decodedEscape = '\b'; break;
                case 'f': decodedEscape = '\f'; break;
                case 'n': decodedEscape = '\n'; break;
                case 'r': decodedEscape = '\r'; break;
                case 't': decodedEscape = '\t'; break;
                case 'u': {
                    if (_source.size() - _position < 4) {
                        return false;
                    }
                    uint32_t codePoint = 0;
                    for (size_t i = 0; i < 4; ++i) {
                        const int hex = HexValue(_source[_position++]);
                        if (hex < 0) {
                            return false;
                        }
                        codePoint = (codePoint << 4) | static_cast<uint32_t>(hex);
                    }
                    if (decoded != nullptr) {
                        decoded->push_back(codePoint <= 0x7f ? static_cast<char>(codePoint) : '?');
                    }
                    continue;
                }
                default:
                    return false;
            }
            if (decoded != nullptr) {
                decoded->push_back(decodedEscape);
            }
        }
        return false;
    }

    bool SkipValue(size_t* valueBegin = nullptr, size_t* valueEnd = nullptr) {
        SkipWhitespace();
        const size_t begin = _position;
        if (_position >= _source.size()) {
            return false;
        }

        const char first = _source[_position];
        if (first == '"') {
            if (!ParseString()) {
                return false;
            }
        } else if (first == '{') {
            ++_position;
            SkipWhitespace();
            if (!Consume('}')) {
                for (;;) {
                    if (!ParseString()) {
                        return false;
                    }
                    SkipWhitespace();
                    if (!Consume(':') || !SkipValue()) {
                        return false;
                    }
                    SkipWhitespace();
                    if (Consume('}')) {
                        break;
                    }
                    if (!Consume(',')) {
                        return false;
                    }
                    SkipWhitespace();
                }
            }
        } else if (first == '[') {
            ++_position;
            SkipWhitespace();
            if (!Consume(']')) {
                for (;;) {
                    if (!SkipValue()) {
                        return false;
                    }
                    SkipWhitespace();
                    if (Consume(']')) {
                        break;
                    }
                    if (!Consume(',')) {
                        return false;
                    }
                    SkipWhitespace();
                }
            }
        } else if (_source.substr(_position, 4) == "true" ||
                   _source.substr(_position, 4) == "null") {
            _position += 4;
        } else if (_source.substr(_position, 5) == "false") {
            _position += 5;
        } else {
            const size_t numberBegin = _position;
            while (_position < _source.size()) {
                const char value = _source[_position];
                if ((value >= '0' && value <= '9') || value == '-' || value == '+' ||
                    value == '.' || value == 'e' || value == 'E') {
                    ++_position;
                    continue;
                }
                break;
            }
            if (_position == numberBegin) {
                return false;
            }
        }

        if (valueBegin != nullptr) {
            *valueBegin = begin;
        }
        if (valueEnd != nullptr) {
            *valueEnd = _position;
        }
        return true;
    }

    bool ParseAssets(vector<std::optional<string>>& settings) {
        if (!Consume('[')) {
            return false;
        }
        SkipWhitespace();
        if (Consume(']')) {
            return true;
        }
        for (;;) {
            std::optional<string> raw;
            if (!ParseEntry(raw)) {
                return false;
            }
            settings.push_back(std::move(raw));
            SkipWhitespace();
            if (Consume(']')) {
                return true;
            }
            if (!Consume(',')) {
                return false;
            }
            SkipWhitespace();
        }
    }

    bool ParseEntry(std::optional<string>& rawSettings) {
        if (!Consume('{')) {
            return false;
        }
        SkipWhitespace();
        if (Consume('}')) {
            return true;
        }
        unordered_set<string> keys;
        for (;;) {
            string key;
            if (!ParseString(&key)) {
                return false;
            }
            const bool isSchemaKey =
                key == "guid" || key == "path" || key == "type" || key == "settings";
            if (isSchemaKey && !keys.insert(key).second) {
                return false;
            }
            SkipWhitespace();
            if (!Consume(':')) {
                return false;
            }
            size_t begin = 0;
            size_t end = 0;
            if (!SkipValue(&begin, &end)) {
                return false;
            }
            if (key == "settings") {
                if (rawSettings.has_value()) {
                    return false;
                }
                rawSettings = string{_source.substr(begin, end - begin)};
            }
            SkipWhitespace();
            if (Consume('}')) {
                return true;
            }
            if (!Consume(',')) {
                return false;
            }
            SkipWhitespace();
        }
    }

    std::string_view _source;
    size_t _position{0};
};

std::optional<string> EncodeJsonString(std::string_view value) noexcept {
    return SerializeJson(value, false);
}

std::optional<string> EncodeSettings(const AssetImportSettings& settings) noexcept {
    JsonWriter writer;
    if (!writer.IsValid()) {
        return std::nullopt;
    }
    JsonWriteContext context{writer};
    if (!settings.Serialize(context)) {
        return std::nullopt;
    }
    return writer.Write(false);
}

bool WriteManifestAtomically(
    const std::filesystem::path& manifestPath,
    std::string_view contents,
    string& outError) {
    std::error_code error;
    if (manifestPath.has_parent_path()) {
        std::filesystem::create_directories(manifestPath.parent_path(), error);
        if (error) {
            outError = fmt::format(
                "failed to create asset manifest directory '{}': {}",
                manifestPath.parent_path().string(),
                error.message());
            return false;
        }
    }

    std::filesystem::path temporaryPath = manifestPath;
    temporaryPath += ".tmp";
    std::ofstream file{temporaryPath, std::ios::binary | std::ios::trunc};
    if (!file) {
        outError = fmt::format("failed to open temporary asset manifest '{}'", temporaryPath.string());
        return false;
    }
    if (!contents.empty()) {
        file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }
    file.flush();
    const bool writeSucceeded = static_cast<bool>(file);
    file.close();
    if (!writeSucceeded || file.fail()) {
        outError = fmt::format("failed to write temporary asset manifest '{}'", temporaryPath.string());
        error.clear();
        std::filesystem::remove(temporaryPath, error);
        return false;
    }

#if defined(RADRAY_PLATFORM_WINDOWS)
    if (!MoveFileExW(
            temporaryPath.c_str(),
            manifestPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const std::error_code moveError{
            static_cast<int>(GetLastError()),
            std::system_category()};
        outError = fmt::format(
            "failed to replace asset manifest '{}': {}",
            manifestPath.string(),
            moveError.message());
        error.clear();
        std::filesystem::remove(temporaryPath, error);
        return false;
    }
#else
    std::filesystem::rename(temporaryPath, manifestPath, error);
    if (error) {
        outError = fmt::format(
            "failed to replace asset manifest '{}': {}",
            manifestPath.string(),
            error.message());
        std::error_code cleanupError;
        std::filesystem::remove(temporaryPath, cleanupError);
        return false;
    }
#endif
    return true;
}

}  // namespace

AssetDatabase::AssetDatabase(
    std::filesystem::path assetRoot,
    vector<unique_ptr<AssetImporter>> importers) noexcept
    : _assetRoot(std::move(assetRoot)),
      _ownedImporters(std::move(importers)) {
}

unique_ptr<AssetDatabase> AssetDatabase::Open(
    const std::filesystem::path& assetRoot,
    vector<unique_ptr<AssetImporter>> importers,
    string& outError) {
    outError.clear();
    if (assetRoot.empty()) {
        outError = "asset root is empty";
        return nullptr;
    }

    std::error_code error;
    std::filesystem::path absoluteRoot = std::filesystem::absolute(assetRoot, error);
    if (error) {
        outError = fmt::format("cannot resolve asset root '{}': {}", assetRoot.string(), error.message());
        return nullptr;
    }
    absoluteRoot = absoluteRoot.lexically_normal();
    const bool rootExists = std::filesystem::exists(absoluteRoot, error);
    if (error) {
        outError = fmt::format("cannot inspect asset root '{}': {}", absoluteRoot.string(), error.message());
        return nullptr;
    }
    if (rootExists && !std::filesystem::is_directory(absoluteRoot, error)) {
        outError = fmt::format("asset root '{}' is not a directory", absoluteRoot.string());
        return nullptr;
    }
    if (error) {
        outError = fmt::format("cannot inspect asset root '{}': {}", absoluteRoot.string(), error.message());
        return nullptr;
    }

    unique_ptr<AssetDatabase> database{new AssetDatabase(std::move(absoluteRoot), std::move(importers))};
    for (const unique_ptr<AssetImporter>& importerOwner : database->_ownedImporters) {
        if (importerOwner == nullptr) {
            outError = "importer list contains a null importer";
            return nullptr;
        }
        AssetImporter* importer = importerOwner.get();
        const std::string_view typeName = importer->GetTypeName();
        if (typeName.empty()) {
            outError = "importer type name is empty";
            return nullptr;
        }
        auto [typeIt, typeInserted] = database->_importers.emplace(string{typeName}, importer);
        if (!typeInserted) {
            outError = fmt::format("duplicate importer type '{}'", typeName);
            return nullptr;
        }
        for (std::string_view extension : importer->GetFileExtensions()) {
            if (!IsValidImporterExtension(extension)) {
                outError = fmt::format("importer '{}' has invalid extension '{}'", typeName, extension);
                return nullptr;
            }
            string extensionKey = LowerAscii(extension);
            auto [extensionIt, extensionInserted] = database->_extensionImporters.emplace(std::move(extensionKey), importer);
            if (!extensionInserted) {
                outError = fmt::format(
                    "file extension '{}' is claimed by both '{}' and '{}'",
                    extension,
                    extensionIt->second->GetTypeName(),
                    typeName);
                return nullptr;
            }
        }
    }

    const std::filesystem::path manifestPath = database->_assetRoot / kManifestFileName;
    const bool manifestExists = std::filesystem::exists(manifestPath, error);
    if (error) {
        outError = fmt::format("cannot inspect asset manifest '{}': {}", manifestPath.string(), error.message());
        return nullptr;
    }
    if (!manifestExists) {
        return database;
    }

    std::optional<string> source = ReadTextFile(manifestPath);
    if (!source.has_value()) {
        outError = fmt::format("cannot read asset manifest '{}'", manifestPath.string());
        return nullptr;
    }
    std::optional<JsonDocument> document = JsonDocument::Parse(source.value());
    if (!document.has_value()) {
        outError = fmt::format("asset manifest '{}' is not valid JSON", manifestPath.string());
        return nullptr;
    }

    const JsonValue root = document->Root();
    if (!root.IsObject() || root.Size() != 2 || !root.Has("version") || !root.Has("assets")) {
        outError = "asset manifest root must contain only 'version' and 'assets'";
        return nullptr;
    }
    uint32_t version = 0;
    if (!DeserializeJsonValue(root["version"], version) || version != kManifestVersion) {
        outError = fmt::format("unsupported asset manifest version (expected {})", kManifestVersion);
        return nullptr;
    }
    const JsonValue assets = root["assets"];
    if (!assets.IsArray()) {
        outError = "asset manifest 'assets' must be an array";
        return nullptr;
    }

    vector<std::optional<string>> rawSettings;
    SettingsSourceScanner scanner{source.value()};
    if (!scanner.Scan(rawSettings) || rawSettings.size() != assets.Size()) {
        outError = "asset manifest settings source spans could not be recovered";
        return nullptr;
    }

    for (size_t index = 0; index < assets.Size(); ++index) {
        const JsonValue jsonEntry = assets.At(index);
        if (!jsonEntry.IsObject()) {
            outError = fmt::format("asset entry {} must be an object", index);
            return nullptr;
        }
        const JsonValue guidValue = jsonEntry["guid"];
        const JsonValue pathValue = jsonEntry["path"];
        const JsonValue typeValue = jsonEntry["type"];
        if (!guidValue.IsString() || !pathValue.IsString() || !typeValue.IsString()) {
            outError = fmt::format("asset entry {} requires string 'guid', 'path', and 'type' fields", index);
            return nullptr;
        }

        AssetEntry entry;
        const std::string_view guidText = guidValue.AsString();
        if (!Guid::TryParse(guidText, entry.Guid) || entry.Guid.IsEmpty()) {
            outError = fmt::format("asset entry {} has invalid or empty guid '{}'", index, guidText);
            return nullptr;
        }
        entry.Path = string{pathValue.AsString()};
        if (!IsValidStoredPath(entry.Path)) {
            outError = fmt::format("asset entry {} has invalid stored path '{}'", index, entry.Path);
            return nullptr;
        }
        entry.Type = string{typeValue.AsString()};
        if (entry.Type.empty()) {
            outError = fmt::format("asset entry {} has an empty type", index);
            return nullptr;
        }
        if (database->_entries.contains(entry.Guid)) {
            outError = fmt::format("asset manifest contains duplicate guid {}", entry.Guid);
            return nullptr;
        }
        const string pathKey = MakePathKey(entry.Path);
        if (auto existing = database->_paths.find(pathKey); existing != database->_paths.end()) {
            outError = fmt::format(
                "asset path '{}' duplicates the path of asset {} (paths are case-insensitive)",
                entry.Path,
                existing->second);
            return nullptr;
        }

        AssetImporter* importer = database->FindImporter(entry.Type);
        const bool hasSettings = jsonEntry.Has("settings");
        if (hasSettings && !rawSettings[index].has_value()) {
            outError = fmt::format("asset entry {} settings text is unavailable", index);
            return nullptr;
        }
        if (importer == nullptr) {
            RADRAY_WARN_LOG("AssetDatabase: asset {} uses unregistered importer type '{}'", entry.Guid, entry.Type);
            if (hasSettings) {
                entry.RawSettings = std::move(rawSettings[index].value());
            }
        } else if (hasSettings) {
            unique_ptr<AssetImportSettings> settings = importer->CreateSettings();
            if (settings == nullptr) {
                RADRAY_WARN_LOG("AssetDatabase: importer '{}' does not accept settings for asset {}", entry.Type, entry.Guid);
                entry.RawSettings = std::move(rawSettings[index].value());
            } else if (!settings->Deserialize(jsonEntry["settings"])) {
                RADRAY_WARN_LOG("AssetDatabase: settings for asset {} failed to decode as type '{}'", entry.Guid, entry.Type);
                entry.RawSettings = std::move(rawSettings[index].value());
            } else {
                entry.Settings = std::move(settings);
            }
        } else {
            entry.Settings = importer->CreateSettings();
        }

        WarnIfSourceFileUnavailable(
            entry.Guid,
            database->_assetRoot / PathFromUtf8(entry.Path));

        const AssetId guid = entry.Guid;
        database->_paths.emplace(pathKey, guid);
        database->_entries.emplace(guid, std::move(entry));
    }

    return database;
}

const AssetEntry* AssetDatabase::Find(const AssetId& id) const noexcept {
    auto it = _entries.find(id);
    return it == _entries.end() ? nullptr : &it->second;
}

const AssetEntry* AssetDatabase::Find(std::string_view relPath) const noexcept {
    const std::optional<string> normalized = NormalizeEntryPath(relPath);
    if (!normalized.has_value()) {
        return nullptr;
    }
    auto pathIt = _paths.find(MakePathKey(normalized.value()));
    return pathIt == _paths.end() ? nullptr : Find(pathIt->second);
}

std::filesystem::path AssetDatabase::ResolvePath(const AssetEntry& entry) const {
    return (_assetRoot / PathFromUtf8(entry.Path)).lexically_normal();
}

AssetImporter* AssetDatabase::FindImporter(std::string_view type) const noexcept {
    auto it = _importers.find(string{type});
    return it == _importers.end() ? nullptr : it->second;
}

std::optional<AssetId> AssetDatabase::AddEntry(
    std::string_view relPath,
    std::string_view type,
    string& outError) {
    outError.clear();
    const std::optional<string> normalized = NormalizeEntryPath(relPath);
    if (!normalized.has_value()) {
        outError = fmt::format(
            "invalid asset path '{}': expected an asset-root-relative path without a drive, absolute root, or '..'",
            relPath);
        return std::nullopt;
    }
    if (type.empty()) {
        outError = "asset type is empty";
        return std::nullopt;
    }

    const string pathKey = MakePathKey(normalized.value());
    if (auto existing = _paths.find(pathKey); existing != _paths.end()) {
        outError = fmt::format("asset path '{}' is already registered as {}", normalized.value(), existing->second);
        return std::nullopt;
    }

    AssetId guid;
    do {
        guid = Guid::NewGuid();
    } while (guid.IsEmpty() || _entries.contains(guid));

    AssetEntry entry{
        .Guid = guid,
        .Path = normalized.value(),
        .Type = string{type}};
    if (AssetImporter* importer = FindImporter(type); importer != nullptr) {
        entry.Settings = importer->CreateSettings();
    } else {
        RADRAY_WARN_LOG("AssetDatabase: adding asset {} with unregistered importer type '{}'", guid, type);
    }

    _paths.emplace(pathKey, guid);
    _entries.emplace(guid, std::move(entry));
    return guid;
}

bool AssetDatabase::SetPath(
    const AssetId& id,
    std::string_view newRelPath,
    string& outError) {
    outError.clear();
    auto entryIt = _entries.find(id);
    if (entryIt == _entries.end()) {
        outError = fmt::format("asset {} is not registered", id);
        return false;
    }
    const std::optional<string> normalized = NormalizeEntryPath(newRelPath);
    if (!normalized.has_value()) {
        outError = fmt::format(
            "invalid asset path '{}': expected an asset-root-relative path without a drive, absolute root, or '..'",
            newRelPath);
        return false;
    }

    const string newKey = MakePathKey(normalized.value());
    if (auto existing = _paths.find(newKey); existing != _paths.end() && existing->second != id) {
        outError = fmt::format("asset path '{}' is already registered as {}", normalized.value(), existing->second);
        return false;
    }

    _paths.erase(MakePathKey(entryIt->second.Path));
    entryIt->second.Path = normalized.value();
    _paths.emplace(newKey, id);
    return true;
}

bool AssetDatabase::RemoveEntry(const AssetId& id) noexcept {
    auto it = _entries.find(id);
    if (it == _entries.end()) {
        return false;
    }
    _paths.erase(MakePathKey(it->second.Path));
    _entries.erase(it);
    return true;
}

bool AssetDatabase::Save(string& outError) const {
    outError.clear();
    vector<const AssetEntry*> entries;
    entries.reserve(_entries.size());
    for (const auto& [guid, entry] : _entries) {
        entries.push_back(&entry);
    }
    std::sort(entries.begin(), entries.end(), [](const AssetEntry* left, const AssetEntry* right) {
        return left->Path < right->Path;
    });

    string manifest = "{\n  \"version\": 1,\n  \"assets\": [";
    if (!entries.empty()) {
        manifest.push_back('\n');
    }
    for (size_t index = 0; index < entries.size(); ++index) {
        const AssetEntry& entry = *entries[index];
        const std::optional<string> guid = EncodeJsonString(entry.Guid.ToString());
        const std::optional<string> path = EncodeJsonString(entry.Path);
        const std::optional<string> type = EncodeJsonString(entry.Type);
        if (!guid.has_value() || !path.has_value() || !type.has_value()) {
            outError = fmt::format("failed to encode manifest strings for asset {}", entry.Guid);
            return false;
        }

        manifest += "    {\n      \"guid\": ";
        manifest += guid.value();
        manifest += ",\n      \"path\": ";
        manifest += path.value();
        manifest += ",\n      \"type\": ";
        manifest += type.value();

        if (entry.Settings != nullptr) {
            const std::optional<string> settings = EncodeSettings(*entry.Settings);
            if (!settings.has_value()) {
                outError = fmt::format("failed to encode settings for asset {}", entry.Guid);
                return false;
            }
            manifest += ",\n      \"settings\": ";
            manifest += settings.value();
        } else if (!entry.RawSettings.empty()) {
            manifest += ",\n      \"settings\": ";
            manifest += entry.RawSettings;
        }
        manifest += "\n    }";
        manifest += index + 1 == entries.size() ? "\n" : ",\n";
    }
    manifest += "  ]\n}\n";

    const std::filesystem::path manifestPath = _assetRoot / kManifestFileName;
    return WriteManifestAtomically(manifestPath, manifest, outError);
}

bool AssetDatabase::Refresh(string& outError) {
    outError.clear();
    std::error_code error;
    if (!std::filesystem::is_directory(_assetRoot, error)) {
        outError = error
                       ? fmt::format("cannot scan asset root '{}': {}", _assetRoot.string(), error.message())
                       : fmt::format("asset root '{}' is not a directory", _assetRoot.string());
        return false;
    }

    std::filesystem::recursive_directory_iterator iterator{_assetRoot, error};
    const std::filesystem::recursive_directory_iterator end;
    while (iterator != end) {
        if (error) {
            outError = fmt::format("failed while scanning asset root '{}': {}", _assetRoot.string(), error.message());
            return false;
        }
        const std::filesystem::directory_entry& file = *iterator;
        error.clear();
        const bool regularFile = file.is_regular_file(error);
        if (error) {
            outError = fmt::format("cannot inspect asset path '{}': {}", file.path().string(), error.message());
            return false;
        }
        if (regularFile) {
            const std::filesystem::path relativePath = file.path().lexically_relative(_assetRoot);
            const string storedPath = PathToUtf8(relativePath);
            if (storedPath != kManifestFileName && Find(storedPath) == nullptr) {
                const string extension = LowerAscii(relativePath.extension().generic_string());
                auto importerIt = _extensionImporters.find(extension);
                if (importerIt != _extensionImporters.end()) {
                    if (!AddEntry(storedPath, importerIt->second->GetTypeName(), outError).has_value()) {
                        return false;
                    }
                }
            }
        }
        iterator.increment(error);
    }
    if (error) {
        outError = fmt::format("failed while scanning asset root '{}': {}", _assetRoot.string(), error.message());
        return false;
    }

    for (const auto& [guid, entry] : _entries) {
        WarnIfSourceFileUnavailable(guid, ResolvePath(entry));
    }
    return true;
}

std::optional<task<AssetLoadResult>> AssetDatabase::CreateLoadTask(const AssetId& id) {
    const AssetEntry* entry = Find(id);
    if (entry == nullptr) {
        return std::nullopt;
    }
    AssetImporter* importer = FindImporter(entry->Type);
    if (importer == nullptr) {
        RADRAY_ERR_LOG("AssetDatabase: no importer is registered for asset {} type '{}'", id, entry->Type);
        return std::nullopt;
    }

    AssetLoadContext context{
        .AbsolutePath = ResolvePath(*entry),
        .Settings = entry->Settings.get()};
    return importer->Load(context);
}

std::optional<AssetId> AssetDatabase::ResolveId(std::string_view relPath) const {
    const AssetEntry* entry = Find(relPath);
    return entry != nullptr ? std::optional<AssetId>{entry->Guid} : std::nullopt;
}

}  // namespace radray
