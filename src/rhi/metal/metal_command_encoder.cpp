#include "metal_command_encoder.h"

#include <radray/utility.h>
#include "metal_command_queue.h"
#include "metal_swap_chain.h"

namespace radray::rhi::metal {

MetalCommandEncoder::MetalCommandEncoder(MetalCommandQueue* queue)
    : queue(queue) {}

void MetalCommandEncoder::CheckCmdBuffer() {
    if (cmdBuffer == nullptr) {
        MTL::CommandBufferDescriptor* desc = MTL::CommandBufferDescriptor::alloc()->init();
        auto sg_ = MakeScopeGuard([=]() { desc->release(); });
        desc->setRetainedReferences(false);
#ifdef RADRAY_IS_DEBUG
        desc->setErrorOptions(MTL::CommandBufferErrorOptionEncoderExecutionStatus);
#else
        desc->setErrorOptions(MTL::CommandBufferErrorOptionNone);
#endif
        cmdBuffer = queue->queue->commandBuffer(desc);
    }
}

void MetalCommandEncoder::operator()(const ClearRenderTargetCommand& cmd) {
    auto getTex = [](const std::variant<SwapChainHandle, ResourceHandle>& v) -> MTL::Texture* {
        if (const SwapChainHandle* h = std::get_if<SwapChainHandle>(&v)) {
            MetalSwapChain* swapchain = reinterpret_cast<MetalSwapChain*>(h->Handle);
            return swapchain->currentBackBuffer;
        } else if (const ResourceHandle* h = std::get_if<ResourceHandle>(&v)) {
            return reinterpret_cast<MTL::Texture*>(h->Ptr);
        } else {
            RADRAY_ABORT("unreachable");
            return nullptr;
        }
    };
    CheckCmdBuffer();
    MTL::RenderPassDescriptor* desc = MTL::RenderPassDescriptor::alloc()->init();
    auto sg_ = MakeScopeGuard([=]() { desc->release(); });
    MTL::RenderPassColorAttachmentDescriptorArray* colorAttach = desc->colorAttachments();
    MTL::RenderPassColorAttachmentDescriptor* colorDesc = colorAttach->object(0);
    colorDesc->setTexture(getTex(cmd.texture));
    colorDesc->setLoadAction(MTL::LoadAction::LoadActionClear);
    colorDesc->setClearColor(MTL::ClearColor::Make(cmd.color[0], cmd.color[1], cmd.color[2], cmd.color[3]));
    colorDesc->setStoreAction(MTL::StoreAction::StoreActionStore);
    MTL::RenderCommandEncoder* encoder = cmdBuffer->renderCommandEncoder(desc);
    encoder->endEncoding();
}

}  // namespace radray::rhi::metal
