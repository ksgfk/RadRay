#pragma once

#include <concepts>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

#include <radray/json.h>
#include <radray/nullable.h>
#include <radray/runtime/asset_source.h>
#include <radray/types.h>

namespace radray {

class AssetImportSettings {
public:
    virtual ~AssetImportSettings() noexcept = default;

    virtual bool Deserialize(const JsonValue& json) = 0;
    virtual bool Serialize(JsonWriteContext& context) const noexcept = 0;
};

struct AssetEntry {
    AssetId Guid;
    string Path;
    string Type;
    unique_ptr<AssetImportSettings> Settings{};
    /// 仅在 type 未注册、type 无 settings 形状或 settings 解码失败时保存原始 JSON 值。
    string RawSettings{};
};

template <class T>
concept AssetSettingsQueryTarget =
    std::is_class_v<std::remove_cv_t<T>> &&
    std::same_as<T, std::remove_reference_t<T>> &&
    requires { sizeof(std::remove_cv_t<T>); };

template <class T>
requires AssetSettingsQueryTarget<T>
Nullable<const T*> GetSettings(const AssetEntry& entry) noexcept {
    if (entry.Settings == nullptr) {
        return nullptr;
    }
    return dynamic_cast<const T*>(entry.Settings.get());
}

struct AssetLoadContext {
    std::filesystem::path AbsolutePath;
    const AssetImportSettings* Settings{nullptr};
};

class AssetImporter {
public:
    virtual ~AssetImporter() noexcept = default;

    virtual std::string_view GetTypeName() const noexcept = 0;
    virtual std::span<const std::string_view> GetFileExtensions() const noexcept { return {}; }
    virtual unique_ptr<AssetImportSettings> CreateSettings() const { return nullptr; }

    /// 【不得实现成协程】调用返回前必须同步读完 ctx；惰性 task 启动后 ctx 可能已经失效。
    virtual task<AssetLoadResult> Load(const AssetLoadContext& ctx) = 0;
};

template <class TSettings>
requires std::derived_from<TSettings, AssetImportSettings> && std::copy_constructible<TSettings>
class TypedAssetImporter : public AssetImporter {
public:
    unique_ptr<AssetImportSettings> CreateSettings() const override {
        return make_unique<TSettings>();
    }

    task<AssetLoadResult> Load(const AssetLoadContext& ctx) final {
        if (ctx.Settings == nullptr) {
            return InvalidSettings();
        }
        const auto* settings = dynamic_cast<const TSettings*>(ctx.Settings);
        if (settings == nullptr) {
            return InvalidSettings();
        }
        std::filesystem::path path = ctx.AbsolutePath;
        TSettings settingsSnapshot = *settings;
        return LoadTyped(std::move(path), std::move(settingsSnapshot));
    }

protected:
    /// 两个参数刻意按值进入真正的加载 task，不能改成引用。
    virtual task<AssetLoadResult> LoadTyped(std::filesystem::path path, TSettings settings) = 0;

private:
    static task<AssetLoadResult> InvalidSettings() {
        co_return AssetLoadResult::Failure("asset import settings are missing or have the wrong type");
    }
};

/// `<assetRoot>/assets.json` 的内存索引，也是 AssetManager 的可选 IAssetSource。
class AssetDatabase final : public IAssetSource {
public:
    /// importer 必须在 Open 前全部给出；未注册 type 的 settings 会退化为 RawSettings。
    /// 清单不存在时打开为空库。结构错误返回 nullptr 并填写 outError。
    static unique_ptr<AssetDatabase> Open(
        const std::filesystem::path& assetRoot,
        vector<unique_ptr<AssetImporter>> importers,
        string& outError);

    ~AssetDatabase() noexcept override = default;
    AssetDatabase(const AssetDatabase&) = delete;
    AssetDatabase& operator=(const AssetDatabase&) = delete;
    AssetDatabase(AssetDatabase&&) = delete;
    AssetDatabase& operator=(AssetDatabase&&) = delete;

    /// 返回指针在表 rehash 后仍有效；RemoveEntry 对该条目的调用会使它失效。
    const AssetEntry* Find(const AssetId& id) const noexcept;
    const AssetEntry* Find(std::string_view relPath) const noexcept;
    std::filesystem::path ResolvePath(const AssetEntry& entry) const;

    std::optional<AssetId> AddEntry(
        std::string_view relPath,
        std::string_view type,
        string& outError);

    template <class T>
    requires AssetSettingsQueryTarget<T>
    Nullable<T*> MutableSettings(const AssetId& id) noexcept;

    bool SetPath(const AssetId& id, std::string_view newRelPath, string& outError);
    bool RemoveEntry(const AssetId& id) noexcept;

    /// 按 path 排序全量重写清单。不保留条目顺序或其他根内容。
    bool Save(string& outError) const;

    /// 扫描 importer 认领的文件并登记新条目；缺失文件只记 warning。不会自动 Save。
    bool Refresh(string& outError);

    std::optional<task<AssetLoadResult>> CreateLoadTask(const AssetId& id) override;
    std::optional<AssetId> ResolveId(std::string_view relPath) const override;

private:
    AssetDatabase(
        std::filesystem::path assetRoot,
        vector<unique_ptr<AssetImporter>> importers) noexcept;

    AssetImporter* FindImporter(std::string_view type) const noexcept;

    std::filesystem::path _assetRoot;
    vector<unique_ptr<AssetImporter>> _ownedImporters;
    unordered_map<string, AssetImporter*> _importers;
    unordered_map<string, AssetImporter*> _extensionImporters;
    unordered_map<AssetId, AssetEntry> _entries;
    unordered_map<string, AssetId> _paths;
};

template <class T>
requires AssetSettingsQueryTarget<T>
Nullable<T*> AssetDatabase::MutableSettings(const AssetId& id) noexcept {
    auto it = _entries.find(id);
    if (it == _entries.end() || it->second.Settings == nullptr) {
        return nullptr;
    }
    return dynamic_cast<T*>(it->second.Settings.get());
}

}  // namespace radray
