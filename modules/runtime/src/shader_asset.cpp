#include <radray/runtime/shader_asset.h>

#include <utility>

#include <radray/logger.h>

namespace radray {

std::optional<uint32_t> ShaderKeywordSet::Add(std::string_view name) noexcept {
    if (name.empty()) {
        RADRAY_ERR_LOG("ShaderKeywordSet::Add: empty keyword name");
        return std::nullopt;
    }
    if (_index.find(name) != _index.end()) {
        RADRAY_ERR_LOG("ShaderKeywordSet::Add: duplicate keyword '{}'", name);
        return std::nullopt;
    }
    if (_names.size() >= kMaxKeywords) {
        RADRAY_ERR_LOG("ShaderKeywordSet::Add: keyword count exceeds limit {} (adding '{}')", kMaxKeywords, name);
        return std::nullopt;
    }
    const uint32_t bit = static_cast<uint32_t>(_names.size());
    _names.emplace_back(name);
    _index.emplace(string{name}, bit);
    return bit;
}

std::optional<uint32_t> ShaderKeywordSet::IndexOf(std::string_view name) const noexcept {
    auto it = _index.find(name);
    if (it == _index.end()) {
        return std::nullopt;
    }
    return it->second;
}

uint64_t ShaderKeywordSet::Project(std::span<const std::string_view> enabled) const noexcept {
    uint64_t mask = 0;
    for (std::string_view name : enabled) {
        auto it = _index.find(name);
        if (it == _index.end()) {
            continue;  // 未声明的 keyword 忽略
        }
        mask |= (uint64_t{1} << it->second);
    }
    return mask;
}

vector<string> ShaderKeywordSet::ResolveDefines(uint64_t bitmask) const noexcept {
    vector<string> defines;
    for (uint32_t i = 0; i < _names.size(); ++i) {
        if ((bitmask & (uint64_t{1} << i)) != 0) {
            defines.emplace_back(fmt::format("{}=1", _names[i]));
        }
    }
    return defines;
}

ShaderAsset::ShaderAsset() noexcept
    : _programId(Guid::NewGuid()) {}

ShaderAsset::ShaderAsset(ShaderKeywordSet keywords, vector<ShaderPassDesc> passes) noexcept
    : _programId(Guid::NewGuid()),
      _keywords(std::move(keywords)),
      _passes(std::move(passes)) {}

ShaderAsset::~ShaderAsset() noexcept = default;

void ShaderAsset::OnUnload(IRenderResourceRecycler& /*recycler*/) {
    // 编译好的变体由 runtime ShaderVariantLibrary 持有并回收, ShaderAsset 不拥有 GPU 资源。
    _passes.clear();
}

AssetTypeId ShaderAsset::GetTypeId() const noexcept {
    return runtime_type_id_v<ShaderAsset>;
}

std::optional<uint32_t> ShaderAsset::FindPassByTag(std::string_view passTag) const noexcept {
    for (uint32_t i = 0; i < _passes.size(); ++i) {
        if (_passes[i].PassTag == passTag) {
            return i;
        }
    }
    return std::nullopt;
}

Nullable<const CompiledShaderVariant*> ShaderAsset::GetOrCreateVariant(
    ShaderVariantLibrary& cache,
    uint32_t passIndex,
    std::span<const std::string_view> enabledKeywords,
    render::HlslShaderModel sm) noexcept {
    if (passIndex >= _passes.size()) {
        RADRAY_ERR_LOG("ShaderAsset::GetOrCreateVariant: pass index {} out of range ({} passes)", passIndex, _passes.size());
        return nullptr;
    }
    const ShaderPassDesc& pass = _passes[passIndex];

    const uint64_t bitmask = _keywords.Project(enabledKeywords) & pass.VariantKeywordMask;
    vector<string> defineStrings = _keywords.ResolveDefines(bitmask);
    vector<std::string_view> defineViews;
    defineViews.reserve(defineStrings.size());
    for (const string& d : defineStrings) {
        defineViews.emplace_back(d);
    }

    const ShaderVariantStageDesc stages[] = {
        ShaderVariantStageDesc{
            .Source = pass.Source,
            .EntryPoint = pass.VertexEntry,
            .Stage = render::ShaderStage::Vertex,
        },
        ShaderVariantStageDesc{
            .Source = pass.Source,
            .EntryPoint = pass.PixelEntry,
            .Stage = render::ShaderStage::Pixel,
        },
    };

    vector<std::string_view> includeViews;
    includeViews.reserve(pass.IncludeDirs.size());
    for (const string& dir : pass.IncludeDirs) {
        includeViews.emplace_back(dir);
    }

    ShaderVariantDescriptor desc{};
    desc.ProgramId = _programId;
    desc.PassIndex = passIndex;
    desc.KeywordBitmask = bitmask;
    desc.Defines = defineViews;
    desc.Includes = includeViews;
    desc.Stages = stages;
    desc.DynamicBufferBindings = pass.DynamicBufferBindings;
    desc.PushConstantBindings = pass.PushConstantBindings;
    if (!pass.InterfaceSchema.Bindings.empty() || !pass.InterfaceSchema.PushConstants.empty()) {
        desc.InterfaceSchema = &pass.InterfaceSchema;
    }
    desc.SM = sm;
    desc.IsOptimize = false;
    desc.LogicalName = pass.ProgramName;
    return cache.GetOrCreate(desc);
}

}  // namespace radray
