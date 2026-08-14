#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

#include <radray/types.h>
#include <radray/xml.h>

#include <radray/runtime/asset.h>

namespace radray {

/// bundle.xml 清单的常驻表示。XML DOM 文档本身是后备存储: 读取 = 解析 + 校验 + 在 DOM
/// 上建索引, 写回 = 序列化同一份 DOM —— 未触碰节点的顺序、内容与注释逐字节保持, 这是选
/// XML 的 merge 友好动机对写回策略的机械要求。设计取舍:
/// docs/adr/0037-manifest-dom-is-backing-store.md, 现状契约:
/// docs/architecture/asset-database.md。
///
/// 清单层的身份契约只覆盖条目的元素名、guid、path 三样; 其余属性与全部子节点原样保留,
/// 语义归各资产 loader (基础值的统一编码与读写 helper 见本文件尾部)。
///
/// 【DOM 节点访问只允许发生在拥有 AssetDatabase 的主线程】见 AssetDatabase::ResolvedAsset。
class AssetBundleManifest {
public:
    AssetBundleManifest() = default;
    AssetBundleManifest(const AssetBundleManifest&) = delete;
    AssetBundleManifest(AssetBundleManifest&&) noexcept = default;
    AssetBundleManifest& operator=(const AssetBundleManifest&) = delete;
    AssetBundleManifest& operator=(AssetBundleManifest&&) noexcept = default;

    /// 解析并校验 bundle.xml。失败返回 false, 本对象清空为未加载态 —— 失败后不存在
    /// "半套索引"可用。
    bool LoadFromFile(const std::filesystem::path& path, string& outError);

    /// 把 DOM 落盘回 LoadFromFile 的路径。未加载或落盘失败返回 false。
    bool Save(string& outError) const;

    bool IsLoaded() const noexcept { return _loaded; }

    /// <bundle> 根元素。未加载时返回空句柄。
    XmlElement Root() const noexcept;

    /// 按 GUID / 存储形态的 bundle 内相对路径查条目节点。未命中返回 nullopt。
    /// path 查表按大小写不敏感口径 (Windows 上两个只差大小写的 path 指向同一文件)。
    std::optional<XmlElement> FindByGuid(const AssetId& id) const noexcept;
    std::optional<XmlElement> FindByPath(std::string_view relPath) const noexcept;

    /// 把新条目追加到 <bundle> 末尾并进索引。
    /// 【前置条件】type 非空 (条目元素名即资产类型); relPath 必须是规范化后的存储形态
    /// (见 IsValidStoredPath / NormalizeEntryPath); guid 必须未被本清单使用 —— 违反是
    /// 编程错误, abort。
    XmlElement AppendEntry(std::string_view type, const AssetId& guid, std::string_view relPath);

private:
    void Reset() noexcept;

    XmlDocument _document;
    std::filesystem::path _path;
    unordered_map<AssetId, XmlElement> _byGuid;
    /// key: 小写折叠后的存储形态 path。查找口径与写入口径一致 (大小写不敏感)。
    unordered_map<string, XmlElement> _byPath;
    bool _loaded{false};
};

/// D10 基础类型的叶子元素名。叶子元素: 元素名 = 基础类型, name 属性 = 字段名,
/// value 属性 = 值。
inline constexpr std::string_view kLeafTypeString = "string";
inline constexpr std::string_view kLeafTypeInt = "int";
inline constexpr std::string_view kLeafTypeFloat = "float";
inline constexpr std::string_view kLeafTypeBool = "bool";
inline constexpr std::string_view kLeafTypeGuid = "guid";

/// 条目子节点基础值的统一编码 helper (docs/adr/0037-manifest-dom-is-backing-store.md)。
/// 语义归 loader, 但值的读写形态统一:
/// - 叶子: 元素名是五种基础类型之一, name 是字段名, value 是值;
/// - 非叶子: 元素名直接是字段名 (如 <setting>), loader 取 node.child("setting")
///   后再在那一层读写;
/// - 同名元素重复出现即列表, 列表读法收集全部同名项, 不引入 <list> 包装。
/// 坏值 (如 <int> 里放非数字) 一律按"读不到"处理 (nullopt / 从列表跳过) —— Mount 不校验
/// 子节点结构, 由 loader 在加载时报错。
std::optional<string> ReadString(const XmlElement& node, std::string_view name);
std::optional<int64_t> ReadInt(const XmlElement& node, std::string_view name);
std::optional<float> ReadFloat(const XmlElement& node, std::string_view name);
std::optional<bool> ReadBool(const XmlElement& node, std::string_view name);
/// guid 值读取宽容 N/D/B/P (Guid::TryParse), 写回固定 D 格式小写 (Guid::ToString)。
std::optional<Guid> ReadGuid(const XmlElement& node, std::string_view name);

vector<string> ReadStringList(const XmlElement& node, std::string_view name);
vector<int64_t> ReadIntList(const XmlElement& node, std::string_view name);
vector<float> ReadFloatList(const XmlElement& node, std::string_view name);
vector<bool> ReadBoolList(const XmlElement& node, std::string_view name);
vector<Guid> ReadGuidList(const XmlElement& node, std::string_view name);

/// 写入 = 命中同名同型叶子则原位改 value 属性, 否则在 node 子节点末尾追加新叶子。
void WriteString(XmlElement& node, std::string_view name, std::string_view value);
void WriteInt(XmlElement& node, std::string_view name, int64_t value);
void WriteFloat(XmlElement& node, std::string_view name, float value);
void WriteBool(XmlElement& node, std::string_view name, bool value);
void WriteGuid(XmlElement& node, std::string_view name, const Guid& value);

/// AddEntry 输入路径的规范化: 宽容接受 \ 与 / 混用、重复分隔符与 "." 段, 输出 '/'
/// 分隔的存储形态。绝对路径 (开头 /)、盘符 (含 ':')、空段结果与 ".." 一律拒绝。
std::optional<string> NormalizeEntryPath(std::string_view input) noexcept;

/// 存储形态校验 (Load/Mount 时清单里 path 的硬门槛): '/'-分隔、非空、无 '\'、无 ':'
/// (盘符)、无绝对路径、无 "." / ".." / 空段、无开头 "./"、无尾斜杠。
bool IsValidStoredPath(std::string_view path) noexcept;

}  // namespace radray
