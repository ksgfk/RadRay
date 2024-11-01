#include "metal_helper.h"

namespace radray::render::metal {

ScopedAutoreleasePool::ScopedAutoreleasePool() noexcept
    : _pool{NS::AutoreleasePool::alloc()->init()} {}

ScopedAutoreleasePool::~ScopedAutoreleasePool() noexcept {
    _pool->release();
}

}  // namespace radray::render::metal

namespace MTL {
std::string_view format_as(LanguageVersion v) noexcept {
    switch (v) {
        case LanguageVersion1_0: return "1.0";
        case LanguageVersion1_1: return "1.1";
        case LanguageVersion1_2: return "1.2";
        case LanguageVersion2_0: return "2.0";
        case LanguageVersion2_1: return "2.1";
        case LanguageVersion2_2: return "2.2";
        case LanguageVersion2_3: return "2.3";
        case LanguageVersion2_4: return "2.4";
        case LanguageVersion3_0: return "3.0";
        case LanguageVersion3_1: return "3.1";
        case LanguageVersion3_2: return "3.2";
    }
}
std::string_view format_as(GPUFamily v) noexcept {
    switch (v) {
        case GPUFamilyApple1: return "Apple1";
        case GPUFamilyApple2: return "Apple2";
        case GPUFamilyApple3: return "Apple3";
        case GPUFamilyApple4: return "Apple4";
        case GPUFamilyApple5: return "Apple5";
        case GPUFamilyApple6: return "Apple6";
        case GPUFamilyApple7: return "Apple7";
        case GPUFamilyApple8: return "Apple8";
        case GPUFamilyApple9: return "Apple9";
        case GPUFamilyMac1: return "Mac1";
        case GPUFamilyMac2: return "Mac2";
        case GPUFamilyCommon1: return "Common1";
        case GPUFamilyCommon2: return "Common2";
        case GPUFamilyCommon3: return "Common3";
        case GPUFamilyMacCatalyst1: return "MacCatalyst1";
        case GPUFamilyMacCatalyst2: return "MacCatalyst2";
        case GPUFamilyMetal3: return "Metal3";
    }
}
}  // namespace MTL
