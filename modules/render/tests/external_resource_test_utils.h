#pragma once

#include "gpu_test_fixture.h"
#include <radray/render/external_resource.h>

namespace radray::render::test {

enum class ExternalTextureTestUse : uint32_t { Copy,
                                               Sampled,
                                               Storage,
                                               RenderTarget };
struct ExternalTestSlot {
    unique_ptr<Buffer> BufferObject;
    unique_ptr<Texture> TextureObject;
};
struct ExternalTestExports {
    ExternalBufferExport BufferObject;
    ExternalTextureExport TextureObject;
};

testing::AssertionResult ExternalSupportQuerySucceeded(const ExternalSupport& support);
ExternalSupport QueryExternalTextureSupport(DeviceContext& context, const TextureDescriptor& descriptor, const ExternalTextureDescriptor& options, std::string_view label);

BufferDescriptor ExternalTestBufferDescriptor(uint32_t generation);
TextureDescriptor ExternalTestTextureDescriptor(TextureFormat format, ExternalTextureTestUse use, uint32_t generation);
bool CreateExternalTestSlot(ExternalResourceExtension& extension, TextureFormat format, ExternalTextureTestUse use, uint32_t generation, ExternalTestSlot& slot, const ExternalResourceDescriptor& external = {});
bool ExportExternalTestSlot(ExternalResourceExtension& extension, ExternalTestSlot& slot, ExternalTestExports& exported);
bool ImportExternalTestSlot(ExternalResourceExtension& extension, const ExternalTestExports& exported, ExternalTestSlot& slot);
// Reads incoming content before writing a new pattern, then releases the entire slot.
bool ExternalTestTurn(DeviceContext& context, ExternalTestSlot& slot, ExternalTextureTestUse use,
                      uint32_t expected, uint32_t written, bool first, Fence* wait, uint64_t waitValue,
                      Fence* signal, uint64_t signalValue);

template <class T>
bool TakeExternal(ExternalResult<T> result, T& output) {
    if (auto* error = std::get_if<ExternalError>(&result)) {
        ADD_FAILURE() << "external API error=" << error->Message << " native=" << error->NativeCode;
        return false;
    }
    output = std::get<T>(std::move(result));
    return true;
}
}  // namespace radray::render::test
