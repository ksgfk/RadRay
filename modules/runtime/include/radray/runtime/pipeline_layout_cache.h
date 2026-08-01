#pragma once

#include <optional>
#include <span>

#include <radray/intrusive_ptr.h>
#include <radray/render/rhi.h>
#include <radray/types.h>

// 按 binding 布局内容去重 render::PipelineLayout, 引用计数共享。
//
// 缓存拥有条目但【不占引用计数】, 且【允许先死于持有者】(那是常规关停路径)。layout 为何
// 不需要延迟销毁、key 为何要归一化: docs/architecture/asset-system.md

namespace radray {

/// PipelineLayout 的内容指纹。语义相同的布局必须得到相等的 key, 否则 manifest 的书写
/// 方式会凭空劈开缓存条目。
///
/// 归一化三步 (合并同组 set、组内按 Binding 排序、组按 GroupIndex 排序) 各自对应后端建
/// layout 时的既有行为。【刻意保留空组】它是可观测的 —— vk 的 setLayoutCount 取
/// max(GroupIndex)+1。
class PipelineLayoutKey {
public:
    /// 一组 binding。Entries 存在 key 的扁平数组里, 本结构只记录区间。
    struct Group {
        uint32_t GroupIndex{0};
        uint32_t EntryOffset{0};
        uint32_t EntryCount{0};

        friend bool operator==(const Group&, const Group&) noexcept = default;
    };

    using Entry = render::ShaderParameterSetLayoutEntryDescriptor;

    /// 空布局 (无组、无 push constant)。等价于 Build 一个空 descriptor。
    PipelineLayoutKey() noexcept;

    static PipelineLayoutKey Build(const render::PipelineLayoutDescriptor& desc);

    std::span<const Group> GetGroups() const noexcept { return _groups; }

    /// group 必须来自本 key 的 GetGroups()。
    std::span<const Entry> GetEntries(const Group& group) const noexcept {
        return std::span<const Entry>{_entries}.subspan(group.EntryOffset, group.EntryCount);
    }

    const std::optional<render::PushConstantDescriptor>& GetPushConstant() const noexcept {
        return _pushConstant;
    }

    /// 构建期算一次。【逐字段算, 不 memcmp】Entry 含 optional<SamplerDescriptor>,
    /// 填充字节会让两个 operator== 相等的对象拿到不同的散列值。
    size_t GetHash() const noexcept { return _hash; }

    PipelineLayoutKey(const PipelineLayoutKey&) = default;
    PipelineLayoutKey& operator=(const PipelineLayoutKey&) = default;

    /// 【移动必须自己写】默认移动会留下"空 vector + 原 _hash"的源, 那个源与真正的空 key
    /// 内容相等却散列不等, 违反 unordered_map 的 "equal => same hash" 契约。
    PipelineLayoutKey(PipelineLayoutKey&& other) noexcept;
    PipelineLayoutKey& operator=(PipelineLayoutKey&& other) noexcept;

    friend bool operator==(const PipelineLayoutKey& lhs, const PipelineLayoutKey& rhs) noexcept {
        return lhs._hash == rhs._hash &&
               lhs._pushConstant == rhs._pushConstant &&
               lhs._groups == rhs._groups &&
               lhs._entries == rhs._entries;
    }

private:
    size_t ComputeHash() const noexcept;

    vector<Group> _groups;
    /// 全部组的 entry 扁平存一份, 由 Group 的 (EntryOffset, EntryCount) 切分。
    vector<Entry> _entries;
    std::optional<render::PushConstantDescriptor> _pushConstant;
    size_t _hash{0};
};

struct PipelineLayoutKeyHash {
    size_t operator()(const PipelineLayoutKey& key) const noexcept { return key.GetHash(); }
};

class PipelineLayoutCache;

/// 一个被共享的 render::PipelineLayout。布局相同的每个持有者拿到的都是同一个对象。
class SharedPipelineLayout {
public:
    SharedPipelineLayout(PipelineLayoutCache* cache, unique_ptr<render::PipelineLayout> object) noexcept;
    SharedPipelineLayout(const SharedPipelineLayout&) = delete;
    SharedPipelineLayout& operator=(const SharedPipelineLayout&) = delete;
    SharedPipelineLayout(SharedPipelineLayout&&) = delete;
    SharedPipelineLayout& operator=(SharedPipelineLayout&&) = delete;
    ~SharedPipelineLayout() noexcept;

    /// 非空。对象存活即 layout 有效。
    render::PipelineLayout* Get() const noexcept { return _object.get(); }

    /// 活着的 IntrusivePtr 份数。缓存自己不占份额。
    uint32_t GetRefCount() const noexcept { return _refCount; }

    /// 是否仍在缓存索引内。缓存先于本对象销毁后为 false, 此后 layout 自持。
    bool IsCached() const noexcept { return _cache != nullptr; }

private:
    friend class PipelineLayoutCache;
    friend void IntrusivePtrAddRef(SharedPipelineLayout* obj) noexcept;
    friend void IntrusivePtrRelease(SharedPipelineLayout* obj) noexcept;

    PipelineLayoutCache* _cache{nullptr};
    /// 指回缓存表内的 key, 供 O(1) 摘除。脱离缓存后为空。
    const PipelineLayoutKey* _key{nullptr};
    unique_ptr<render::PipelineLayout> _object;
    uint32_t _refCount{0};
};

void IntrusivePtrAddRef(SharedPipelineLayout* obj) noexcept;
void IntrusivePtrRelease(SharedPipelineLayout* obj) noexcept;

class PipelineLayoutCache {
public:
    /// device 必须非空, 且必须活得比本缓存久。
    explicit PipelineLayoutCache(render::Device* device) noexcept;
    ~PipelineLayoutCache() noexcept;
    PipelineLayoutCache(const PipelineLayoutCache&) = delete;
    PipelineLayoutCache& operator=(const PipelineLayoutCache&) = delete;
    PipelineLayoutCache(PipelineLayoutCache&&) = delete;
    PipelineLayoutCache& operator=(PipelineLayoutCache&&) = delete;

    /// 返回共享 layout; 仅在 CreatePipelineLayout 失败时返回 nullptr。
    IntrusivePtr<SharedPipelineLayout> GetOrCreate(const render::PipelineLayoutDescriptor& desc) noexcept;

    /// 非空 —— 构造时即定, 不随析构失效。
    render::Device* GetDevice() const noexcept { return _device; }

    uint32_t GetLayoutCount() const noexcept { return static_cast<uint32_t>(_layouts.size()); }
    /// 命中 / 未命中都指【查表结果】, 二者之和即 GetOrCreate 的调用次数。
    uint64_t GetHitCount() const noexcept { return _hits; }
    uint64_t GetMissCount() const noexcept { return _misses; }

private:
    friend void IntrusivePtrRelease(SharedPipelineLayout* obj) noexcept;

    void Evict(SharedPipelineLayout* layout) noexcept;

    render::Device* _device{nullptr};
    unordered_map<PipelineLayoutKey, unique_ptr<SharedPipelineLayout>, PipelineLayoutKeyHash> _layouts;
    uint64_t _hits{0};
    uint64_t _misses{0};
};

}  // namespace radray
