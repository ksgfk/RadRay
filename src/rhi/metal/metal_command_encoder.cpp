#include "metal_command_encoder.h"

#include "metal_command_queue.h"
#include "metal_swap_chain.h"

namespace radray::rhi::metal {

MetalCommandEncoder::MetalCommandEncoder(MetalCommandQueue* queue)
    : queue(queue) {
}

void MetalCommandEncoder::CheckCmdBuffer() {
    if (cmdBuffer == nullptr) {
        auto desc = NS::TransferPtr(MTL::CommandBufferDescriptor::alloc()->init());
        desc->setRetainedReferences(false);
#ifdef RADRAY_IS_DEBUG
        desc->setErrorOptions(MTL::CommandBufferErrorOptionEncoderExecutionStatus);
#else
        desc->setErrorOptions(MTL::CommandBufferErrorOptionNone);
#endif
        cmdBuffer = queue->queue->commandBuffer(desc.get());
    }
}

void MetalCommandEncoder::operator()(const ClearRenderTargetCommand& cmd) {
    auto getTex = [](const std::variant<SwapChainHandle, ResourceHandle>& v) -> MTL::Texture* {
        if (const SwapChainHandle* h = std::get_if<SwapChainHandle>(&v)) {
            auto swapchain = reinterpret_cast<MetalSwapChain*>(h->Handle);
            return swapchain->nowRt->texture.get();
        } else if (const ResourceHandle* h = std::get_if<ResourceHandle>(&v)) {
            return reinterpret_cast<MTL::Texture*>(h->Ptr);
        } else {
            RADRAY_ABORT("unreachable");
            return nullptr;
        }
    };
    CheckCmdBuffer();
    auto desc = NS::TransferPtr(MTL::RenderPassDescriptor::alloc()->init());
    auto colorAttach = desc->colorAttachments();
    auto colorDesc = colorAttach->object(0);
    colorDesc->setTexture(getTex(cmd.texture));
    colorDesc->setLoadAction(MTL::LoadAction::LoadActionClear);
    colorDesc->setClearColor(MTL::ClearColor::Make(cmd.color[0], cmd.color[1], cmd.color[2], cmd.color[3]));
    colorDesc->setStoreAction(MTL::StoreAction::StoreActionStore);
    auto encoder = cmdBuffer->renderCommandEncoder(desc.get());
    encoder->endEncoding();
}

void MetalCommandEncoder::operator()(const PresentCommand& cmd) {
    CheckCmdBuffer();
    auto swapchain = reinterpret_cast<MetalSwapChain*>(cmd.swapchain.Handle);
    cmdBuffer->presentDrawable(swapchain->nowRt->drawable.get());
    swapchain->NextDrawable();
}

}  // namespace radray::rhi::metal
