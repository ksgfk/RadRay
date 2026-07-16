#include <radray/runtime/shader_variant.h>

#include <algorithm>
#include <utility>

#include <radray/hash.h>

namespace radray {
namespace {

void NormalizeDefines(vector<string>& defines) {
    std::erase_if(defines, [](const string& define) noexcept { return define.empty(); });
    std::sort(defines.begin(), defines.end());
    defines.erase(std::unique(defines.begin(), defines.end()), defines.end());
}

void AddStringVector(HashCode& hash, const vector<string>& values) noexcept {
    hash.Add(values.size());
    for (const string& value : values) {
        hash.Add(value);
    }
}

}  // namespace

void ShaderVariantKey::Normalize() {
    NormalizeDefines(Defines);
}

bool operator==(const ShaderVariantKey& lhs, const ShaderVariantKey& rhs) noexcept {
    return lhs.ShaderId == rhs.ShaderId && lhs.Defines == rhs.Defines && lhs.PassIndex == rhs.PassIndex;
}

size_t ShaderVariantKeyHasher::operator()(const ShaderVariantKey& key) const noexcept {
    HashCode hash;
    hash.Add(key.ShaderId);
    hash.Add(key.PassIndex);
    AddStringVector(hash, key.Defines);
    return hash.ToHashCode();
}

void ShaderModuleKey::Normalize() {
    NormalizeDefines(Defines);
}

bool operator==(const ShaderModuleKey& lhs, const ShaderModuleKey& rhs) noexcept {
    return lhs.ShaderId == rhs.ShaderId && lhs.Defines == rhs.Defines &&
           lhs.PassIndex == rhs.PassIndex && lhs.Stage == rhs.Stage;
}

size_t ShaderModuleKeyHasher::operator()(const ShaderModuleKey& key) const noexcept {
    HashCode hash;
    hash.Add(key.ShaderId);
    hash.Add(key.PassIndex);
    hash.Add(static_cast<uint32_t>(key.Stage));
    AddStringVector(hash, key.Defines);
    return hash.ToHashCode();
}

void ShaderPipelineLayoutKey::Normalize() {
    std::sort(Entries.begin(), Entries.end());
}

bool operator==(const ShaderPipelineLayoutKey& lhs, const ShaderPipelineLayoutKey& rhs) noexcept {
    return lhs.Entries == rhs.Entries;
}

size_t ShaderPipelineLayoutKeyHasher::operator()(const ShaderPipelineLayoutKey& key) const noexcept {
    HashCode hash;
    AddStringVector(hash, key.Entries);
    return hash.ToHashCode();
}

bool CompiledShaderVariant::HasShader() const noexcept {
    return Vertex != nullptr || Pixel != nullptr || Compute != nullptr;
}

ShaderVariantCache::~ShaderVariantCache() noexcept {
    Clear();
}

const ShaderVariantEntry* ShaderVariantCache::Find(const ShaderVariantKey& sourceKey) const {
    ShaderVariantKey key = sourceKey;
    key.Normalize();
    const auto it = _variants.find(key);
    return it == _variants.end() ? nullptr : &it->second;
}

ShaderVariantEntry* ShaderVariantCache::Find(const ShaderVariantKey& sourceKey) {
    ShaderVariantKey key = sourceKey;
    key.Normalize();
    const auto it = _variants.find(key);
    return it == _variants.end() ? nullptr : &it->second;
}

ShaderVariantEntry& ShaderVariantCache::GetOrCreateEntry(ShaderVariantKey sourceKey) {
    sourceKey.Normalize();
    auto [it, inserted] = _variants.try_emplace(std::move(sourceKey));
    if (inserted) {
        it->second.State = ShaderVariantState::Building;
    }
    return it->second;
}

const ShaderModuleEntry* ShaderVariantCache::Find(const ShaderModuleKey& sourceKey) const {
    ShaderModuleKey key = sourceKey;
    key.Normalize();
    const auto it = _modules.find(key);
    return it == _modules.end() ? nullptr : &it->second;
}

ShaderModuleEntry* ShaderVariantCache::Find(const ShaderModuleKey& sourceKey) {
    ShaderModuleKey key = sourceKey;
    key.Normalize();
    const auto it = _modules.find(key);
    return it == _modules.end() ? nullptr : &it->second;
}

ShaderModuleEntry& ShaderVariantCache::GetOrCreateEntry(ShaderModuleKey sourceKey) {
    sourceKey.Normalize();
    return _modules.try_emplace(std::move(sourceKey)).first->second;
}

const ShaderPipelineLayoutEntry* ShaderVariantCache::Find(
    const ShaderPipelineLayoutKey& sourceKey) const {
    ShaderPipelineLayoutKey key = sourceKey;
    key.Normalize();
    const auto it = _layouts.find(key);
    return it == _layouts.end() ? nullptr : &it->second;
}

ShaderPipelineLayoutEntry* ShaderVariantCache::Find(const ShaderPipelineLayoutKey& sourceKey) {
    ShaderPipelineLayoutKey key = sourceKey;
    key.Normalize();
    const auto it = _layouts.find(key);
    return it == _layouts.end() ? nullptr : &it->second;
}

ShaderPipelineLayoutEntry& ShaderVariantCache::GetOrCreateEntry(ShaderPipelineLayoutKey sourceKey) {
    sourceKey.Normalize();
    return _layouts.try_emplace(std::move(sourceKey)).first->second;
}

void ShaderVariantCache::Clear() noexcept {
    // variant 只保存裸指针，因此先清它们，再清理被引用的 GPU 对象。
    _variants.clear();
    _modules.clear();
    _layouts.clear();
}

size_t ShaderVariantCache::GetVariantCount() const noexcept {
    return _variants.size();
}

size_t ShaderVariantCache::GetModuleCount() const noexcept {
    return _modules.size();
}

size_t ShaderVariantCache::GetLayoutCount() const noexcept {
    return _layouts.size();
}

}  // namespace radray
