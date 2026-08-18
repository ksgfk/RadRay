#include <radray/xml.h>

#include <fmt/format.h>

#include <pugixml.hpp>

namespace radray {
namespace {

unsigned int ToPugiParse(EnumFlags<XmlParseFlag> flags) noexcept {
    unsigned int options = pugi::parse_default;
    if (flags.HasFlag(XmlParseFlag::Comments)) {
        options |= pugi::parse_comments;
    }
    if (flags.HasFlag(XmlParseFlag::WhitespaceText)) {
        options |= pugi::parse_ws_pcdata;
    }
    if (flags.HasFlag(XmlParseFlag::Declaration)) {
        options |= pugi::parse_declaration;
    }
    if (flags.HasFlag(XmlParseFlag::Doctype)) {
        options |= pugi::parse_doctype;
    }
    if (flags.HasFlag(XmlParseFlag::ProcessingInstruction)) {
        options |= pugi::parse_pi;
    }
    return options;
}

unsigned int ToPugiFormat(EnumFlags<XmlFormatFlag> flags) noexcept {
    unsigned int format = flags.HasFlag(XmlFormatFlag::Raw) ? pugi::format_raw : pugi::format_indent;
    if (flags.HasFlag(XmlFormatFlag::NoDeclaration)) {
        format |= pugi::format_no_declaration;
    }
    if (flags.HasFlag(XmlFormatFlag::WriteBom)) {
        format |= pugi::format_write_bom;
    }
    if (flags.HasFlag(XmlFormatFlag::NoEscapes)) {
        format |= pugi::format_no_escapes;
    }
    if (flags.HasFlag(XmlFormatFlag::IndentAttributes)) {
        format |= pugi::format_indent_attributes;
    }
    if (flags.HasFlag(XmlFormatFlag::NoEmptyElementTags)) {
        format |= pugi::format_no_empty_element_tags;
    }
    if (flags.HasFlag(XmlFormatFlag::SingleQuoteAttributes)) {
        format |= pugi::format_attribute_single_quote;
    }
    return format;
}

XmlNodeType FromPugiNodeType(pugi::xml_node_type type) noexcept {
    switch (type) {
        case pugi::node_element:
            return XmlNodeType::Element;
        case pugi::node_pcdata:
            return XmlNodeType::Text;
        case pugi::node_cdata:
            return XmlNodeType::CDATA;
        case pugi::node_comment:
            return XmlNodeType::Comment;
        case pugi::node_pi:
            return XmlNodeType::ProcessingInstruction;
        case pugi::node_document:
            return XmlNodeType::Document;
        case pugi::node_doctype:
            return XmlNodeType::DocumentType;
        case pugi::node_declaration:
            return XmlNodeType::Declaration;
        case pugi::node_null:
            break;
    }
    return XmlNodeType::None;
}

/// 序列化到 string 的 writer (pugixml::xml_writer 回调)。
class StringWriter final : public pugi::xml_writer {
public:
    explicit StringWriter(string& out) noexcept : _out(out) {}

    void write(const void* data, size_t size) override {
        _out.append(static_cast<const char*>(data), size);
    }

private:
    string& _out;
};

void AppendInnerText(pugi::xml_node node, string& out) {
    for (pugi::xml_node child = node.first_child(); child; child = child.next_sibling()) {
        const pugi::xml_node_type type = child.type();
        if (type == pugi::node_pcdata || type == pugi::node_cdata) {
            out.append(child.value());
        } else {
            AppendInnerText(child, out);
        }
    }
}

}  // namespace

// -------------------------------- XmlNode --------------------------------

XmlNodeType XmlNode::NodeType() const noexcept {
    return FromPugiNodeType(pugi::xml_node{_node}.type());
}

std::string_view XmlNode::Name() const noexcept {
    switch (pugi::xml_node{_node}.type()) {
        case pugi::node_document:
            return "#document";
        case pugi::node_pcdata:
            return "#text";
        case pugi::node_cdata:
            return "#cdata-section";
        case pugi::node_comment:
            return "#comment";
        default:
            return pugi::xml_node{_node}.name();
    }
}

std::string_view XmlNode::Value() const noexcept {
    return pugi::xml_node{_node}.value();
}

int32_t XmlNode::AsInt(int32_t def) const noexcept {
    return pugi::xml_node{_node}.text().as_int(def);
}

uint32_t XmlNode::AsUint(uint32_t def) const noexcept {
    return pugi::xml_node{_node}.text().as_uint(def);
}

int64_t XmlNode::AsLongLong(int64_t def) const noexcept {
    return pugi::xml_node{_node}.text().as_llong(def);
}

uint64_t XmlNode::AsULongLong(uint64_t def) const noexcept {
    return pugi::xml_node{_node}.text().as_ullong(def);
}

float XmlNode::AsFloat(float def) const noexcept {
    return pugi::xml_node{_node}.text().as_float(def);
}

double XmlNode::AsDouble(double def) const noexcept {
    return pugi::xml_node{_node}.text().as_double(def);
}

bool XmlNode::AsBool(bool def) const noexcept {
    return pugi::xml_node{_node}.text().as_bool(def);
}

XmlNode XmlNode::ParentNode() const noexcept {
    return XmlNode{pugi::xml_node{_node}.parent().internal_object()};
}

XmlNode XmlNode::FirstChild() const noexcept {
    return XmlNode{pugi::xml_node{_node}.first_child().internal_object()};
}

XmlNode XmlNode::LastChild() const noexcept {
    return XmlNode{pugi::xml_node{_node}.last_child().internal_object()};
}

XmlNode XmlNode::NextSibling() const noexcept {
    return XmlNode{pugi::xml_node{_node}.next_sibling().internal_object()};
}

XmlNode XmlNode::PreviousSibling() const noexcept {
    return XmlNode{pugi::xml_node{_node}.previous_sibling().internal_object()};
}

bool XmlNode::HasChildNodes() const noexcept {
    return !pugi::xml_node{_node}.first_child().empty();
}

vector<XmlNode> XmlNode::ChildNodes() const noexcept {
    vector<XmlNode> out;
    for (pugi::xml_node child = pugi::xml_node{_node}.first_child(); child; child = child.next_sibling()) {
        out.emplace_back(child.internal_object());
    }
    return out;
}

string XmlNode::InnerText() const {
    const pugi::xml_node self{_node};
    const pugi::xml_node_type type = self.type();
    if (type == pugi::node_pcdata || type == pugi::node_cdata) {
        return self.value();
    }
    string out;
    AppendInnerText(self, out);
    return out;
}

string XmlNode::InnerXml(EnumFlags<XmlFormatFlag> flags) const {
    string out;
    StringWriter writer{out};
    const unsigned int format = ToPugiFormat(flags);
    for (pugi::xml_node child = pugi::xml_node{_node}.first_child(); child; child = child.next_sibling()) {
        child.print(writer, "  ", format, pugi::encoding_auto, 0);
    }
    return out;
}

string XmlNode::OuterXml(EnumFlags<XmlFormatFlag> flags) const {
    string out;
    StringWriter writer{out};
    pugi::xml_node{_node}.print(writer, "  ", ToPugiFormat(flags), pugi::encoding_auto, 0);
    return out;
}

XmlNode XmlNode::AppendChild(XmlNode child) {
    return XmlNode{pugi::xml_node{_node}.append_move(pugi::xml_node{child._node}).internal_object()};
}

XmlNode XmlNode::PrependChild(XmlNode child) {
    return XmlNode{pugi::xml_node{_node}.prepend_move(pugi::xml_node{child._node}).internal_object()};
}

bool XmlNode::RemoveChild(XmlNode child) {
    return pugi::xml_node{_node}.remove_child(pugi::xml_node{child._node});
}

void XmlNode::RemoveAll() {
    pugi::xml_node{_node}.remove_children();
}

// ------------------------------- XmlElement -------------------------------

bool XmlElement::HasAttribute(std::string_view name) const noexcept {
    return !pugi::xml_node{_node}.attribute(pugi::string_view_t{name.data(), name.size()}).empty();
}

string XmlElement::GetAttribute(std::string_view name) const {
    const pugi::xml_attribute attr = pugi::xml_node{_node}.attribute(pugi::string_view_t{name.data(), name.size()});
    if (!attr) {
        return {};
    }
    return attr.value();
}

XmlAttribute XmlElement::GetAttributeNode(std::string_view name) const noexcept {
    return XmlAttribute{pugi::xml_node{_node}.attribute(pugi::string_view_t{name.data(), name.size()}).internal_object()};
}

void XmlElement::SetAttribute(std::string_view name, std::string_view value) {
    const pugi::string_view_t pugiName{name.data(), name.size()};
    pugi::xml_attribute attr = pugi::xml_node{_node}.attribute(pugiName);
    if (!attr) {
        attr = pugi::xml_node{_node}.append_attribute(pugiName);
    }
    attr.set_value(pugi::string_view_t{value.data(), value.size()});
}

bool XmlElement::RemoveAttribute(std::string_view name) {
    return pugi::xml_node{_node}.remove_attribute(pugi::string_view_t{name.data(), name.size()});
}

void XmlElement::RemoveAllAttributes() {
    pugi::xml_node{_node}.remove_attributes();
}

vector<XmlAttribute> XmlElement::Attributes() const noexcept {
    vector<XmlAttribute> out;
    for (pugi::xml_attribute attr = pugi::xml_node{_node}.first_attribute(); attr; attr = attr.next_attribute()) {
        out.emplace_back(attr.internal_object());
    }
    return out;
}

XmlElement XmlElement::Child(std::string_view name) const noexcept {
    for (pugi::xml_node child = pugi::xml_node{_node}.first_child(); child; child = child.next_sibling()) {
        if (child.type() == pugi::node_element && pugi::string_view_t(child.name()) == pugi::string_view_t{name.data(), name.size()}) {
            return XmlElement{child.internal_object()};
        }
    }
    return {};
}

vector<XmlElement> XmlElement::Children(std::string_view name) const noexcept {
    vector<XmlElement> out;
    const pugi::string_view_t target{name.data(), name.size()};
    for (pugi::xml_node child = pugi::xml_node{_node}.first_child(); child; child = child.next_sibling()) {
        if (child.type() == pugi::node_element && pugi::string_view_t(child.name()) == target) {
            out.emplace_back(child.internal_object());
        }
    }
    return out;
}

XmlElement XmlElement::AppendChild(std::string_view name) {
    return XmlElement{pugi::xml_node{_node}.append_child(pugi::string_view_t{name.data(), name.size()}).internal_object()};
}

// ------------------------------ XmlAttribute ------------------------------

std::string_view XmlAttribute::Name() const noexcept {
    return pugi::xml_attribute{_attr}.name();
}

std::string_view XmlAttribute::Value() const noexcept {
    return pugi::xml_attribute{_attr}.value();
}

void XmlAttribute::SetValue(std::string_view value) {
    pugi::xml_attribute{_attr}.set_value(pugi::string_view_t{value.data(), value.size()});
}

int32_t XmlAttribute::AsInt(int32_t def) const noexcept {
    return pugi::xml_attribute{_attr}.as_int(def);
}

uint32_t XmlAttribute::AsUint(uint32_t def) const noexcept {
    return pugi::xml_attribute{_attr}.as_uint(def);
}

int64_t XmlAttribute::AsLongLong(int64_t def) const noexcept {
    return pugi::xml_attribute{_attr}.as_llong(def);
}

uint64_t XmlAttribute::AsULongLong(uint64_t def) const noexcept {
    return pugi::xml_attribute{_attr}.as_ullong(def);
}

float XmlAttribute::AsFloat(float def) const noexcept {
    return pugi::xml_attribute{_attr}.as_float(def);
}

double XmlAttribute::AsDouble(double def) const noexcept {
    return pugi::xml_attribute{_attr}.as_double(def);
}

bool XmlAttribute::AsBool(bool def) const noexcept {
    return pugi::xml_attribute{_attr}.as_bool(def);
}

// ------------------------------ XmlDocument ------------------------------

XmlDocument::XmlDocument() : _document(make_unique<pugi::xml_document>()) {}

XmlDocument::~XmlDocument() = default;

XmlDocument::XmlDocument(XmlDocument&& other) noexcept = default;

XmlDocument& XmlDocument::operator=(XmlDocument&& other) noexcept = default;

bool XmlDocument::LoadXml(std::string_view xml, EnumFlags<XmlParseFlag> flags, string* outError) {
    const void* data = xml.empty() ? "" : xml.data();
    const pugi::xml_parse_result result = _document->load_buffer(data, xml.size(), ToPugiParse(flags), pugi::encoding_auto);
    if (!result) {
        if (outError != nullptr) {
            *outError = fmt::format("XML parse failed at offset {}: {}", result.offset, result.description());
        }
        return false;
    }
    return true;
}

bool XmlDocument::Load(const std::filesystem::path& path, EnumFlags<XmlParseFlag> flags, string* outError) {
    // path.c_str() 在 Windows 上是 wchar_t*, 命中 pugixml 的宽字符重载 (内部转 UTF-8
    // 打开), 避免窄字符重载的 ANSI 代码页路径丢失; POSIX 上命中 char 重载。
    const pugi::xml_parse_result result = _document->load_file(path.c_str(), ToPugiParse(flags), pugi::encoding_auto);
    if (!result) {
        if (outError != nullptr) {
            *outError = fmt::format("{}: XML parse failed at offset {}: {}", path.string(), result.offset, result.description());
        }
        return false;
    }
    return true;
}

bool XmlDocument::Save(const std::filesystem::path& path, EnumFlags<XmlFormatFlag> flags) const {
    return _document->save_file(path.c_str(), "  ", ToPugiFormat(flags), pugi::encoding_auto);
}

XmlElement XmlDocument::DocumentElement() const noexcept {
    return XmlElement{_document->document_element().internal_object()};
}

string XmlDocument::OuterXml(EnumFlags<XmlFormatFlag> flags) const {
    string out;
    StringWriter writer{out};
    _document->save(writer, "  ", ToPugiFormat(flags), pugi::encoding_auto);
    return out;
}

void XmlDocument::Reset() noexcept {
    _document->reset();
}

XmlElement XmlDocument::CreateElement(std::string_view name) {
    return XmlElement{_document->append_child(pugi::string_view_t{name.data(), name.size()}).internal_object()};
}

XmlNode XmlDocument::CreateTextNode(std::string_view text) {
    pugi::xml_node node = _document->append_child(pugi::node_pcdata);
    node.set_value(pugi::string_view_t{text.data(), text.size()});
    return XmlNode{node.internal_object()};
}

XmlNode XmlDocument::CreateComment(std::string_view text) {
    pugi::xml_node node = _document->append_child(pugi::node_comment);
    node.set_value(pugi::string_view_t{text.data(), text.size()});
    return XmlNode{node.internal_object()};
}

XmlNode XmlDocument::CreateCDataSection(std::string_view text) {
    pugi::xml_node node = _document->append_child(pugi::node_cdata);
    node.set_value(pugi::string_view_t{text.data(), text.size()});
    return XmlNode{node.internal_object()};
}

}  // namespace radray
