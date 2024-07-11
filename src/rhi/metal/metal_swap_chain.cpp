#include "metal_swap_chain.h"

#include "metal_device.h"

namespace radray::rhi::metal {

MetalSwapChain::MetalSwapChain(
    MTL::Device* device,
    uint64_t windowHandle,
    uint width,
    uint height,
    bool vsync,
    uint32_t backBufferCount)
    : layer(RadrayMetalCreateLayer(device, windowHandle, width, height, vsync, backBufferCount)),
      presentPassDesc(MTL::RenderPassDescriptor::alloc()->init()),
      backBuffer(std::make_unique<MetalTexture>(
          device,
          MTL::PixelFormat::PixelFormatRGBA8Unorm,
          MTL::TextureType::TextureType2D,
          width, height,
          1,
          1,
          MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite | MTL::TextureUsageRenderTarget)) {
    auto attachment_desc = presentPassDesc->colorAttachments()->object(0);
    attachment_desc->setLoadAction(MTL::LoadActionDontCare);
    attachment_desc->setStoreAction(MTL::StoreActionStore);
}

MetalSwapChain::~MetalSwapChain() noexcept {
    if (layer != nullptr) {
        layer->release();
        layer = nullptr;
    }
    if (presentPassDesc != nullptr) {
        presentPassDesc->release();
        presentPassDesc = nullptr;
    }
}

void MetalSwapChain::Present(MetalDevice* device, MTL::CommandQueue* queue) {
    if (CA::MetalDrawable* drawable = layer->nextDrawable()) {
        MTL::RenderPassColorAttachmentDescriptor* colorAttachment = presentPassDesc->colorAttachments()->object(0);
        colorAttachment->setTexture(drawable->texture());
        MTL::CommandBuffer* cmdBuffer = queue->commandBufferWithUnretainedReferences();
        MTL::RenderCommandEncoder* encoder = cmdBuffer->renderCommandEncoder(presentPassDesc);
        float vertices[]{-1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f, -1.f};
        encoder->setRenderPipelineState(device->swapchainPresentPso);
        encoder->setVertexBytes(&vertices, sizeof(vertices), 0);
        encoder->setFragmentTexture(backBuffer->texture, 0);
        encoder->drawPrimitives(
            MTL::PrimitiveTypeTriangleStrip,
            static_cast<NS::UInteger>(0),
            static_cast<NS::UInteger>(4));
        encoder->endEncoding();
        cmdBuffer->presentDrawable(drawable);
        cmdBuffer->commit();
    } else {
        RADRAY_WARN_LOG("Failed to acquire next drawable from swapchain.");
    }
}

}  // namespace radray::rhi::metal
