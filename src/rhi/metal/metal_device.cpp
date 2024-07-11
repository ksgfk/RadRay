#include "metal_device.h"

#include <cstring>

#include <radray/logger.h>

#include "metal_command_queue.h"
#include "metal_command_encoder.h"
#include "metal_swap_chain.h"
#include "metal_buffer.h"
#include "metal_texture.h"
#include "metal_event.h"

namespace radray::rhi::metal {

static const char* SWAPCHAIN_PRESENT_SHADER = R"(
#include <metal_stdlib>
using namespace metal;
struct RasterData {
    float4 p [[position]];
    float2 uv;
};
[[vertex]] RasterData swapchain_vert(constant float2* in [[buffer(0)]], uint vid [[vertex_id]]) {
    auto p = in[vid];
    return RasterData{float4(p, 0.0f, 1.0f), saturate(p * float2(0.5f, -0.5f) + 0.5f)};
}
[[fragment]] float4 swapchain_frag(RasterData in [[stage_in]], texture2d<float> image [[texture(0)]]) {
    return float4(image.sample(sampler(filter::linear), in.uv).xyz, 1.0f);
})";

MetalDevice::MetalDevice() = default;

MetalDevice::~MetalDevice() noexcept {
    if (device != nullptr) {
        device->release();
        device = nullptr;
    }
    if (swapchainPresentPso != nullptr) {
        swapchainPresentPso->release();
        swapchainPresentPso = nullptr;
    }
}

std::shared_ptr<MetalDevice> CreateImpl(const DeviceCreateInfoMetal& info) {
    ScopedAutoreleasePool arp_{};
    NS::Array* allDevice = MTL::CopyAllDevices()->autorelease();
    if (!allDevice->count()) {
        RADRAY_WARN_LOG("metal no device");
        return nullptr;
    }
    auto deviceCount = allDevice->count();
    for (size_t i = 0; i < deviceCount; i++) {
        MTL::Device* d = allDevice->object<MTL::Device>(i);
        RADRAY_INFO_LOG("metal find device: {}", d->name()->utf8String());
    }
    if (info.DeviceIndex >= deviceCount) {
        RADRAY_ERR_LOG("device index out of range. count = {}", deviceCount);
        return nullptr;
    }
    MTL::Device* device = allDevice->object<MTL::Device>(info.DeviceIndex.value_or(0));
    RADRAY_INFO_LOG("select metal device: {}", device->name()->utf8String());

    MTL::CompileOptions* compileOption = MTL::CompileOptions::alloc()->init()->autorelease();
    compileOption->setFastMathEnabled(true);
    compileOption->setLanguageVersion(MTL::LanguageVersion2_4);
    compileOption->setLibraryType(MTL::LibraryTypeExecutable);
    NS::String* builtinSource = NS::String::alloc()->init(
        const_cast<char*>(SWAPCHAIN_PRESENT_SHADER),
        std::strlen(SWAPCHAIN_PRESENT_SHADER),
        NS::UTF8StringEncoding,
        false);
    builtinSource->autorelease();
    NS::Error* error{nullptr};
    MTL::Library* builtinLibrary = device->newLibrary(builtinSource, compileOption, &error)->autorelease();
    builtinLibrary->setLabel(MTLSTR("radray_builtin"));
    if (error != nullptr) {
        RADRAY_ERR_LOG("cannot compile built-in shaders.\n{}", error->localizedDescription()->utf8String());
        return nullptr;
    }
    error = nullptr;
    if (builtinLibrary == nullptr) {
        RADRAY_ERR_LOG("cannot compile built-in shaders");
        return nullptr;
    }
    auto createRasterShader = [builtinLibrary](NS::String* name) -> MTL::Function* {
        MTL::FunctionDescriptor* funcDesc = MTL::FunctionDescriptor::alloc()->init()->autorelease();
        funcDesc->setName(name);
        NS::Error* funcErr{nullptr};
        MTL::Function* shader = builtinLibrary->newFunction(funcDesc, &funcErr)->autorelease();
        if (funcErr != nullptr) {
            RADRAY_ERR_LOG("cannot compile built-in shader {}.\n{}", name->utf8String(), funcErr->localizedDescription()->utf8String());
            return nullptr;
        }
        if (shader == nullptr) {
            RADRAY_ERR_LOG("cannot compile built-in shader {}", name->utf8String());
        }
        return shader;
    };
    MTL::Function* swapchainVert = createRasterShader(MTLSTR("swapchain_vert"));
    if (swapchainVert == nullptr) {
        return nullptr;
    }
    MTL::Function* swapchainFrag = createRasterShader(MTLSTR("swapchain_frag"));
    if (swapchainFrag == nullptr) {
        return nullptr;
    }
    MTL::RenderPipelineDescriptor* swapchainPipeDesc = MTL::RenderPipelineDescriptor::alloc()->init()->autorelease();
    swapchainPipeDesc->setVertexFunction(swapchainVert);
    swapchainPipeDesc->setFragmentFunction(swapchainFrag);
    auto colorAttachment = swapchainPipeDesc->colorAttachments()->object(0u);
    colorAttachment->setBlendingEnabled(false);
    colorAttachment->setPixelFormat(MTL::PixelFormat::PixelFormatBGRA8Unorm);
    MTL::RenderPipelineState* swapchainPso = device->newRenderPipelineState(swapchainPipeDesc, MTL::PipelineOptionNone, nullptr, &error);
    if (error != nullptr) {
        RADRAY_ERR_LOG("cannot create pso {}.\n{}", "swapchain", error->localizedDescription()->utf8String());
        return nullptr;
    }
    if (swapchainPso == nullptr) {
        RADRAY_ERR_LOG("cannot create pso swapchain");
        return nullptr;
    }

    auto result = std::make_shared<MetalDevice>();
    result->device = device;
    result->swapchainPresentPso = swapchainPso;
    return result;
}

CommandQueueHandle MetalDevice::CreateCommandQueue(CommandListType type) {
    ScopedAutoreleasePool arp_{};
    MetalCommandQueue* mcq = new MetalCommandQueue{this->device, 0};
    return CommandQueueHandle{
        reinterpret_cast<uint64_t>(mcq),
        mcq->queue};
}

void MetalDevice::DestroyCommandQueue(CommandQueueHandle handle) {
    ScopedAutoreleasePool arp_{};
    auto mcq = reinterpret_cast<MetalCommandQueue*>(handle.Handle);
    delete mcq;
}

FenceHandle MetalDevice::CreateFence() {
    ScopedAutoreleasePool arp_{};
    auto e = new MetalEvent{this->device};
    return FenceHandle{
        reinterpret_cast<uint64_t>(e),
        e->event};
}

void MetalDevice::DestroyFence(FenceHandle handle) {
    ScopedAutoreleasePool arp_{};
    auto e = reinterpret_cast<MetalEvent*>(handle.Handle);
    delete e;
}

SwapChainHandle MetalDevice::CreateSwapChain(const SwapChainCreateInfo& info, uint64_t cmdQueueHandle) {
    ScopedAutoreleasePool arp_{};
    MetalSwapChain* msc = new MetalSwapChain{
        this->device,
        info.WindowHandle,
        info.Width, info.Height,
        info.Vsync,
        info.BackBufferCount};
    return SwapChainHandle{
        reinterpret_cast<uint64_t>(msc),
        msc->layer};
}

void MetalDevice::DestroySwapChain(SwapChainHandle handle) {
    ScopedAutoreleasePool arp_{};
    auto msc = reinterpret_cast<MetalSwapChain*>(handle.Handle);
    delete msc;
}

ResourceHandle MetalDevice::CreateBuffer(BufferType type, uint64_t size) {
    ScopedAutoreleasePool arp_{};
    MetalBuffer* buf = new MetalBuffer{this->device, size};
    return ResourceHandle{
        reinterpret_cast<uint64_t>(buf),
        buf->buffer};
}

void MetalDevice::DestroyBuffer(ResourceHandle handle) {
    ScopedAutoreleasePool arp_{};
    auto buf = reinterpret_cast<MetalBuffer*>(handle.Handle);
    delete buf;
}

ResourceHandle MetalDevice::CreateTexture(
    PixelFormat format,
    TextureDimension dim,
    uint32_t width, uint32_t height,
    uint32_t depth,
    uint32_t mipmap) {
    ScopedAutoreleasePool arp_{};
    MetalTexture* tex = new MetalTexture{
        this->device,
        ToMtlFormat(format),
        ToMtlTextureType(dim),
        width, height,
        depth,
        mipmap};
    return ResourceHandle{
        reinterpret_cast<uint64_t>(tex),
        tex->texture};
}

void MetalDevice::DestroyTexture(ResourceHandle handle) {
    ScopedAutoreleasePool arp_{};
    auto tex = reinterpret_cast<MetalTexture*>(handle.Handle);
    delete tex;
}

void MetalDevice::DispatchCommand(CommandQueueHandle queue, CommandList&& cmdList_) {
    ScopedAutoreleasePool _arp{};
    auto q = reinterpret_cast<MetalCommandQueue*>(queue.Handle);
    CommandList cmdList = std::move(cmdList_);
    MetalCommandEncoder encoder{q};
    for (auto&& cmd : cmdList.list) {
        std::visit(encoder, cmd);
    }
#ifdef RADRAY_IS_DEBUG
    if (encoder.cmdBuffer != nullptr) {
        encoder.cmdBuffer->addCompletedHandler(^(MTL::CommandBuffer* cmdBuffer) noexcept {
          if (auto err = cmdBuffer->error()) {
              RADRAY_ERR_LOG("MTL::CommnadBuffer execute error: {}", err->localizedDescription()->utf8String());
          }
          if (auto logs = cmdBuffer->logs()) {
              RadrayPrintMTLFunctionLog(logs);
          }
        });
    }
#endif
    if (encoder.cmdBuffer != nullptr) {
        encoder.cmdBuffer->commit();
    }
}

void MetalDevice::Signal(FenceHandle fence, CommandQueueHandle queue, uint64_t value) {
    ScopedAutoreleasePool arp_{};
    auto e = reinterpret_cast<MetalEvent*>(fence.Handle);
    auto q = reinterpret_cast<MetalCommandQueue*>(queue.Handle);
    q->Signal(e->event, value);
}

void MetalDevice::Wait(FenceHandle fence, CommandQueueHandle queue, uint64_t value) {
    ScopedAutoreleasePool arp_{};
    auto e = reinterpret_cast<MetalEvent*>(fence.Handle);
    auto q = reinterpret_cast<MetalCommandQueue*>(queue.Handle);
    q->Wait(e->event, value);
}

void MetalDevice::Synchronize(FenceHandle fence, uint64_t value) {
    ScopedAutoreleasePool arp_{};
    auto e = reinterpret_cast<MetalEvent*>(fence.Handle);
    e->Synchronize(value);
}

void MetalDevice::Present(SwapChainHandle swapchain, CommandQueueHandle queue) {
    ScopedAutoreleasePool arp_{};
    auto q = reinterpret_cast<MetalCommandQueue*>(queue.Handle);
    auto msc = reinterpret_cast<MetalSwapChain*>(swapchain.Handle);
    msc->Present(this, q->queue);
}

}  // namespace radray::rhi::metal
