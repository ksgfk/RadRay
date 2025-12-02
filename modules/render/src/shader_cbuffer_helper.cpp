#include <radray/render/shader_cbuffer_helper.h>

#include <radray/render/root_signature_helper.h>

namespace radray::render {

static void _Gen(const HlslShaderTypeDesc* type) noexcept {
    for (const auto& member : type->Members) {
        if (type->IsPrimitive()) {
            RADRAY_INFO_LOG("primitive: {} {}", member.Name, type->Name);
        } else {
            _Gen(member.Type);
        }
    }
}

std::optional<ShaderCBufferStorage> CreateCBufferStorage(std::span<const HlslShaderDesc*> descs) noexcept {
    // vector<HlslRSCombinedBinding> bindings = MergeHlslShaderBoundResources(descs);
    // if (bindings.empty()) {
    //     return std::nullopt;
    // }
    // for (const auto& binding : bindings) {
    //     if (binding.Type == ResourceBindType::CBuffer) {
            
    //     }
    // }
    return std::nullopt;
}

}  // namespace radray::render
