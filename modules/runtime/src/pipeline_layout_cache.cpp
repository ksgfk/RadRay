#include <radray/runtime/pipeline_layout_cache.h>

#include <algorithm>
#include <bit>
#include <type_traits>
#include <utility>

#include <radray/hash.h>
#include <radray/logger.h>

namespace radray {

namespace {

template <class T>
requires std::is_enum_v<T>
void AddEnum(HashCode& hash, T value) noexcept {
    hash.Add(static_cast<std::underlying_type_t<T>>(value));
}

/// 【必须规格化 -0.0f】: -0.0f == 0.0f 为真, 但两者位型不同。直接 bit_cast 会让一对
/// operator== 相等的 key 拿到不同散列值, 那是 unordered_map 的硬性契约违规。
void AddFloat(HashCode& hash, float value) noexcept {
    if (value == 0.0f) {
        value = 0.0f;
    }
    hash.Add(std::bit_cast<uint32_t>(value));
}

void AddSampler(HashCode& hash, const render::SamplerDescriptor& sampler) noexcept {
    AddEnum(hash, sampler.AddressS);
    AddEnum(hash, sampler.AddressT);
    AddEnum(hash, sampler.AddressR);
    AddEnum(hash, sampler.MinFilter);
    AddEnum(hash, sampler.MagFilter);
    AddEnum(hash, sampler.MipmapFilter);
    AddFloat(hash, sampler.LodMin);
    AddFloat(hash, sampler.LodMax);
    hash.Add(sampler.Compare.has_value() ? 1u : 0u);
    if (sampler.Compare.has_value()) {
        AddEnum(hash, sampler.Compare.value());
    }
    hash.Add(sampler.AnisotropyClamp);
}

/// 【逐字段, 不 memcmp】: Entry 含 optional<SamplerDescriptor>, 其填充字节未定义, 会让
/// 两个 operator== 相等的对象拿到不同散列值。
void AddEntry(HashCode& hash, const render::ShaderParameterSetLayoutEntryDescriptor& entry) noexcept {
    hash.Add(entry.Binding);
    AddEnum(hash, entry.Type);
    hash.Add(entry.Count);
    hash.Add(entry.Stages.value());
    hash.Add(entry.ImmutableSampler.has_value() ? 1u : 0u);
    if (entry.ImmutableSampler.has_value()) {
        AddSampler(hash, entry.ImmutableSampler.value());
    }
}

}  // namespace

PipelineLayoutKey::PipelineLayoutKey() noexcept
    : _hash(ComputeHash()) {}

PipelineLayoutKey::PipelineLayoutKey(PipelineLayoutKey&& other) noexcept
    : _groups(std::move(other._groups)),
      _entries(std::move(other._entries)),
      _pushConstant(std::move(other._pushConstant)),
      _hash(other._hash) {
    // 【显式清空】: vector 移动后为空是实现约定而非标准保证, optional 移动后更是明确
    // 仍处于 engaged。不清就会留下一个内容与散列值不符的源。
    other._groups.clear();
    other._entries.clear();
    other._pushConstant.reset();
    other._hash = other.ComputeHash();
}

PipelineLayoutKey& PipelineLayoutKey::operator=(PipelineLayoutKey&& other) noexcept {
    if (this != &other) {
        _groups = std::move(other._groups);
        _entries = std::move(other._entries);
        _pushConstant = std::move(other._pushConstant);
        _hash = other._hash;
        other._groups.clear();
        other._entries.clear();
        other._pushConstant.reset();
        other._hash = other.ComputeHash();
    }
    return *this;
}

PipelineLayoutKey PipelineLayoutKey::Build(const render::PipelineLayoutDescriptor& desc) {
    // 【第一步: 合并 GroupIndex 相同的 parameter set】。两个后端都把同组的 entry 拼在
    // 一起, 故 [{0,[b0]},{0,[b1]}] 与 [{0,[b0,b1]}] 会建出逐字节相同的 GPU 对象。不合并
    // 就会为它们各建一份 root signature。
    //
    // 用 vector 而非 map 累积: 组数是个位数, 线性查找远快于建树/散列。
    struct Accumulated {
        uint32_t GroupIndex{0};
        vector<Entry> Entries;
    };
    vector<Accumulated> accumulated;
    accumulated.reserve(desc.ParameterSets.size());
    for (const render::ShaderParameterSetLayoutDescriptor& set : desc.ParameterSets) {
        auto it = std::ranges::find_if(
            accumulated,
            [&set](const Accumulated& value) noexcept { return value.GroupIndex == set.GroupIndex; });
        if (it == accumulated.end()) {
            // 【空组要保留】: 它是可观测的 —— d3d12 会留一个可被 CreateShaderParameterSet
            // 查到的 parameter group, vk 的 setLayoutCount 取 max(GroupIndex)+1。
            accumulated.push_back(Accumulated{set.GroupIndex, {}});
            it = accumulated.end() - 1;
        }
        it->Entries.insert(it->Entries.end(), set.Entries.begin(), set.Entries.end());
    }

    // 【第二步: 组按 GroupIndex 排序】。GroupIndex 在合并后唯一, 故这是全序。
    std::ranges::sort(accumulated, {}, &Accumulated::GroupIndex);

    PipelineLayoutKey key{};
    key._groups.reserve(accumulated.size());
    size_t totalEntries = 0;
    for (const Accumulated& group : accumulated) {
        totalEntries += group.Entries.size();
    }
    key._entries.reserve(totalEntries);

    for (Accumulated& group : accumulated) {
        // 【第三步: 组内 entry 按 Binding 排序】。两个后端都排, 且都拒绝重复 binding,
        // 故对任何能建成的 layout 而言 Binding 就是全序。stable_sort 只为让重复
        // binding (注定建失败, 永远进不了缓存) 也有确定的归一化结果。
        std::stable_sort(
            group.Entries.begin(),
            group.Entries.end(),
            [](const Entry& lhs, const Entry& rhs) noexcept { return lhs.Binding < rhs.Binding; });
        key._groups.push_back(
            Group{
                group.GroupIndex,
                static_cast<uint32_t>(key._entries.size()),
                static_cast<uint32_t>(group.Entries.size())});
        key._entries.insert(
            key._entries.end(),
            std::make_move_iterator(group.Entries.begin()),
            std::make_move_iterator(group.Entries.end()));
    }

    key._pushConstant = desc.PushConstant;
    key._hash = key.ComputeHash();
    return key;
}

size_t PipelineLayoutKey::ComputeHash() const noexcept {
    HashCode hash{};
    hash.Add(_groups.size());
    for (const Group& group : _groups) {
        hash.Add(group.GroupIndex);
        hash.Add(group.EntryCount);
    }
    for (const Entry& entry : _entries) {
        AddEntry(hash, entry);
    }
    hash.Add(_pushConstant.has_value() ? 1u : 0u);
    if (_pushConstant.has_value()) {
        const render::PushConstantDescriptor& pushConstant = _pushConstant.value();
        hash.Add(pushConstant.Location.Group);
        hash.Add(pushConstant.Location.Binding);
        hash.Add(pushConstant.Size);
        hash.Add(pushConstant.Stages.value());
    }
    return hash.ToHashCode();
}

SharedPipelineLayout::SharedPipelineLayout(
    PipelineLayoutCache* cache,
    unique_ptr<render::PipelineLayout> object) noexcept
    : _cache(cache), _object(std::move(object)) {
    RADRAY_ASSERT(_object != nullptr);
}

SharedPipelineLayout::~SharedPipelineLayout() noexcept {
    if (_object != nullptr) {
        _object->Destroy();
    }
}

void IntrusivePtrAddRef(SharedPipelineLayout* obj) noexcept {
    ++obj->_refCount;
}

void IntrusivePtrRelease(SharedPipelineLayout* obj) noexcept {
    RADRAY_ASSERT(obj->_refCount > 0);
    if (--obj->_refCount != 0) {
        return;
    }
    if (obj->_cache != nullptr) {
        // 摘除即销毁 —— 表内的 unique_ptr 是所有者。
        obj->_cache->Evict(obj);
        return;
    }
    // 缓存已先死并把所有权交还给本对象 (见 PipelineLayoutCache 的析构)。
    const unique_ptr<SharedPipelineLayout> owner{obj};
}

PipelineLayoutCache::PipelineLayoutCache(render::Device* device) noexcept
    : _device(device) {
    RADRAY_ASSERT(_device != nullptr);
}

PipelineLayoutCache::~PipelineLayoutCache() noexcept {
    // 【残留条目是常规情况, 不是泄漏】: Application::Shutdown 先 reset RenderSystem
    // (本缓存的宿主), 后 reset AssetManager, 而资产要到那时才放开最后一份引用。
    // 故把所有权交还给条目自己: 切断反向指针后 release, 此后它自持, 归零时自毁。
    for (auto& [key, layout] : _layouts) {
        (void)key;
        // 计数为零的条目在归零那一刻就被摘除了, 故表里的每一条都还有持有者。
        RADRAY_ASSERT(layout->_refCount > 0);
        layout->_cache = nullptr;
        layout->_key = nullptr;
        (void)layout.release();
    }
    _layouts.clear();
}

IntrusivePtr<SharedPipelineLayout> PipelineLayoutCache::GetOrCreate(
    const render::PipelineLayoutDescriptor& desc) noexcept {
    PipelineLayoutKey key = PipelineLayoutKey::Build(desc);
    if (const auto it = _layouts.find(key); it != _layouts.end()) {
        ++_hits;
        return RetainRef(it->second.get());
    }
    ++_misses;

    // 【喂给后端的是调用方原本的 descriptor, 不是 key 重建的】: key 只用于查表, 故它
    // 无需能还原成 descriptor —— 省掉了一整套 span 后备存储, 以及"上一次 Get() 返回的
    // descriptor 会被下一次 Get() 悄悄悬空"那个陷阱。归一化不改变语义, 两个后端自己
    // 也会再归一化一遍, 建出的对象与喂 key 重建的 descriptor 完全一致。
    auto object = _device->CreatePipelineLayout(desc);
    if (!object.HasValue()) {
        return nullptr;
    }

    const auto [it, inserted] = _layouts.try_emplace(
        std::move(key),
        make_unique<SharedPipelineLayout>(this, object.Release()));
    // find 刚刚未命中, 故必然是新插入。
    RADRAY_ASSERT(inserted);
    it->second->_key = &it->first;
    // 【计数从 0 起】: 缓存自己不占份额, 故这里 RetainRef 把它抬到 1。
    return RetainRef(it->second.get());
}

void PipelineLayoutCache::Evict(SharedPipelineLayout* layout) noexcept {
    RADRAY_ASSERT(layout->_key != nullptr);
    const auto it = _layouts.find(*layout->_key);
    RADRAY_ASSERT(it != _layouts.end() && it->second.get() == layout);
    _layouts.erase(it);
}

}  // namespace radray
