#include "metal_device.h"

#include <radray/stopwatch.h>

#include "dispatch_semaphore.h"
#include "metal_command_queue.h"
#include "metal_command_buffer.h"
#include "metal_command_encoder.h"
#include "metal_swapchain.h"
#include "metal_texture.h"
#include "metal_library.h"
#include "metal_pipeline_state.h"

namespace radray::rhi::metal {

static CommandQueue* Underlying(RadrayCommandQueue queue) noexcept { return reinterpret_cast<CommandQueue*>(queue.Ptr); }
static CommandQueue* Underlying(RadrayCommandAllocator alloc) noexcept { return reinterpret_cast<CommandQueue*>(alloc.Ptr); }
static CommandBuffer* Underlying(RadrayCommandList list) noexcept { return reinterpret_cast<CommandBuffer*>(list.Ptr); }
static RenderCommandEncoder* Underlying(RadrayRenderPassEncoder encoder) noexcept { return reinterpret_cast<RenderCommandEncoder*>(encoder.Ptr); }
static Semaphore* Underlying(RadrayFence fence) noexcept { return reinterpret_cast<Semaphore*>(fence.Ptr); }
static SwapChain* Underlying(RadraySwapChain swapchain) noexcept { return reinterpret_cast<SwapChain*>(swapchain.Ptr); }
static Texture* Underlying(RadrayTexture texture) noexcept { return reinterpret_cast<Texture*>(texture.Ptr); }
static TextureView* Underlying(RadrayTextureView view) noexcept { return reinterpret_cast<TextureView*>(view.Handle); }
static Library* Underlying(RadrayShader shader) noexcept { return reinterpret_cast<Library*>(shader.Ptr); }
static RenderPipelineState* Underlying(RadrayGraphicsPipeline pso) noexcept { return reinterpret_cast<RenderPipelineState*>(pso.Ptr); }

Device::Device(const RadrayDeviceDescriptorMetal& desc) {
    AutoRelease([this, &desc]() {
        auto allDevices = MTL::CopyAllDevices()->autorelease();
        auto deviceCount = allDevices->count();
        uint32_t deviceIndex = desc.DeviceIndex == RADRAY_RHI_AUTO_SELECT_DEVICE ? 0 : desc.DeviceIndex;
        if (deviceIndex >= deviceCount) {
            RADRAY_MTL_THROW("Metal device index out of range (need={}, count={})", desc.DeviceIndex, deviceCount);
        }
        device = allDevices->object<MTL::Device>(deviceIndex)->retain();
        RADRAY_INFO_LOG("Metal device: {}", device->name()->utf8String());
    });
    shaderCompiler = std::make_shared<ShaderCompilerBridge>();
}

Device::~Device() noexcept {
    AutoRelease([this]() {
        device->release();
    });
}

RadrayCommandQueue Device::CreateCommandQueue(RadrayQueueType type) {
    RADRAY_UNUSED(type);
    return AutoRelease([this]() {
        auto q = RhiNew<CommandQueue>(device);
        return RadrayCommandQueue{q, q->queue};
    });
}

void Device::DestroyCommandQueue(RadrayCommandQueue queue) {
    AutoRelease([queue]() {
        auto q = Underlying(queue);
        RhiDelete(q);
    });
}

void Device::SubmitQueue(const RadraySubmitQueueDescriptor& desc) {
    AutoRelease([&desc]() {
        auto q = Underlying(desc.Queue);
        for (size_t i = 0; i < desc.ListCount; i++) {
            auto cb = Underlying(desc.Lists[i]);
            cb->Commit();
        }
        if (!RADRAY_RHI_IS_EMPTY_RES(desc.SignalFence)) {
            auto sem = Underlying(desc.SignalFence);
            auto cb = q->queue->commandBufferWithUnretainedReferences();
            cb->addCompletedHandler(^(MTL::CommandBuffer* cmdBuf) {
              RADRAY_UNUSED(cmdBuf);
              sem->Signal();
            });
            cb->commit();
        }
    });
}

void Device::WaitQueue(RadrayCommandQueue queue) {
    AutoRelease([queue]() {
        auto q = Underlying(queue);
        MTL::CommandBuffer* cb = q->queue->commandBufferWithUnretainedReferences();
        cb->commit();
        cb->waitUntilCompleted();
    });
}

RadrayFence Device::CreateFence() {
    return AutoRelease([]() {
        auto e = RhiNew<Semaphore>(1);
        return RadrayFence{e, e->semaphore};
    });
}

void Device::DestroyFence(RadrayFence fence) {
    AutoRelease([fence]() {
        auto e = Underlying(fence);
        RhiDelete(e);
    });
}

RadrayFenceState Device::GetFenceState(RadrayFence fence) {
    return AutoRelease([fence]() {
        auto e = Underlying(fence);
        return e->count < 0 ? RADRAY_FENCE_STATE_INCOMPLETE : RADRAY_FENCE_STATE_COMPLETE;
    });
}

void Device::WaitFences(std::span<const RadrayFence> fences) {
    AutoRelease([fences]() {
        for (auto&& i : fences) {
            auto e = Underlying(i);
            e->Wait();
        }
    });
}

RadrayCommandAllocator Device::CreateCommandAllocator(RadrayCommandQueue queue) {
    return RadrayCommandAllocator{queue.Ptr, queue.Native};
}

void Device::DestroyCommandAllocator(RadrayCommandAllocator alloc) {
    RADRAY_UNUSED(alloc);
}

RadrayCommandList Device::CreateCommandList(RadrayCommandAllocator alloc) {
    return AutoRelease([alloc]() {
        auto q = Underlying(alloc);
        auto cb = RhiNew<CommandBuffer>(q->queue);
        return RadrayCommandList{cb, cb->cmdBuffer};
    });
}

void Device::DestroyCommandList(RadrayCommandList list) {
    AutoRelease([list]() {
        auto cb = Underlying(list);
        RhiDelete(cb);
    });
}

void Device::ResetCommandAllocator(RadrayCommandAllocator alloc) {
    RADRAY_UNUSED(alloc);
}

void Device::BeginCommandList(RadrayCommandList list) {
    AutoRelease([list]() {
        auto cb = Underlying(list);
        cb->Begin();
    });
}

void Device::EndCommandList(RadrayCommandList list) {
    RADRAY_UNUSED(list);
}

RadrayRenderPassEncoder Device::BeginRenderPass(const RadrayRenderPassDescriptor& desc) {
    return AutoRelease([&desc]() {
        auto cb = Underlying(desc.List);
        auto rp = MTL::RenderPassDescriptor::renderPassDescriptor()->autorelease();
        auto colorArray = rp->colorAttachments();
        for (size_t i = 0; i < desc.ColorCount; i++) {
            auto&& radColor = desc.Colors[i];
            auto color = colorArray->object(i);
            auto colorView = Underlying(radColor.View);
            auto clear = MTL::ClearColor::Make(radColor.Clear.R, radColor.Clear.G, radColor.Clear.B, radColor.Clear.A);
            color->setTexture(colorView->texture);
            color->setClearColor(clear);
            color->setLoadAction(EnumConvert(radColor.Load));
            color->setStoreAction(EnumConvert(radColor.Store));
        }
        if (desc.DepthStencil != nullptr) {
            auto&& radDs = *desc.DepthStencil;
            auto depth = rp->depthAttachment();
            auto dsView = Underlying(radDs.View);
            depth->setTexture(dsView->texture);
            depth->setClearDepth(radDs.DepthClear);
            depth->setLoadAction(EnumConvert(radDs.DepthLoad));
            depth->setStoreAction(EnumConvert(radDs.DepthStore));
            auto stencil = rp->stencilAttachment();
            stencil->setTexture(dsView->texture);
            stencil->setClearStencil(radDs.StencilClear);
            stencil->setLoadAction(EnumConvert(radDs.StencilLoad));
            stencil->setStoreAction(EnumConvert(radDs.StencilStore));
        }
        auto rpe = RhiNew<RenderCommandEncoder>(cb->cmdBuffer, rp);
        return RadrayRenderPassEncoder{rpe, rpe->encoder};
    });
}

void Device::EndRenderPass(RadrayRenderPassEncoder encoder) {
    AutoRelease([encoder]() {
        auto rpe = Underlying(encoder);
        RhiDelete(rpe);
    });
}

void Device::ResourceBarriers(RadrayCommandList list, const RadrayResourceBarriersDescriptor& desc) {
    RADRAY_UNUSED(list);
    RADRAY_UNUSED(desc);
}

RadraySwapChain Device::CreateSwapChain(const RadraySwapChainDescriptor& desc) {
    return AutoRelease([&desc, this]() {
        auto q = Underlying(desc.PresentQueue);
        auto sc = RhiNew<SwapChain>(
            device,
            q->queue,
            desc.NativeWindow,
            desc.Width,
            desc.Height,
            desc.BackBufferCount,
            EnumConvert(desc.Format),
            desc.EnableSync);
        return RadraySwapChain{sc, sc->layer};
    });
}

void Device::DestroySwapChian(RadraySwapChain swapchain) {
    AutoRelease([swapchain]() {
        auto sc = Underlying(swapchain);
        RhiDelete(sc);
    });
}

RadrayTexture Device::AcquireNextRenderTarget(RadraySwapChain swapchain) {
    return AutoRelease([swapchain]() {
        auto sc = Underlying(swapchain);
        if (sc->presented != nullptr) {
            RhiDelete(sc->presented);
            sc->presented = nullptr;
        }
        auto drawable = sc->layer->nextDrawable();
        if (drawable == nullptr) {
            RADRAY_MTL_THROW("metal cannot acquire next drawable");
        }
        auto rt = RhiNew<MetalDrawableTexture>(drawable);
        return RadrayTexture{rt, rt->texture};
    });
}

void Device::Present(RadraySwapChain swapchain, RadrayTexture currentRt) {
    AutoRelease([swapchain, currentRt]() {
        auto sc = Underlying(swapchain);
        auto tex = static_cast<MetalDrawableTexture*>(Underlying(currentRt));
        MTL::CommandBuffer* cmdBuffer = sc->queue->commandBufferWithUnretainedReferences();
        cmdBuffer->presentDrawable(tex->drawable);
        cmdBuffer->commit();
        sc->presented = tex;
    });
}

RadrayBuffer Device::CreateBuffer(const RadrayBufferDescriptor& desc) {
    RADRAY_MTL_THROW("no impl");
}
void Device::DestroyBuffer(RadrayBuffer buffer) {
    RADRAY_MTL_THROW("no impl");
}
RadrayBufferView Device::CreateBufferView(const RadrayBufferViewDescriptor& desc) {
    RADRAY_MTL_THROW("no impl");
}
void Device::DestroyBufferView(RadrayBuffer buffer, RadrayBufferView view) {
    RADRAY_MTL_THROW("no impl");
}

RadrayTexture Device::CreateTexture(const RadrayTextureDescriptor& desc) {
    return AutoRelease([this, &desc]() {
        auto td = MTL::TextureDescriptor::alloc()->init()->autorelease();
        td->setWidth(desc.Width);
        td->setHeight(desc.Height);
        td->setDepth(desc.Depth);
        td->setArrayLength(desc.ArraySize);
        td->setPixelFormat(EnumConvert(desc.Format));
        td->setMipmapLevelCount(desc.MipLevels);
        td->setSampleCount(EnumConvert(desc.SampleCount));
        td->setTextureType(EnumConvert(desc.Dimension));
        td->setStorageMode(MTL::StorageModePrivate);
        auto tex = RhiNew<Texture>(device, td);
        auto name = NS::String::alloc()->init(desc.Name, NS::UTF8StringEncoding)->autorelease();
        tex->texture->setLabel(name);
        return RadrayTexture{tex, tex->texture};
    });
}

void Device::DestroyTexture(RadrayTexture texture) {
    AutoRelease([texture]() {
        auto t = Underlying(texture);
        RhiDelete(t);
    });
}

RadrayTextureView Device::CreateTextureView(const RadrayTextureViewDescriptor& desc) {
    return AutoRelease([&desc]() {
        auto tex = Underlying(desc.Texture);
        MTL::TextureType type = EnumConvert(desc.Dimension);
        if (tex->texture->textureType() == MTL::TextureType2DMultisample) {
            type = MTL::TextureType2DMultisample;
        }
        MTL::PixelFormat format = EnumConvert(desc.Format);
        bool isEqFmt = format == tex->texture->pixelFormat();
        bool isEqType = type == tex->texture->textureType();
        bool isAllRes = desc.BaseMipLevel == 0 &&
                        desc.MipLevelCount == tex->texture->mipmapLevelCount() &&
                        desc.BaseArrayLayer == 0 &&
                        desc.ArrayLayerCount == tex->texture->arrayLength();
        RadrayTextureView result;
        if (isEqFmt && isEqType && isAllRes) {
            auto cp = RhiNew<TextureView>(tex);
            result = {cp};
        } else {
            auto v = RhiNew<TextureView>(
                tex,
                format,
                type,
                desc.BaseMipLevel, desc.MipLevelCount,
                desc.BaseArrayLayer, desc.ArrayLayerCount);
            result = {v};
        }
        return result;
    });
}

void Device::DestroyTextureView(RadrayTextureView view) {
    AutoRelease([view]() {
        auto v = Underlying(view);
        RhiDelete(v);
    });
}

RadrayShader Device::CompileShader(const RadrayCompileRasterizationShaderDescriptor& desc) {
    return AutoRelease([this, &desc]() {
        Stopwatch sw{};
        sw.Start();
        auto cr = shaderCompiler->DxcHlslToDxil(desc);
        RadrayShader result{nullptr, nullptr};
        if (auto err = std::get_if<radray::string>(&cr)) {
            RADRAY_ERR_LOG("cannot compile shader {}\n{}", desc.Name, *err);
            RADRAY_MTL_THROW("{}", *err);
        } else if (auto bc = std::get_if<DxilWithReflection>(&cr)) {
            auto&& dxil = bc->Dxil;
            auto cvtr = shaderCompiler->MscDxilToMetallib(dxil.GetView(), ToMscStage(desc.Stage));
            if (auto err = std::get_if<radray::string>(&cvtr)) {
                RADRAY_ERR_LOG("cannot convert ir {}\n{}", desc.Name, *err);
                RADRAY_MTL_THROW("{}", *err);
            } else if (auto mlib = std::get_if<CompilerBlob>(&cvtr)) {
                auto libbyte = mlib->GetView();
                auto mtllib = RhiNew<Library>(device, libbyte, desc.EntryPoint);
                result = {mtllib, mtllib->lib};
            }
        }
        sw.Stop();
        RADRAY_INFO_LOG(
            "compile shader name={} stage={} entry={} ({}ms)",
            desc.Name,
            desc.Stage,
            desc.EntryPoint,
            sw.ElapsedMilliseconds());
        return result;
    });
}

void Device::DestroyShader(RadrayShader shader) {
    AutoRelease([shader]() {
        auto s = Underlying(shader);
        RhiDelete(s);
    });
}

RadrayRootSignature Device::CreateRootSignature(const RadrayRootSignatureDescriptor& desc) {
    RADRAY_UNUSED(desc);
    return RADRAY_RHI_EMPTY_RES(RadrayRootSignature);
}

void Device::DestroyRootSignature(RadrayRootSignature rootSig) {
    RADRAY_UNUSED(rootSig);
}

RadrayGraphicsPipeline Device::CreateGraphicsPipeline(const RadrayGraphicsPipelineDescriptor& desc) {
    return AutoRelease([this, &desc]() {
        auto rpdesc = MTL::RenderPipelineDescriptor::alloc()->init()->autorelease();
        if (desc.Primitive.Polygon == RADRAY_POLYGON_MODE_POINT) {
            RADRAY_MTL_THROW("metal does not support point polygon");
        }
        auto rawFillMode = EnumConvert(desc.Primitive.Polygon);
        auto&& [primClass, primType] = EnumConvert(desc.Primitive.Topology);
        {
            auto vs = Underlying(desc.VertexShader);
            rpdesc->setVertexFunction(vs->entryPoint);
            if (RADRAY_RHI_IS_EMPTY_RES(desc.PixelShader)) {
                if (desc.ColorTargetCount == 0 && !desc.HasDepthStencil) {
                    rpdesc->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);
                }
            } else {
                auto ps = Underlying(desc.PixelShader);
                rpdesc->setFragmentFunction(ps->entryPoint);
            }
        }
        for (size_t i = 0; i < desc.ColorTargetCount; i++) {
            const auto& rt = desc.ColorTargets[i];
            auto rtDesc = rpdesc->colorAttachments()->object(i);
            auto rawFormat = EnumConvert(rt.Format);
            rtDesc->setPixelFormat(MTL::PixelFormatInvalid);
            if (rawFormat == MTL::PixelFormatInvalid) {
                continue;
            }
            auto rawWriteMask = EnumConvert(rt.WriteMask);
            rtDesc->setWriteMask(rawWriteMask);
            if (rt.EnableBlend) {
                rtDesc->setBlendingEnabled(true);
                const auto& bld = rt.Blend;
                auto&& [colorOp, colorSrc, colorDst] = EnumConvert(bld.Color);
                rtDesc->setRgbBlendOperation(colorOp);
                rtDesc->setSourceRGBBlendFactor(colorSrc);
                
                auto&& [alphaOp, alphaSrc, alphaDst] = EnumConvert(bld.Alpha);
            }
        }

        // rpdesc->setVertexFunction(vs->entryPoint);
        // rpdesc->setFragmentFunction(ps->entryPoint);
        // {
        //     auto vd = MTL::VertexDescriptor::vertexDescriptor()->autorelease();
        //     auto layouts = vd->layouts();
        //     auto attrs = vd->attributes();
        //     for (size_t i = 0; i < desc.VertexLayout.ElementCount; i++) {
        //         const auto& ve = desc.VertexLayout.Elements[i];
        //         auto attr = attrs->object(i);
        //         attr->setFormat(EnumConvert(ve.Format));
        //         attr->setOffset(ve.Offset);
        //         attr->setBufferIndex(ve.Binding);
        //     }
        //     for (size_t i = 0; i < desc.VertexLayout.LayoutCount; i++) {
        //         const auto& vl = desc.VertexLayout.Layouts[i];
        //         auto layout = layouts->object(i);
        //         layout->setStride(vl.Stride);
        //         layout->setStepFunction(EnumConvert(vl.Rate));
        //     }
        //     rpdesc->setVertexDescriptor(vd);
        // }
        // rpdesc->setRasterSampleCount(EnumConvert(desc.SampleCount));
        // // rpdesc->setAlphaToCoverageEnabled();
        // rpdesc->setRasterizationEnabled(true);
        // {
        //     auto colors = rpdesc->colorAttachments();
        //     for (size_t i = 0; i < desc.RenderTargetCount; i++) {
        //         auto format = desc.ColorFormats[i];
        //         const auto& blend = desc.ColorBlends[i];
        //         auto color = colors->object(i);
        //         color->setPixelFormat(EnumConvert(format));
        //         color->setBlendingEnabled(blend.IsEnableBlend);
        //         if (blend.IsEnableBlend) {
        //             color->setSourceRGBBlendFactor(EnumConvert(blend.SrcFactors));
        //             color->setDestinationRGBBlendFactor(EnumConvert(blend.DstFactors));
        //             color->setRgbBlendOperation(EnumConvert(blend.BlendModes));
        //             color->setSourceAlphaBlendFactor(EnumConvert(blend.SrcAlphaFactors));
        //             color->setDestinationAlphaBlendFactor(EnumConvert(blend.DstAlphaFactors));
        //             color->setAlphaBlendOperation(EnumConvert(blend.BlendAlphaModes));
        //             color->setWriteMask(blend.Masks);
        //         }
        //     }
        // }
        // if (desc.DepthStencil.IsEnableDepthTest) {
        //     rpdesc->setDepthAttachmentPixelFormat(EnumConvert(desc.DepthStencilFormat));
        //     rpdesc->setStencilAttachmentPixelFormat(EnumConvert(desc.DepthStencilFormat));
        // }
        // rpdesc->setInputPrimitiveTopology(EnumConvert(desc.PrimitiveTopology));
        auto rps = RhiNew<RenderPipelineState>(device, rpdesc);
        return RadrayGraphicsPipeline{rps, rps->pso};
    });
}

void Device::DestroyGraphicsPipeline(RadrayGraphicsPipeline pipe) {
    AutoRelease([pipe]() {
        auto rps = Underlying(pipe);
        RhiDelete(rps);
    });
}

}  // namespace radray::rhi::metal
