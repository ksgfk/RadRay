#include <radray/runtime/asset_database.h>

#include <algorithm>
#include <limits>
#include <string_view>
#include <system_error>

#include <fmt/format.h>

#include <radray/guid.h>
#include <radray/logger.h>

namespace radray {
namespace {

/// 与 AssetBundleManifest 同口径的 ASCII 小写折叠 (bundle 名 / path 查表键)。
string FoldPathKey(std::string_view value) {
    string out(value);
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch + ('a' - 'A'));
        }
    }
    return out;
}

bool PathExists(const std::filesystem::path& path) noexcept {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

bool HasManifest(const std::filesystem::path& dir) noexcept {
    std::error_code ec;
    return std::filesystem::is_regular_file(dir / "bundle.xml", ec);
}

/// child 是否为 parent 的严格子孙目录。经 filesystem::relative 处理分隔符与大小写,
/// 相对结果出现 ".." 即在外。
bool IsInside(const std::filesystem::path& child, const std::filesystem::path& parent) {
    std::error_code ec;
    const std::filesystem::path rel = std::filesystem::relative(child, parent, ec);
    if (ec || rel.empty() || rel == std::filesystem::path(".")) {
        return false;
    }
    for (const auto& part : rel) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

}  // namespace

bool AssetDatabase::Mount(const std::filesystem::path& assetRoot, string& outError) {
    Clear();

    std::error_code ec;
    // 归一为绝对路径: 否则 ResolvedAsset::AbsolutePath 与 bundle 名在调用方传相对根时
    // 都跟着变相对, 违反字段契约。absolute 不做存在性检查, 失配目录留给 is_directory 报。
    const std::filesystem::path absRoot = std::filesystem::absolute(assetRoot, ec);
    const std::filesystem::path& root = ec ? assetRoot : absRoot;
    if (!std::filesystem::is_directory(root, ec)) {
        outError = fmt::format("asset root '{}' is not a directory", root.string());
        return false;
    }
    _assetRoot = root;

    vector<std::filesystem::path> bundleDirs;
    std::filesystem::recursive_directory_iterator iter(root, std::filesystem::directory_options::none, ec);
    const std::filesystem::recursive_directory_iterator end;
    if (ec) {
        outError = fmt::format("failed to scan asset root '{}': {}", root.string(), ec.message());
        Clear();
        return false;
    }
    for (; iter != end; iter.increment(ec)) {
        if (ec) {
            outError = fmt::format("failed to scan asset root '{}': {}", root.string(), ec.message());
            Clear();
            return false;
        }
        if (!iter->is_directory(ec)) {
            if (ec) {
                outError = fmt::format("failed to scan asset root '{}': {}", root.string(), ec.message());
                Clear();
                return false;
            }
            continue;
        }
        if (HasManifest(iter->path())) {
            bundleDirs.push_back(iter->path());
        }
    }

    std::sort(bundleDirs.begin(), bundleDirs.end());

    // 嵌套 bundle = 结构性错误: 任一 bundle 目录不得是另一 bundle 目录的严格子孙。
    for (size_t i = 0; i < bundleDirs.size(); ++i) {
        for (size_t j = 0; j < bundleDirs.size(); ++j) {
            if (i != j && IsInside(bundleDirs[i], bundleDirs[j])) {
                outError = fmt::format("nested bundle: '{}' is inside bundle '{}' (bundles must not nest)",
                    bundleDirs[i].generic_string(), bundleDirs[j].generic_string());
                Clear();
                return false;
            }
        }
    }

    for (const std::filesystem::path& dir : bundleDirs) {
        Bundle bundle;
        bundle.Dir = dir;
        std::error_code relEc;
        const std::filesystem::path rel = std::filesystem::relative(dir, root, relEc);
        if (relEc) {
            outError = fmt::format("failed to compute bundle name for '{}': {}", dir.generic_string(), relEc.message());
            Clear();
            return false;
        }
        bundle.Name = (rel == std::filesystem::path(".")) ? string{} : rel.generic_string();

        string error;
        if (!bundle.Manifest.LoadFromFile(dir / "bundle.xml", error)) {
            outError = std::move(error);
            Clear();
            return false;
        }
        _bundles.push_back(std::move(bundle));
    }

    // 跨 bundle 全局校验 + 建索引。清单层已保证单 bundle 内的 guid / path 唯一,
    // 这里补跨 bundle 的 GUID 唯一 (身份权威)。
    for (size_t i = 0; i < _bundles.size(); ++i) {
        const Bundle& bundle = _bundles[i];
        for (const XmlNode& child : bundle.Manifest.Root().ChildNodes()) {
            if (child.NodeType() != XmlNodeType::Element) {
                continue;
            }
            const XmlElement entry{child};

            AssetId guid;
            if (!Guid::TryParse(entry.GetAttributeNode("guid").Value(), guid)) {
                // LoadFromFile 已校验, 理论不可达; 保底防身份污染。
                outError = fmt::format("bundle '{}': entry <{}> lost its guid after load", bundle.Name, entry.Name());
                Clear();
                return false;
            }
            if (_byId.contains(guid)) {
                outError = fmt::format("duplicate guid {} across bundles (bundle '{}', entry <{}>)", guid, bundle.Name, entry.Name());
                Clear();
                return false;
            }

            const std::string_view relPath = entry.GetAttributeNode("path").Value();
            const string type(entry.Name());
            const string pathKey = FoldPathKey(bundle.Name) + "/" + FoldPathKey(relPath);

            if (!_loaders.contains(type)) {
                RADRAY_WARN_LOG("AssetDatabase: bundle '{}': no loader registered for asset type '{}' ('{}'), entry stays in index",
                    bundle.Name, type, relPath);
            }
            if (!PathExists(bundle.Dir / std::filesystem::path(string(relPath)))) {
                RADRAY_WARN_LOG("AssetDatabase: bundle '{}': file '{}' does not exist yet", bundle.Name, relPath);
            }

            _byId.emplace(guid, EntryRef{i, entry});
            _byPath.emplace(pathKey, guid);
        }
    }

    return true;
}

std::optional<AssetDatabase::ResolvedAsset> AssetDatabase::Resolve(const AssetId& id) const noexcept {
    auto it = _byId.find(id);
    if (it == _byId.end()) {
        return std::nullopt;
    }
    const EntryRef& ref = it->second;
    const Bundle& bundle = _bundles[ref.BundleIndex];

    ResolvedAsset resolved;
    resolved.AbsolutePath = bundle.Dir / std::filesystem::path(string(ref.Node.GetAttributeNode("path").Value()));
    resolved.Type = ref.Node.Name();
    resolved.Node = ref.Node;
    return resolved;
}

std::optional<AssetId> AssetDatabase::FindByPath(std::string_view bundleName, std::string_view relPath) const noexcept {
    auto it = _byPath.find(FoldPathKey(bundleName) + "/" + FoldPathKey(relPath));
    if (it == _byPath.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<AssetId> AssetDatabase::AddEntry(std::string_view bundleName, std::string_view relPath, std::string_view type, string& outError) {
    const size_t index = FindBundleIndex(bundleName);
    if (index == std::numeric_limits<size_t>::max()) {
        outError = fmt::format("unknown bundle '{}' (Mount the asset root first)", bundleName);
        return std::nullopt;
    }
    Bundle& bundle = _bundles[index];

    std::optional<string> normalized = NormalizeEntryPath(relPath);
    if (!normalized.has_value()) {
        outError = fmt::format("invalid asset path '{}': must be a bundle-relative path (no absolute paths, drive letters or '..')", relPath);
        return std::nullopt;
    }

    const string pathKey = FoldPathKey(bundle.Name) + "/" + FoldPathKey(*normalized);
    auto existing = _byPath.find(pathKey);
    if (existing != _byPath.end()) {
        outError = fmt::format("bundle '{}' already has an entry at '{}' (guid {})", bundle.Name, *normalized, existing->second);
        return std::nullopt;
    }

    // 入库资产的 GUID 由 NewGuid 一次分配、永不改变; 移动/重命名 = 人改清单 path, 不存在
    // 自动改 GUID 的代码路径 (ADR-0036)。
    const AssetId guid = Guid::NewGuid();
    const XmlElement node = bundle.Manifest.AppendEntry(type, guid, *normalized);

    _byId.emplace(guid, EntryRef{index, node});
    _byPath.emplace(pathKey, guid);
    return guid;
}

bool AssetDatabase::SaveBundle(std::string_view bundleName, string& outError) {
    const size_t index = FindBundleIndex(bundleName);
    if (index == std::numeric_limits<size_t>::max()) {
        outError = fmt::format("unknown bundle '{}'", bundleName);
        return false;
    }
    return _bundles[index].Manifest.Save(outError);
}

void AssetDatabase::RegisterLoader(string type, LoaderFn loader) {
    if (type.empty() || loader == nullptr) {
        RADRAY_ABORT("AssetDatabase::RegisterLoader: type must be non-empty and loader non-null");
    }
    auto [it, inserted] = _loaders.emplace(std::move(type), loader);
    if (!inserted && it->second != loader) {
        RADRAY_WARN_LOG("AssetDatabase: loader for asset type '{}' replaced", it->first);
        it->second = loader;
    }
}

std::optional<AssetDatabase::LoaderFn> AssetDatabase::FindLoader(std::string_view type) const noexcept {
    auto it = _loaders.find(string(type));
    if (it == _loaders.end()) {
        return std::nullopt;
    }
    return it->second;
}

size_t AssetDatabase::FindBundleIndex(std::string_view name) const noexcept {
    for (size_t i = 0; i < _bundles.size(); ++i) {
        if (FoldPathKey(_bundles[i].Name) == FoldPathKey(name)) {
            return i;
        }
    }
    return std::numeric_limits<size_t>::max();
}

void AssetDatabase::Clear() noexcept {
    _assetRoot.clear();
    _bundles.clear();
    _byId.clear();
    _byPath.clear();
    // loader 注册表跨 Mount 保留: 装配代码注册一次, 重 Mount 不必重注册。
}

StreamingAssetRefAny LoadFromDatabase(AssetManager& manager, const AssetDatabase& db, const AssetId& id) {
    std::optional<AssetDatabase::ResolvedAsset> resolved = db.Resolve(id);
    if (!resolved.has_value()) {
        RADRAY_ERR_LOG("LoadFromDatabase: asset {} is not in the database", id);
        return {};
    }
    std::optional<AssetDatabase::LoaderFn> loader = db.FindLoader(resolved->Type);
    if (!loader.has_value()) {
        RADRAY_ERR_LOG("LoadFromDatabase: no loader registered for asset type '{}' (asset {})", resolved->Type, id);
        return {};
    }

    // 这里仍在主线程、仍在构造 AssetLoadRequest 的时刻 —— loader 在此刻解析
    // ResolvedAsset::Node 是合法的; task 挂起之后不得再碰 DOM (ResolvedAsset::Node)。
    return manager.Load(AssetLoadRequest{
        .Id = id,
        .Task = (*loader)(*resolved),
        .DebugName = fmt::format("{} @ {}", resolved->Type, resolved->AbsolutePath.generic_string()),
    });
}

}  // namespace radray
