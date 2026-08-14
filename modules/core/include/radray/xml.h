#pragma once

#include <filesystem>
#include <string_view>

#include <radray/enum_flags.h>
#include <radray/types.h>

// pugixml 内部类型前置声明: 公开头不 include <pugixml.hpp>, 只暴露不透明指针。
namespace pugi {
class xml_document;
struct xml_node_struct;
struct xml_attribute_struct;
}  // namespace pugi

namespace radray {

/// 节点类型, 命名对齐 System.Xml.XmlNodeType, 覆盖 pugixml 能表达的节点种类。
enum class XmlNodeType : uint8_t {
    None,
    Element,
    Text,
    CDATA,
    Comment,
    ProcessingInstruction,
    Document,
    DocumentType,
    Declaration,
};

/// 在 pugixml 默认解析模式之上追加的解析选项 (默认模式已含 cdata/escapes/wconv/eol)。
enum class XmlParseFlag : uint32_t {
    None = 0x0,
    Comments = 0x1,
    WhitespaceText = 0x2,
    Declaration = 0x4,
    Doctype = 0x8,
    ProcessingInstruction = 0x10,
};

/// 序列化格式选项。未设置任何位时输出带缩进的形态。
enum class XmlFormatFlag : uint32_t {
    None = 0x0,
    Raw = 0x1,
    NoDeclaration = 0x2,
    WriteBom = 0x4,
    NoEscapes = 0x8,
    IndentAttributes = 0x10,
    NoEmptyElementTags = 0x20,
    SingleQuoteAttributes = 0x40,
};

template <>
struct is_flags<XmlParseFlag> : std::true_type {};

template <>
struct is_flags<XmlFormatFlag> : std::true_type {};

class XmlDocument;
class XmlAttribute;

/// 通用 DOM 节点句柄 (轻量 view, 不拥有底层内存, 生命周期依附于 XmlDocument)。
/// 命名与取值口径对齐 System.Xml.XmlNode: 元素/文本/注释/CDATA/PI/文档等节点统一经本类访问。
class XmlNode {
public:
    XmlNode() noexcept = default;
    explicit XmlNode(pugi::xml_node_struct* node) noexcept : _node(node) {}

    bool IsValid() const noexcept { return _node != nullptr; }
    explicit operator bool() const noexcept { return _node != nullptr; }

    bool operator==(const XmlNode& other) const noexcept { return _node == other._node; }
    bool operator!=(const XmlNode& other) const noexcept { return _node != other._node; }

    XmlNodeType NodeType() const noexcept;
    /// 节点名。文本/注释/CDATA/文档等无名字节点按 System.Xml 口径返回 "#text" 等。
    std::string_view Name() const noexcept;
    /// 节点值 (文本/CDATA/注释/PI/doctype); 元素与文档节点为空串。
    std::string_view Value() const noexcept;

    /// 文本值转数值 (读取本节点的 text())。转换失败或非文本节点返回 def。
    int32_t AsInt(int32_t def = 0) const noexcept;
    uint32_t AsUint(uint32_t def = 0) const noexcept;
    int64_t AsLongLong(int64_t def = 0) const noexcept;
    uint64_t AsULongLong(uint64_t def = 0) const noexcept;
    float AsFloat(float def = 0) const noexcept;
    double AsDouble(double def = 0) const noexcept;
    /// 首个字符在 '1tTyY' 集合内为 true, 否则 def。
    bool AsBool(bool def = false) const noexcept;

    XmlNode ParentNode() const noexcept;
    XmlNode FirstChild() const noexcept;
    XmlNode LastChild() const noexcept;
    XmlNode NextSibling() const noexcept;
    XmlNode PreviousSibling() const noexcept;
    bool HasChildNodes() const noexcept;

    /// 直接子节点 (按文档序)。
    vector<XmlNode> ChildNodes() const noexcept;

    /// 串联全部后代文本 (Text/CDATA) 节点的内容。
    string InnerText() const;
    /// 序列化全部子节点。
    string InnerXml(EnumFlags<XmlFormatFlag> flags = {}) const;
    /// 序列化本节点 (含自身标签)。
    string OuterXml(EnumFlags<XmlFormatFlag> flags = {}) const;

    /// 追加/前置子节点 (child 会被移动到本节点下, 脱离原父节点)。
    XmlNode AppendChild(XmlNode child);
    XmlNode PrependChild(XmlNode child);
    bool RemoveChild(XmlNode child);
    void RemoveAll();

protected:
    pugi::xml_node_struct* _node{nullptr};
};

/// 元素节点句柄。对齐 System.Xml.XmlElement: 属性访问与子节点管理。
class XmlElement : public XmlNode {
public:
    XmlElement() noexcept = default;
    explicit XmlElement(pugi::xml_node_struct* node) noexcept : XmlNode(node) {}
    explicit XmlElement(XmlNode node) noexcept : XmlNode(node) {}

    using XmlNode::AppendChild;

    bool HasAttribute(std::string_view name) const noexcept;
    /// 属性值; 不存在返回空串。
    string GetAttribute(std::string_view name) const;
    /// 按名取属性句柄, 未命中返回空句柄。
    XmlAttribute GetAttributeNode(std::string_view name) const noexcept;
    /// 设置属性值; 不存在则新建。
    void SetAttribute(std::string_view name, std::string_view value);
    bool RemoveAttribute(std::string_view name);
    void RemoveAllAttributes();

    vector<XmlAttribute> Attributes() const noexcept;

    /// 按名取第一个直接子元素, 未命中返回空句柄。
    XmlElement Child(std::string_view name) const noexcept;
    /// 收集按名匹配的直接子元素 (文档序)。
    vector<XmlElement> Children(std::string_view name) const noexcept;

    /// 创建并追加一个具名子元素, 返回其句柄。
    XmlElement AppendChild(std::string_view name);
};

/// 属性句柄 (轻量 view)。对齐 System.Xml.XmlAttribute。
class XmlAttribute {
public:
    XmlAttribute() noexcept = default;
    explicit XmlAttribute(pugi::xml_attribute_struct* attr) noexcept : _attr(attr) {}

    bool IsValid() const noexcept { return _attr != nullptr; }
    explicit operator bool() const noexcept { return _attr != nullptr; }

    bool operator==(const XmlAttribute& other) const noexcept { return _attr == other._attr; }
    bool operator!=(const XmlAttribute& other) const noexcept { return _attr != other._attr; }

    std::string_view Name() const noexcept;
    std::string_view Value() const noexcept;
    void SetValue(std::string_view value);

    /// 属性值转数值。转换失败或属性为空返回 def。
    int32_t AsInt(int32_t def = 0) const noexcept;
    uint32_t AsUint(uint32_t def = 0) const noexcept;
    int64_t AsLongLong(int64_t def = 0) const noexcept;
    uint64_t AsULongLong(uint64_t def = 0) const noexcept;
    float AsFloat(float def = 0) const noexcept;
    double AsDouble(double def = 0) const noexcept;
    /// 首个字符在 '1tTyY' 集合内为 true, 否则 def。
    bool AsBool(bool def = false) const noexcept;

private:
    pugi::xml_attribute_struct* _attr{nullptr};
};

/// 拥有式 XML 文档 (对齐 System.Xml.XmlDocument)。move-only, 析构释放底层内存;
/// 节点/属性句柄生命周期依附于它。
class XmlDocument {
public:
    XmlDocument();
    ~XmlDocument();
    XmlDocument(const XmlDocument&) = delete;
    XmlDocument& operator=(const XmlDocument&) = delete;
    XmlDocument(XmlDocument&& other) noexcept;
    XmlDocument& operator=(XmlDocument&& other) noexcept;

    bool IsValid() const noexcept { return _document != nullptr; }

    /// 解析 XML 文本 / 读文件并解析。失败返回 false 并填充 outError (可空)。
    bool LoadXml(std::string_view xml, EnumFlags<XmlParseFlag> flags = {}, string* outError = nullptr);
    bool Load(const std::filesystem::path& path, EnumFlags<XmlParseFlag> flags = {}, string* outError = nullptr);

    /// 序列化并写文件。成功返回 true。
    bool Save(const std::filesystem::path& path, EnumFlags<XmlFormatFlag> flags = {}) const;

    /// 文档根元素。空文档返回空句柄。
    XmlElement DocumentElement() const noexcept;

    /// 序列化整个文档 (含 XML 声明, 除非设 NoDeclaration)。
    string OuterXml(EnumFlags<XmlFormatFlag> flags = {}) const;

    /// 清空为未加载态 (DOM 节点全部移除)。
    void Reset() noexcept;

    /// 创建未挂接节点 (作为文档根节点的子节点暂存, AppendChild 时再移动到目标下)。
    XmlElement CreateElement(std::string_view name);
    XmlNode CreateTextNode(std::string_view text);
    XmlNode CreateComment(std::string_view text);
    XmlNode CreateCDataSection(std::string_view text);

private:
    unique_ptr<pugi::xml_document> _document;
};

}  // namespace radray
