#include <radray/runtime/asset_bundle.h>

#include <radray/file.h>
#include <radray/runtime/asset_bundle_descriptors.h>
#include <radray/runtime/image_asset.h>
#include <radray/runtime/shader_asset.h>
#include <radray/runtime/static_mesh.h>
#include <radray/runtime/texture_asset.h>

namespace radray {

namespace {

bool IsUtf8(std::string_view value) noexcept {
    for (size_t i = 0; i < value.size();) {
        const uint8_t lead = static_cast<uint8_t>(value[i]);
        if (lead <= 0x7f) {
            ++i;
            continue;
        }

        size_t continuationCount = 0;
        uint8_t secondMinimum = 0x80;
        uint8_t secondMaximum = 0xbf;
        if (lead >= 0xc2 && lead <= 0xdf) {
            continuationCount = 1;
        } else if (lead == 0xe0) {
            continuationCount = 2;
            secondMinimum = 0xa0;
        } else if (lead >= 0xe1 && lead <= 0xec) {
            continuationCount = 2;
        } else if (lead == 0xed) {
            continuationCount = 2;
            secondMaximum = 0x9f;
        } else if (lead >= 0xee && lead <= 0xef) {
            continuationCount = 2;
        } else if (lead == 0xf0) {
            continuationCount = 3;
            secondMinimum = 0x90;
        } else if (lead >= 0xf1 && lead <= 0xf3) {
            continuationCount = 3;
        } else if (lead == 0xf4) {
            continuationCount = 3;
            secondMaximum = 0x8f;
        } else {
            return false;
        }

        if (i + continuationCount >= value.size()) {
            return false;
        }
        const uint8_t second = static_cast<uint8_t>(value[i + 1]);
        if (second < secondMinimum || second > secondMaximum) {
            return false;
        }
        for (size_t j = 2; j <= continuationCount; ++j) {
            const uint8_t continuation = static_cast<uint8_t>(value[i + j]);
            if (continuation < 0x80 || continuation > 0xbf) {
                return false;
            }
        }
        i += continuationCount + 1;
    }
    return true;
}

}  // namespace

namespace {

constexpr size_t kMaxXmlCatalogBytes = 4 * 1024 * 1024;
constexpr size_t kMaxXmlCatalogEntries = 65536;
constexpr size_t kMaxXmlAttributesPerEntry = 64;
constexpr size_t kMaxXmlAttributeValueBytes = 64 * 1024;

struct XmlAttribute {
    std::string_view Name;
    string Value;
};

class XmlCatalogReader {
public:
    explicit XmlCatalogReader(std::string_view text) noexcept : _text(text) {}

    bool Parse(BundleCatalog& catalog, vector<BundleDiagnostic>& diagnostics) {
        if (!IsUtf8(_text)) {
            AddError(BundleDiagnosticCode::InvalidCatalog, "XML Catalog is not valid UTF-8");
        }
        SkipBomAndDeclaration();
        if (_failed || !ParseBundle(catalog)) {
            if (_error.has_value()) {
                diagnostics.push_back(std::move(_error.value()));
            }
            return false;
        }
        return true;
    }

private:
    static bool IsSpace(char value) noexcept {
        return value == ' ' || value == '\t' || value == '\r' || value == '\n';
    }

    static bool IsNameStart(char value) noexcept {
        return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
    }

    static bool IsNameContinue(char value) noexcept {
        return IsNameStart(value) || (value >= '0' && value <= '9') || value == '-' || value == '.';
    }

    void AddError(BundleDiagnosticCode code, string message, std::optional<AssetId> asset = std::nullopt) {
        if (_failed) {
            return;
        }
        _failed = true;
        _error = BundleDiagnostic{
            .Code = code,
            .Message = std::move(message),
            .Asset = asset,
        };
    }

    void SkipWhitespace() noexcept {
        while (_position < _text.size() && IsSpace(_text[_position])) {
            ++_position;
        }
    }

    void SkipBomAndDeclaration() {
        if (_text.size() >= 3 &&
            static_cast<uint8_t>(_text[0]) == 0xef &&
            static_cast<uint8_t>(_text[1]) == 0xbb &&
            static_cast<uint8_t>(_text[2]) == 0xbf) {
            _position = 3;
        }
        if (_text.size() - _position >= 5 && _text.substr(_position, 5) == "<?xml") {
            if (_position + 5 < _text.size() &&
                _text[_position + 5] != ' ' &&
                _text[_position + 5] != '\t' &&
                _text[_position + 5] != '\r' &&
                _text[_position + 5] != '\n' &&
                _text[_position + 5] != '?') {
                AddError(BundleDiagnosticCode::InvalidCatalog, "XML processing instruction is not the XML declaration");
                return;
            }
            const size_t end = _text.find("?>", _position + 5);
            if (end == std::string_view::npos) {
                AddError(BundleDiagnosticCode::InvalidCatalog, "unterminated XML declaration");
                return;
            }
            _position = end + 2;
        }
        SkipWhitespace();
    }

    bool ParseName(std::string_view& name) {
        if (_position >= _text.size() || !IsNameStart(_text[_position])) {
            AddError(BundleDiagnosticCode::InvalidCatalog, "XML element or attribute name is invalid");
            return false;
        }
        const size_t begin = _position++;
        while (_position < _text.size() && IsNameContinue(_text[_position])) {
            ++_position;
        }
        name = _text.substr(begin, _position - begin);
        return true;
    }

    bool ParseEntity(string& output) {
        const size_t end = _text.find(';', _position + 1);
        if (end == std::string_view::npos || end - _position > 16) {
            AddError(BundleDiagnosticCode::InvalidCatalog, "unterminated or oversized XML entity");
            return false;
        }
        const std::string_view entity = _text.substr(_position + 1, end - _position - 1);
        if (entity == "amp") {
            output.push_back('&');
        } else if (entity == "lt") {
            output.push_back('<');
        } else if (entity == "gt") {
            output.push_back('>');
        } else if (entity == "quot") {
            output.push_back('"');
        } else if (entity == "apos") {
            output.push_back('\'');
        } else {
            AddError(BundleDiagnosticCode::InvalidCatalog, "unsupported XML entity");
            return false;
        }
        _position = end + 1;
        return true;
    }

    bool ParseQuotedValue(string& value) {
        if (_position >= _text.size() || (_text[_position] != '\'' && _text[_position] != '"')) {
            AddError(BundleDiagnosticCode::InvalidCatalog, "XML attribute value must be quoted");
            return false;
        }
        const char quote = _text[_position++];
        value.clear();
        value.reserve(32);
        while (_position < _text.size() && _text[_position] != quote) {
            if (value.size() >= kMaxXmlAttributeValueBytes) {
                AddError(BundleDiagnosticCode::InvalidCatalog, "XML attribute value exceeds the size limit");
                return false;
            }
            if (_text[_position] == '<') {
                AddError(BundleDiagnosticCode::InvalidCatalog, "XML attribute value contains '<'");
                return false;
            }
            if (_text[_position] == '&') {
                if (!ParseEntity(value)) {
                    return false;
                }
                continue;
            }
            const unsigned char character = static_cast<unsigned char>(_text[_position]);
            if (character < 0x20 && character != '\t' && character != '\r' && character != '\n') {
                AddError(BundleDiagnosticCode::InvalidCatalog, "XML attribute value contains a control character");
                return false;
            }
            value.push_back(_text[_position++]);
        }
        if (_position >= _text.size()) {
            AddError(BundleDiagnosticCode::InvalidCatalog, "unterminated XML attribute value");
            return false;
        }
        ++_position;
        return true;
    }

    bool ParseAttributes(vector<XmlAttribute>& attributes, bool& selfClosing) {
        attributes.clear();
        selfClosing = false;
        for (;;) {
            SkipWhitespace();
            if (_position >= _text.size()) {
                AddError(BundleDiagnosticCode::InvalidCatalog, "unterminated XML start tag");
                return false;
            }
            if (_text[_position] == '>') {
                ++_position;
                return true;
            }
            if (_text[_position] == '/' && _position + 1 < _text.size() && _text[_position + 1] == '>') {
                _position += 2;
                selfClosing = true;
                return true;
            }
            if (attributes.size() >= kMaxXmlAttributesPerEntry) {
                AddError(BundleDiagnosticCode::InvalidCatalog, "XML element has too many attributes");
                return false;
            }
            std::string_view name;
            if (!ParseName(name)) {
                return false;
            }
            for (const XmlAttribute& attribute : attributes) {
                if (attribute.Name == name) {
                    AddError(BundleDiagnosticCode::InvalidCatalog, "XML element has duplicate attributes");
                    return false;
                }
            }
            SkipWhitespace();
            if (_position >= _text.size() || _text[_position] != '=') {
                AddError(BundleDiagnosticCode::InvalidCatalog, "XML attribute is missing '='");
                return false;
            }
            ++_position;
            SkipWhitespace();
            XmlAttribute attribute;
            attribute.Name = name;
            if (!ParseQuotedValue(attribute.Value)) {
                return false;
            }
            attributes.push_back(std::move(attribute));
        }
    }

    bool ParseStartTag(std::string_view& name, vector<XmlAttribute>& attributes, bool& selfClosing) {
        if (_position >= _text.size() || _text[_position] != '<' ||
            (_position + 1 < _text.size() &&
             (_text[_position + 1] == '/' || _text[_position + 1] == '!' || _text[_position + 1] == '?'))) {
            AddError(BundleDiagnosticCode::InvalidCatalog, "XML start tag expected");
            return false;
        }
        ++_position;
        if (!ParseName(name)) {
            return false;
        }
        return ParseAttributes(attributes, selfClosing);
    }

    bool ParseEndTag(std::string_view expectedName) {
        if (_position + 2 > _text.size() || _text.substr(_position, 2) != "</") {
            AddError(BundleDiagnosticCode::InvalidCatalog, "XML end tag expected");
            return false;
        }
        _position += 2;
        std::string_view name;
        if (!ParseName(name) || name != expectedName) {
            AddError(BundleDiagnosticCode::InvalidCatalog, "XML end tag does not match its start tag");
            return false;
        }
        SkipWhitespace();
        if (_position >= _text.size() || _text[_position] != '>') {
            AddError(BundleDiagnosticCode::InvalidCatalog, "XML end tag is malformed");
            return false;
        }
        ++_position;
        return true;
    }

    const XmlAttribute* FindAttribute(const vector<XmlAttribute>& attributes, std::string_view name) const noexcept {
        for (const XmlAttribute& attribute : attributes) {
            if (attribute.Name == name) {
                return &attribute;
            }
        }
        return nullptr;
    }

    void AddEntryDiagnostic(
        BundleAssetEntry& entry,
        BundleDiagnosticCode code,
        string message) {
        entry.State = BundleEntryState::Invalid;
        entry.Diagnostics.push_back(BundleDiagnostic{
            .Code = code,
            .Message = std::move(message),
            .Asset = entry.Asset,
        });
    }

    bool ParseBoolAttribute(
        const vector<XmlAttribute>& attributes,
        std::string_view name,
        bool defaultValue,
        bool& value) {
        const XmlAttribute* attribute = FindAttribute(attributes, name);
        if (attribute == nullptr) {
            value = defaultValue;
            return true;
        }
        if (attribute->Value == "true" || attribute->Value == "1") {
            value = true;
            return true;
        }
        if (attribute->Value == "false" || attribute->Value == "0") {
            value = false;
            return true;
        }
        return false;
    }

    bool ParseShaderTargetAttribute(
        const vector<XmlAttribute>& attributes,
        shader::ShaderTarget& target) {
        const XmlAttribute* attribute = FindAttribute(attributes, "target");
        if (attribute == nullptr || attribute->Value == "dxil") {
            target = shader::ShaderTarget::DXIL;
            return true;
        }
        if (attribute->Value == "spirv" || attribute->Value == "spv") {
            target = shader::ShaderTarget::SPIRV;
            return true;
        }
        return false;
    }

    void DecodeKnownDescriptor(
        BundleAssetEntry& entry,
        std::string_view typeName,
        const vector<XmlAttribute>& attributes) {
        if (!entry.Diagnostics.empty() || entry.TypeId.IsEmpty()) {
            return;
        }

        if (typeName == "image") {
            if (entry.TypeId != runtime_type_id_v<ImageAsset>) {
                AddEntryDiagnostic(entry, BundleDiagnosticCode::TypeIdMismatch, "image tag TypeId does not match ImageAsset");
                return;
            }
            bool convert = true;
            if (!ParseBoolAttribute(attributes, "convertToRgba8", true, convert)) {
                AddEntryDiagnostic(entry, BundleDiagnosticCode::InvalidDescriptor, "image convertToRgba8 must be boolean");
                return;
            }
            entry.Descriptor = make_unique<ImageAssetDescriptor>(convert);
            entry.State = BundleEntryState::Valid;
            return;
        }

        if (typeName == "texture") {
            if (entry.TypeId != runtime_type_id_v<TextureAsset>) {
                AddEntryDiagnostic(entry, BundleDiagnosticCode::TypeIdMismatch, "texture tag TypeId does not match TextureAsset");
                return;
            }
            bool srgb = false;
            if (!ParseBoolAttribute(attributes, "srgb", false, srgb)) {
                AddEntryDiagnostic(entry, BundleDiagnosticCode::InvalidDescriptor, "texture srgb must be boolean");
                return;
            }
            entry.Descriptor = make_unique<TextureAssetDescriptor>(srgb);
            entry.State = BundleEntryState::Valid;
            return;
        }

        if (typeName == "staticMesh" || typeName == "static-mesh") {
            if (entry.TypeId != runtime_type_id_v<StaticMesh>) {
                AddEntryDiagnostic(entry, BundleDiagnosticCode::TypeIdMismatch, "static mesh tag TypeId does not match StaticMesh");
                return;
            }
            entry.Descriptor = make_unique<StaticMeshAssetDescriptor>();
            entry.State = BundleEntryState::Valid;
            return;
        }

        if (typeName == "shader") {
            if (entry.TypeId != runtime_type_id_v<ShaderAsset>) {
                AddEntryDiagnostic(entry, BundleDiagnosticCode::TypeIdMismatch, "shader tag TypeId does not match ShaderAsset");
                return;
            }
            const XmlAttribute* representation = FindAttribute(attributes, "representation");
            ShaderAssetRepresentation mode{};
            if (representation == nullptr) {
                AddEntryDiagnostic(entry, BundleDiagnosticCode::InvalidDescriptor, "shader representation is required");
                return;
            }
            if (representation->Value == "jit-source") {
                mode = ShaderAssetRepresentation::JitSource;
            } else if (representation->Value == "aot-artifact") {
                mode = ShaderAssetRepresentation::AotArtifact;
            } else {
                AddEntryDiagnostic(entry, BundleDiagnosticCode::InvalidDescriptor, "shader representation must be jit-source or aot-artifact");
                return;
            }
            shader::ShaderTarget target{};
            if (!ParseShaderTargetAttribute(attributes, target)) {
                AddEntryDiagnostic(entry, BundleDiagnosticCode::InvalidDescriptor, "shader target must be dxil or spirv");
                return;
            }
            entry.Descriptor = make_unique<ShaderAssetDescriptor>(mode, target);
            entry.State = BundleEntryState::Valid;
            return;
        }

        entry.State = BundleEntryState::Unknown;
    }

    bool ParseGuidAttribute(
        const vector<XmlAttribute>& attributes,
        std::string_view name,
        Guid& value,
        BundleDiagnosticCode missingCode,
        BundleDiagnosticCode invalidCode,
        bool fatal) {
        const XmlAttribute* attribute = FindAttribute(attributes, name);
        if (attribute == nullptr) {
            AddError(missingCode, "required XML GUID attribute is missing");
            return false;
        }
        if (!Guid::TryParse(attribute->Value, value)) {
            if (fatal) {
                AddError(invalidCode, "XML GUID attribute is invalid");
                return false;
            }
            return false;
        }
        return true;
    }

    bool ParseBundle(BundleCatalog& catalog) {
        std::string_view name;
        vector<XmlAttribute> attributes;
        bool selfClosing = false;
        if (!ParseStartTag(name, attributes, selfClosing) || name != "bundle" || selfClosing) {
            AddError(BundleDiagnosticCode::InvalidCatalog, "XML root must be a non-empty <bundle> element");
            return false;
        }

        const XmlAttribute* schema = FindAttribute(attributes, "schemaVersion");
        if (schema == nullptr || schema->Value != "1") {
            AddError(BundleDiagnosticCode::InvalidCatalog, "only schemaVersion=\"1\" is supported");
            return false;
        }
        if (!ParseGuidAttribute(
                attributes,
                "bundleId",
                catalog.Id,
                BundleDiagnosticCode::MissingBundleId,
                BundleDiagnosticCode::InvalidCatalog,
                true)) {
            return false;
        }
        if (catalog.Id.IsEmpty()) {
            AddError(BundleDiagnosticCode::InvalidCatalog, "XML bundleId must be a non-empty GUID");
            return false;
        }
        for (const XmlAttribute& attribute : attributes) {
            if (attribute.Name != "schemaVersion" && attribute.Name != "bundleId") {
                AddError(BundleDiagnosticCode::InvalidCatalog, "unknown attribute on XML <bundle> root");
                return false;
            }
        }

        SkipWhitespace();
        if (!ParseStartTag(name, attributes, selfClosing) || name != "assets") {
            AddError(BundleDiagnosticCode::InvalidCatalog, "XML <bundle> must contain <assets>");
            return false;
        }
        if (!attributes.empty()) {
            AddError(BundleDiagnosticCode::InvalidCatalog, "XML <assets> does not accept attributes");
            return false;
        }

        unordered_set<AssetId> ids;
        unordered_map<string, string> locatorValues;
        if (!selfClosing) {
            for (;;) {
                SkipWhitespace();
                if (_position + 2 <= _text.size() && _text.substr(_position, 2) == "</") {
                    if (!ParseEndTag("assets")) {
                        return false;
                    }
                    break;
                }
                if (_position >= _text.size() || _text[_position] != '<') {
                    AddError(BundleDiagnosticCode::InvalidCatalog, "XML <assets> contains non-whitespace text");
                    return false;
                }
                if (catalog.Entries.size() >= kMaxXmlCatalogEntries) {
                    AddError(BundleDiagnosticCode::InvalidCatalog, "XML Catalog exceeds the entry limit");
                    return false;
                }
                if (!ParseEntry(catalog.Entries, ids, locatorValues)) {
                    return false;
                }
            }
        }

        SkipWhitespace();
        if (!ParseEndTag("bundle")) {
            return false;
        }
        SkipWhitespace();
        if (_position != _text.size()) {
            AddError(BundleDiagnosticCode::InvalidCatalog, "trailing data follows XML root");
            return false;
        }
        return true;
    }

    bool ParseEntry(
        vector<BundleAssetEntry>& entries,
        unordered_set<AssetId>& ids,
        unordered_map<string, string>& locatorValues) {
        std::string_view name;
        vector<XmlAttribute> attributes;
        bool selfClosing = false;
        if (!ParseStartTag(name, attributes, selfClosing)) {
            return false;
        }
        BundleAssetEntry entry;
        entry.TypeName.assign(name.data(), name.size());

        const XmlAttribute* assetAttribute = FindAttribute(attributes, "assetId");
        if (assetAttribute == nullptr) {
            AddError(BundleDiagnosticCode::MissingAssetId, "XML asset entry is missing assetId");
            return false;
        }
        if (!Guid::TryParse(assetAttribute->Value, entry.Asset) || entry.Asset.IsEmpty()) {
            AddError(BundleDiagnosticCode::InvalidCatalog, "XML assetId is invalid");
            return false;
        }
        if (!ids.emplace(entry.Asset).second) {
            AddError(BundleDiagnosticCode::DuplicateAssetId, "XML Catalog contains duplicate assetId", entry.Asset);
            return false;
        }

        const XmlAttribute* typeAttribute = FindAttribute(attributes, "typeId");
        if (typeAttribute == nullptr || !Guid::TryParse(typeAttribute->Value, entry.TypeId) || entry.TypeId.IsEmpty()) {
            entry.State = BundleEntryState::Invalid;
            entry.Diagnostics.push_back(BundleDiagnostic{
                .Code = BundleDiagnosticCode::TypeIdMismatch,
                .Message = "entry typeId is missing or invalid",
                .Asset = entry.Asset,
            });
        }

        const XmlAttribute* pathAttribute = FindAttribute(attributes, "path");
        if (pathAttribute == nullptr) {
            entry.State = BundleEntryState::Invalid;
            entry.Diagnostics.push_back(BundleDiagnostic{
                .Code = BundleDiagnosticCode::InvalidLocator,
                .Message = "entry path is missing",
                .Asset = entry.Asset,
            });
        } else {
            entry.Locator = BundleLocator::TryCreate(pathAttribute->Value);
            if (!entry.Locator.has_value()) {
                entry.State = BundleEntryState::Invalid;
                entry.Diagnostics.push_back(BundleDiagnostic{
                    .Code = BundleDiagnosticCode::InvalidLocator,
                    .Message = "entry path is not a valid relative UTF-8 locator",
                    .Asset = entry.Asset,
                });
            } else {
                const string key = MakeBundleLocatorCollisionKey(entry.Locator->GetValue());
                auto [existing, inserted] = locatorValues.emplace(key, entry.Locator->GetValue());
                if (!inserted && existing->second != entry.Locator->GetValue()) {
                    entry.State = BundleEntryState::Invalid;
                    entry.Diagnostics.push_back(BundleDiagnostic{
                        .Code = BundleDiagnosticCode::LocatorCaseCollision,
                        .Message = "entry path collides with another path under case-insensitive comparison",
                        .Asset = entry.Asset,
                    });
                }
            }
        }

        if (entry.Diagnostics.empty()) {
            DecodeKnownDescriptor(entry, name, attributes);
        }
        if (entry.Diagnostics.empty() && entry.Descriptor == nullptr) {
            entry.State = BundleEntryState::Unknown;
        }
        if (!selfClosing) {
            SkipWhitespace();
            if (!ParseEndTag(name)) {
                return false;
            }
        }
        entries.push_back(std::move(entry));
        return true;
    }

    std::string_view _text;
    size_t _position{0};
    bool _failed{false};
    std::optional<BundleDiagnostic> _error;
};

}  // namespace

std::optional<BundleLocator> BundleLocator::TryCreate(std::string_view value) {
    if (!IsUtf8(value) || value.empty() || value.front() == '/' || value.front() == '\\') {
        return std::nullopt;
    }

    size_t componentStart = 0;
    for (size_t i = 0; i <= value.size(); ++i) {
        const bool atEnd = i == value.size();
        if (!atEnd && value[i] != '/') {
            if (value[i] == '\\' || value[i] == ':' || value[i] == '\0') {
                return std::nullopt;
            }
            continue;
        }

        const std::string_view component = value.substr(componentStart, i - componentStart);
        if (component.empty() || component == "." || component == "..") {
            return std::nullopt;
        }
        componentStart = i + 1;
    }

    return BundleLocator{string{value}};
}

string MakeBundleLocatorCollisionKey(std::string_view value) {
    string result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        if (character >= 'A' && character <= 'Z') {
            result.push_back(static_cast<char>(character - 'A' + 'a'));
        } else {
            result.push_back(static_cast<char>(character));
        }
    }
    return result;
}

MemoryBundleCatalogSource::MemoryBundleCatalogSource(BundleCatalog catalog)
    : _catalog(std::move(catalog)) {}

BundleCatalogSourceResult MemoryBundleCatalogSource::Read() {
    BundleCatalogSourceResult result;
    if (!_catalog.has_value()) {
        result.Diagnostics.push_back(BundleDiagnostic{
            .Code = BundleDiagnosticCode::InvalidSource,
            .Message = "memory Catalog source was already consumed",
            .Asset = std::nullopt,
        });
        return result;
    }
    result.Catalog = std::move(_catalog);
    return result;
}

XmlBundleCatalogSource::XmlBundleCatalogSource(string xml) : _xml(std::move(xml)) {}

XmlBundleCatalogSource::XmlBundleCatalogSource(std::filesystem::path path) : _path(std::move(path)) {}

BundleCatalogSourceResult XmlBundleCatalogSource::Read() {
    BundleCatalogSourceResult result;
    std::optional<string> xml;
    if (_xml.has_value()) {
        xml = std::move(_xml);
        _xml.reset();
    } else if (_path.has_value()) {
        const std::filesystem::path path = std::move(_path.value());
        _path.reset();
        xml = ReadTextFile(path);
        if (!xml.has_value()) {
            result.Diagnostics.push_back(BundleDiagnostic{
                .Code = BundleDiagnosticCode::InvalidSource,
                .Message = "XML Catalog source file could not be read",
                .Asset = std::nullopt,
            });
            return result;
        }
    } else {
        result.Diagnostics.push_back(BundleDiagnostic{
            .Code = BundleDiagnosticCode::InvalidSource,
            .Message = "XML Catalog source was already consumed",
            .Asset = std::nullopt,
        });
        return result;
    }

    if (xml->size() > kMaxXmlCatalogBytes) {
        result.Diagnostics.push_back(BundleDiagnostic{
            .Code = BundleDiagnosticCode::InvalidCatalog,
            .Message = "XML Catalog exceeds the document size limit",
            .Asset = std::nullopt,
        });
        return result;
    }
    BundleCatalog catalog;
    XmlCatalogReader reader{*xml};
    if (!reader.Parse(catalog, result.Diagnostics)) {
        return result;
    }
    result.Catalog = std::move(catalog);
    return result;
}

}  // namespace radray
