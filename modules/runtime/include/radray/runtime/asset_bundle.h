#pragma once

#include <filesystem>
#include <optional>
#include <string_view>
#include <utility>

#include <radray/nullable.h>
#include <radray/runtime/asset.h>
#include <radray/types.h>

namespace radray {

class AssetManager;
struct BundleSlot;

/// 资产 Bundle 的持久标识。它和 AssetId 一样是 Manifest 中的显式 GUID。
using BundleId = Guid;

/// Catalog entry 的类型擦除描述。描述对象由 Catalog 自己拥有，loader 按值复制需要的数据。
class AssetDescriptor {
public:
    AssetDescriptor() noexcept = default;
    AssetDescriptor(const AssetDescriptor&) = delete;
    AssetDescriptor(AssetDescriptor&&) = delete;
    AssetDescriptor& operator=(const AssetDescriptor&) = delete;
    AssetDescriptor& operator=(AssetDescriptor&&) = delete;
    virtual ~AssetDescriptor() noexcept = default;

    virtual RuntimeTypeId GetTypeId() const noexcept = 0;

    /// 为异步 loader 创建自有副本。旧的/测试 descriptor 可以返回 nullptr；内置持久化
    /// descriptor 都实现此接口，AssetManager 才会把它交给安全快照 loader。
    virtual unique_ptr<const AssetDescriptor> Clone() const { return nullptr; }
};

/// Entry 是否可以交给 typed loader。
enum class BundleEntryState {
    Valid,
    Unknown,
    Invalid,
};

/// Bundle/Catalog 结构化诊断码。消息是给人看的，调用方应按 Code 分支。
enum class BundleDiagnosticCode {
    InvalidSource,
    InvalidRoot,
    InvalidCatalog,
    MissingBundleId,
    DuplicateBundleId,
    MissingAssetId,
    DuplicateAssetId,
    AssetIdAlreadyInUse,
    InvalidLocator,
    LocatorCaseCollision,
    UnknownAssetType,
    InvalidDescriptor,
    TypeIdMismatch,
};

struct BundleDiagnostic {
    BundleDiagnosticCode Code{BundleDiagnosticCode::InvalidCatalog};
    string Message;
    std::optional<AssetId> Asset;
};

/// Bundle 内部的主定位符。它只是一段规范化前的逻辑相对路径，不参与身份计算。
class BundleLocator {
public:
    static std::optional<BundleLocator> TryCreate(std::string_view value);

    const string& GetValue() const noexcept { return _value; }

private:
    explicit BundleLocator(string value) noexcept : _value(std::move(value)) {}

    string _value;
};

/// 用于检测 Windows/POSIX 大小写碰撞的稳定 ASCII key。原始 locator 的大小写保持不变。
string MakeBundleLocatorCollisionKey(std::string_view value);

/// 一个 Manifest entry 的编码无关表示。
///
/// AssetId 是唯一必须跨 Bundle 保持稳定的身份；Locator、TypeName 和 Descriptor 都不是身份。
struct BundleAssetEntry {
    AssetId Asset{};
    RuntimeTypeId TypeId{};
    string TypeName;
    std::optional<BundleLocator> Locator;
    BundleEntryState State{BundleEntryState::Invalid};
    unique_ptr<const AssetDescriptor> Descriptor;
    vector<BundleDiagnostic> Diagnostics;
};

/// 从 Catalog 提交给安全 loader 的一次性值快照。它不含 BundleRef，也不含 Catalog 裸指针；
/// loader 协程可以在 Bundle 被 Pump 摘除后继续使用自己的副本。
struct BundleAssetLoadData {
    BundleAssetEntry Entry;
    std::filesystem::path Root;
};

/// 只读 Catalog 的标准值模型。它不包含 XML DOM，也不拥有 Bundle root 或 payload storage。
struct BundleCatalog {
    BundleId Id{};
    vector<BundleAssetEntry> Entries;
};

struct BundleCatalogSourceResult {
    std::optional<BundleCatalog> Catalog;
    vector<BundleDiagnostic> Diagnostics;

    bool IsSuccess() const noexcept { return Catalog.has_value(); }
};

/// Catalog 来源的抽象层。V1 已实现内存/XML 来源；后续二进制来源只需产出同一个值模型。
class BundleCatalogSource {
public:
    BundleCatalogSource() noexcept = default;
    BundleCatalogSource(const BundleCatalogSource&) = delete;
    BundleCatalogSource(BundleCatalogSource&&) = delete;
    BundleCatalogSource& operator=(const BundleCatalogSource&) = delete;
    BundleCatalogSource& operator=(BundleCatalogSource&&) = delete;
    virtual ~BundleCatalogSource() noexcept = default;

    virtual BundleCatalogSourceResult Read() = 0;
};

/// 测试和后续非 XML 来源可复用的同步内存 Catalog source。
class MemoryBundleCatalogSource final : public BundleCatalogSource {
public:
    explicit MemoryBundleCatalogSource(BundleCatalog catalog);

    BundleCatalogSourceResult Read() override;

private:
    std::optional<BundleCatalog> _catalog;
};

/// XML Catalog V1 source。它只接受本文档定义的严格有限子集，不保留 XML DOM。
class XmlBundleCatalogSource final : public BundleCatalogSource {
public:
    explicit XmlBundleCatalogSource(string xml);
    explicit XmlBundleCatalogSource(std::filesystem::path path);

    BundleCatalogSourceResult Read() override;

private:
    std::optional<string> _xml;
    std::optional<std::filesystem::path> _path;
};

/// Bundle 的轻量 RAII 引用。它保住 Catalog，不保住 payload storage，也不被 Asset slot 持有。
class BundleRef {
public:
    BundleRef() noexcept = default;
    BundleRef(std::nullptr_t) noexcept {}
    BundleRef(const BundleRef& other) noexcept;
    BundleRef(BundleRef&& other) noexcept;
    BundleRef& operator=(const BundleRef& other) noexcept;
    BundleRef& operator=(BundleRef&& other) noexcept;
    ~BundleRef() noexcept;

    bool IsValid() const noexcept { return _slot != nullptr; }
    explicit operator bool() const noexcept { return IsValid(); }

    const BundleId& GetBundleId() const noexcept;
    Nullable<const std::filesystem::path*> GetRoot() const noexcept;
    Nullable<const BundleCatalog*> GetCatalog() const noexcept;

    bool operator==(const BundleRef& other) const noexcept {
        return _manager == other._manager && _slot == other._slot;
    }

    void Reset() noexcept;

private:
    friend class AssetManager;

    BundleRef(AssetManager* manager, BundleSlot* slot) noexcept;

    AssetManager* _manager{nullptr};
    BundleSlot* _slot{nullptr};
};

struct BundleMountResult {
    std::optional<BundleRef> Reference;
    vector<BundleDiagnostic> Diagnostics;

    bool IsSuccess() const noexcept { return Reference.has_value(); }
};

}  // namespace radray
