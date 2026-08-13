#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

#include <radray/coroutine.h>
#include <radray/types.h>

#include <radray/runtime/asset.h>
#include <radray/runtime/asset_bundle_manifest.h>
#include <radray/runtime/asset_manager.h>

namespace radray {

/// 开发时资产身份门面: Mount 扫描资产根下所有 bundle.xml 建索引, 提供查询、登记与落盘。
/// 资产按 bundle (含 bundle.xml 的目录) 组织, 清单 GUID 是入库资产的永久身份 —— 身份规则
/// 与错误分级: docs/adr/0036-per-bundle-manifest-is-asset-identity-authority.md, 现状契约:
/// docs/architecture/asset-database.md。
///
/// 本头包含 asset_manager.h 只为 LoadFromDatabase 桥接签名所需的 AssetLoadResult /
/// AssetManager; 依赖是单向的 (asset_manager.h 不认识本类), AssetManager 与 AssetDatabase
/// 各自单一职责 (生命周期 / 清单身份), 两者互相不知道对方的存在。
class AssetDatabase {
public:
    AssetDatabase() = default;
    AssetDatabase(const AssetDatabase&) = delete;
    AssetDatabase(AssetDatabase&&) = delete;
    AssetDatabase& operator=(const AssetDatabase&) = delete;
    AssetDatabase& operator=(AssetDatabase&&) = delete;

    /// 扫描 assetRoot 下所有 bundle.xml 并按错误分级校验后建索引。结构性错误 (嵌套
    /// bundle、GUID 跨 bundle 重复、path 重复、坏清单) 令 Mount 整体硬失败且索引为空;
    /// 内容性缺损 (条目 type 无注册 loader、条目 path 磁盘上不存在) 只记 warning, 条目
    /// 照常进索引 —— 给"先改清单、后放文件"留活路。
    /// loader 注册表跨 Mount 保留, 装配代码应"先 RegisterLoader、后 Mount"。
    bool Mount(const std::filesystem::path& assetRoot, string& outError);

    struct ResolvedAsset {
        /// assetRoot / bundle 目录 / path 拼出的绝对路径。
        std::filesystem::path AbsolutePath;
        /// 条目元素名 (资产类型)。指向常驻 DOM, 在 AssetDatabase 存活期内有效。
        std::string_view Type;
        /// 条目节点。子节点语义由 loader 自行解析。
        ///
        /// 【DOM 访问只允许发生在构造 AssetLoadRequest 的主线程时刻】loader 协程一旦挂起
        /// 去做异步 IO 就不得再碰 DOM (AddEntry 可能并发修改它), 所需参数必须在构造 task
        /// 前拷出。见 docs/architecture/asset-database.md。
        pugi::xml_node Node;
    };

    /// 按 GUID 解析条目。未命中返回 nullopt。纯查表, 不碰磁盘。
    std::optional<ResolvedAsset> Resolve(const AssetId& id) const noexcept;

    /// 按 bundle 名 + bundle 内路径查 GUID。path 与 bundle 名都按大小写不敏感口径比较。
    std::optional<AssetId> FindByPath(std::string_view bundleName, std::string_view relPath) const noexcept;

    /// 登记新资产: 规范化 relPath (宽容 \ 与 / 混用)、分配一次 NewGuid、追加到 <bundle>
    /// 末尾 (DOM 常驻, 未落盘)、进索引。目标 path 已有条目时返回 nullopt, outError 附带
    /// 已有条目的 GUID (调用方要幂等语义自己拼)。
    std::optional<AssetId> AddEntry(std::string_view bundleName, std::string_view relPath, std::string_view type, string& outError);

    /// 把指定 bundle 的 DOM 落盘。初版唯一的写盘出口 (不做目录扫描 Sync)。
    bool SaveBundle(std::string_view bundleName, string& outError);

    /// 资产类型 → loader 注册表。进程装配代码显式逐个注册, 无静态自动注册。
    using LoaderFn = task<AssetLoadResult> (*)(const ResolvedAsset&);
    void RegisterLoader(string type, LoaderFn loader);
    std::optional<LoaderFn> FindLoader(std::string_view type) const noexcept;

private:
    struct Bundle {
        /// bundle 名 = assetRoot 下的相对目录路径, '/' 分隔 (根下含 bundle.xml 的目录即
        /// bundle; 资产根自身为 bundle 时名称为空串)。
        string Name;
        std::filesystem::path Dir;
        AssetBundleManifest Manifest;
    };

    struct EntryRef {
        size_t BundleIndex{};
        pugi::xml_node Node;
    };

    size_t FindBundleIndex(std::string_view name) const noexcept;
    void Clear() noexcept;

    std::filesystem::path _assetRoot;
    vector<Bundle> _bundles;
    unordered_map<AssetId, EntryRef> _byId;
    /// key: 小写折叠的 "<bundleName>/<relPath>", value: GUID。口径与 AssetBundleManifest
    /// 的 _byPath 折叠一致。
    unordered_map<string, AssetId> _byPath;
    unordered_map<string, LoaderFn> _loaders;
};

/// 唯一同时认识 AssetDatabase 与 AssetManager 的桥接: 按条目元素名查 loader 注册表, 用
/// 清单 GUID 作为 AssetLoadRequest::Id 提交给 manager.Load。未解析 / 未注册 type 时记
/// error 并返回无效引用。loader 的 DOM 访问约束见 ResolvedAsset::Node。
StreamingAssetRefAny LoadFromDatabase(AssetManager& manager, const AssetDatabase& db, const AssetId& id);

}  // namespace radray
