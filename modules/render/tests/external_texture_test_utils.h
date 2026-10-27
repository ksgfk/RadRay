#pragma once

#include "external_resource_test_utils.h"

namespace radray::render::test {

struct ExternalTextureCase {
    string Name;
    TextureDescriptor Descriptor;
    bool Reinterpret{false};
    ExternalTextureTestUse Use{ExternalTextureTestUse::Copy};
};

vector<ExternalTextureCase> ExternalTextureCases();
// Checks incoming contents before writing a new subresource-specific pattern.
bool ExternalTextureTurn(DeviceContext& context, Texture* texture, ExternalTextureTestUse use,
                         uint32_t expected, uint32_t written, bool first,
                         Fence* wait, uint64_t waitValue, Fence* signal, uint64_t signalValue, Nullable<Buffer*> sharedBuffer = nullptr, bool reinterpretViews = false);
void ExpectTextureDescriptor(const TextureDescriptor& actual, const TextureDescriptor& expected);

}  // namespace radray::render::test
