#include <radray/runtime/pipeline_layout_cache.h>

#include <algorithm>

namespace radray {

PipelineLayoutCacheKey PipelineLayoutCacheKey::Build(
    const render::PipelineLayoutDescriptor& desc) {
    PipelineLayoutCacheKey key{};
    key.Groups.reserve(desc.ParameterSets.size());
    for (const render::ShaderParameterSetLayoutDescriptor& set : desc.ParameterSets) {
        Group group{};
        group.GroupIndex = set.GroupIndex;
        group.Entries.assign(set.Entries.begin(), set.Entries.end());
        std::sort(
            group.Entries.begin(),
            group.Entries.end(),
            [](const render::ShaderParameterSetLayoutEntryDescriptor& lhs,
               const render::ShaderParameterSetLayoutEntryDescriptor& rhs) noexcept {
                return lhs.Binding < rhs.Binding;
            });
        key.Groups.push_back(std::move(group));
    }
    std::sort(
        key.Groups.begin(),
        key.Groups.end(),
        [](const Group& lhs, const Group& rhs) noexcept {
            return lhs.GroupIndex < rhs.GroupIndex;
        });
    key.PushConstant = desc.PushConstant;
    return key;
}

render::PipelineLayoutDescriptor PipelineLayoutCacheKey::Get() const {
    // 无条件重建 —— 拷贝 / 移动后 span 会指向旧位置, 见头文件。
    _setsView.clear();
    _setsView.reserve(Groups.size());
    for (const Group& group : Groups) {
        render::ShaderParameterSetLayoutDescriptor set{};
        set.GroupIndex = group.GroupIndex;
        set.Entries = group.Entries;
        _setsView.push_back(set);
    }

    render::PipelineLayoutDescriptor desc{};
    desc.ParameterSets = _setsView;
    desc.PushConstant = PushConstant;
    return desc;
}

SharedPipelineLayout::SharedPipelineLayout(
    PipelineLayoutCache* cache,
    PipelineLayoutCacheKey key,
    unique_ptr<render::PipelineLayout> object) noexcept
    : _cache(cache),
      _key(std::move(key)),
      _object(std::move(object)) {}

SharedPipelineLayout::~SharedPipelineLayout() noexcept {
    if (_object != nullptr) {
        _object->Destroy();
        _object.reset();
    }
}

void IntrusivePtrAddRef(const SharedPipelineLayout* obj) noexcept {
    ++obj->_refCount;
}

void IntrusivePtrRelease(const SharedPipelineLayout* obj) noexcept {
    if (--obj->_refCount != 0) {
        return;
    }
    auto* layout = const_cast<SharedPipelineLayout*>(obj);
    // 缓存已死时 _cache 为空 —— 关停时 RenderSystem 先于 AssetManager 销毁, 这是常规
    // 路径而非异常路径 (见头文件)。
    if (layout->_cache != nullptr) {
        layout->_cache->Detach(layout);
    }
    delete layout;
}

PipelineLayoutCache::PipelineLayoutCache(render::Device* device) noexcept
    : _device(device) {}

PipelineLayoutCache::~PipelineLayoutCache() noexcept {
    // 仍在被引用的 layout 必须撑过本缓存。切断反向指针, 它们归零时自行销毁。
    for (SharedPipelineLayout* layout : _entries) {
        if (layout != nullptr) {
            layout->_cache = nullptr;
        }
    }
    _entries.clear();
    _device = nullptr;
}

SharedPipelineLayoutRef PipelineLayoutCache::GetOrCreate(
    const render::PipelineLayoutDescriptor& desc) noexcept {
    return GetOrCreate(PipelineLayoutCacheKey::Build(desc));
}

SharedPipelineLayoutRef PipelineLayoutCache::GetOrCreate(PipelineLayoutCacheKey key) noexcept {
    if (_device == nullptr) {
        return nullptr;
    }
    for (SharedPipelineLayout* entry : _entries) {
        if (entry->_key == key) {
            ++_hits;
            return RetainRef(entry);
        }
    }

    auto object = _device->CreatePipelineLayout(key.Get());
    if (!object.HasValue()) {
        return nullptr;
    }
    ++_misses;
    auto layout = MakeIntrusive<SharedPipelineLayout>(this, std::move(key), object.Release());
    _entries.push_back(layout.Get());
    return layout;
}

void PipelineLayoutCache::Detach(SharedPipelineLayout* layout) noexcept {
    const auto it = std::find(_entries.begin(), _entries.end(), layout);
    if (it != _entries.end()) {
        _entries.erase(it);
    }
}

}  // namespace radray
