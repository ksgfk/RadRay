#pragma once

#include <initializer_list>
#include <radray/nullable.h>
#include <radray/runtime/render_framework/render_memory_stats.h>
#include <radray/runtime/render_framework/scene_change_set.h>

namespace radray {
struct RenderSceneSnapshot;

namespace detail {
template <class T>
inline constexpr byte SceneExtensionType{};
}

/// Scene-owned scratch and version pools. A row is writable only when no published data or
/// another preparation retains it. Keys are dense provider-local indices, resolved once per batch.
class SceneExtensionWorkspace {
    struct StorageBase {
        virtual ~StorageBase() noexcept = default;
        virtual RenderMemoryStats GetMemoryStats() const noexcept = 0;
    };

public:
    template <class T>
    class Storage final : public StorageBase {
    public:
        shared_ptr<T> Acquire(size_t key) {
            if (_rows.size() <= key) _rows.resize(key + 1);
            auto& versions = _rows[key];
            for (auto& value : versions) {
                if (value.use_count() != 1) continue;
                if constexpr (requires(T& object) { { object.ResetForReuse() } noexcept; }) value->ResetForReuse();
                return value;
            }
            auto value = make_shared<T>();
            versions.push_back(value);
            return value;
        }
        RenderMemoryStats GetMemoryStats() const noexcept override {
            RenderMemoryStats result;
            result.ObjectBytes = sizeof(*this);
            result.VectorCapacityBytes = _rows.capacity() * sizeof(typename decltype(_rows)::value_type);
            for (const auto& versions : _rows) {
                result.VectorCapacityBytes += versions.capacity() * sizeof(shared_ptr<T>);
                result.OwnerReferences += versions.size();
                result.LiveEntries += versions.size();
                for (const auto& value : versions) {
                    if constexpr (requires(const T& object) { object.GetMemoryStats(); })
                        result.Add(value->GetMemoryStats());
                    else
                        result.ObjectBytes += sizeof(T);
                }
            }
            return result;
        }

    private:
        vector<vector<shared_ptr<T>>> _rows;
    };
    template <class T>
    Storage<T>& GetStorage() {
        const auto* type = &detail::SceneExtensionType<T>;
        for (auto& entry : _entries)
            if (entry.Type == type) return *static_cast<Storage<T>*>(entry.Value.get());
        auto value = make_unique<Storage<T>>();
        auto& result = *value;
        _entries.push_back({type, std::move(value)});
        return result;
    }
    RenderMemoryStats GetMemoryStats() const noexcept {
        RenderMemoryStats result;
        result.ObjectBytes = sizeof(*this);
        result.VectorCapacityBytes = _entries.capacity() * sizeof(Entry);
        for (const auto& entry : _entries) result.Add(entry.Value->GetMemoryStats());
        return result;
    }

private:
    struct Entry {
        const void* Type;
        unique_ptr<StorageBase> Value;
    };
    vector<Entry> _entries;
};

/// Immutable CPU-only contract. Prepare reads frozen input and declared table dependencies only;
/// it must not read World, camera state, mutable authoring objects or hidden global state.
/// Returning false leaves the previous immutable value and all publication deltas unacknowledged.
struct SceneRenderExtension {
    uint64_t Id{0}, Revision{0}, Configuration{0};
    array<bool, static_cast<size_t>(SceneDataTable::Count)> Dependencies{};
    bool EveryEpoch{false};
    Nullable<const void*> Type{nullptr};
    bool (*Prepare)(SceneExtensionWorkspace&, const RenderSceneSnapshot&, const SceneChangeSet&, uint64_t,
                    const shared_ptr<const void>&, shared_ptr<const void>&){nullptr};

    bool IsValid() const noexcept { return Id != 0 && Revision != 0 && Type && Prepare; }
    bool SameIdentity(const SceneRenderExtension& other) const noexcept {
        return Id == other.Id && Revision == other.Revision && Configuration == other.Configuration;
    }
    friend bool operator==(const SceneRenderExtension&, const SceneRenderExtension&) = default;

    /// Function(snapshot, changes, configuration, previous, result) constructs immutable T data.
    /// No dynamic callback allocation; the callback identity participates in registration conflicts.
    template <class T, auto Function>
    static SceneRenderExtension Make(uint64_t id, uint64_t revision, uint64_t configuration,
                                     std::initializer_list<SceneDataTable> dependencies, bool everyEpoch = false) {
        SceneRenderExtension result;
        result.Id = id;
        result.Revision = revision;
        result.Configuration = configuration;
        result.EveryEpoch = everyEpoch;
        result.Type = &detail::SceneExtensionType<T>;
        for (const auto dependency : dependencies) {
            if (dependency >= SceneDataTable::Count) return {};
            result.Dependencies[static_cast<size_t>(dependency)] = true;
        }
        result.Prepare = +[](SceneExtensionWorkspace& workspace, const RenderSceneSnapshot& snapshot, const SceneChangeSet& changes, uint64_t config,
                             const shared_ptr<const void>& previous, shared_ptr<const void>& value) {
            shared_ptr<const T> typed;
            const auto old = std::static_pointer_cast<const T>(previous);
            if constexpr (requires { Function(workspace, snapshot, changes, config, old, typed); }) {
                if (!Function(workspace, snapshot, changes, config, old, typed)) return false;
            } else if (!Function(snapshot, changes, config, old, typed))
                return false;
            if (!typed) return false;
            value = std::move(typed);
            return true;
        };
        return result;
    }
};

struct SceneExtensionSnapshot {
    SceneRenderExtension Contract;
    uint64_t PublicationId{0}, Epoch{0}, Revision{0};
    shared_ptr<const void> Data;
    bool Prepared{false};
    template <class T>
    shared_ptr<const T> Get() const noexcept {
        if (Contract.Type.Get() != &detail::SceneExtensionType<T>) return {};
        return std::static_pointer_cast<const T>(Data);
    }
};
}  // namespace radray
