#include <radray/logger.h>
#include <radray/render/shader_variant_cache.h>

namespace radray::render {

std::optional<ShaderVariantKey> BuildShaderVariantKey(const ShaderVariantDescriptor& desc) noexcept {
    if (desc.ProgramId.IsEmpty()) {
        RADRAY_ERR_LOG("shader variant cache: ProgramId is empty (assign an identity before caching)");
        return std::nullopt;
    }
    ShaderVariantKey key{};
    key.ProgramId = desc.ProgramId;
    key.PassIndex = desc.PassIndex;
    key.Bitmask = desc.KeywordBitmask;
    return key;
}

}  // namespace radray::render
