#include "d3d12_device.h"

#include <algorithm>

#include <radray/basic_math.h>
#include <radray/utility.h>
#include <radray/stopwatch.h>
#include <radray/logger.h>
#include <radray/rhi/shader_compiler.h>

#include "d3d12_command_queue.h"
#include "d3d12_command_allocator.h"
#include "d3d12_command_list.h"
#include "d3d12_fence.h"
#include "d3d12_swapchain.h"
#include "d3d12_buffer.h"
#include "d3d12_texture.h"
#include "d3d12_shader.h"

namespace radray::rhi::d3d12 {

static CommandQueue* Underlying(RadrayCommandQueue queue) noexcept { return reinterpret_cast<CommandQueue*>(queue.Ptr); }
static Fence* Underlying(RadrayFence fence) noexcept { return reinterpret_cast<Fence*>(fence.Ptr); }
static CommandAllocator* Underlying(RadrayCommandAllocator alloc) noexcept { return reinterpret_cast<CommandAllocator*>(alloc.Ptr); }
static CommandList* Underlying(RadrayCommandList list) noexcept { return reinterpret_cast<CommandList*>(list.Ptr); }
static SwapChain* Underlying(RadraySwapChain swapchain) noexcept { return reinterpret_cast<SwapChain*>(swapchain.Ptr); }
static Buffer* Underlying(RadrayBuffer buffer) noexcept { return reinterpret_cast<Buffer*>(buffer.Ptr); }
static BufferView* Underlying(RadrayBufferView view) noexcept { return reinterpret_cast<BufferView*>(view.Handle); }
static Texture* Underlying(RadrayTexture texture) noexcept { return reinterpret_cast<Texture*>(texture.Ptr); }
static TextureView* Underlying(RadrayTextureView view) noexcept { return reinterpret_cast<TextureView*>(view.Handle); }
static Shader* Underlying(RadrayShader shader) noexcept { return reinterpret_cast<Shader*>(shader.Ptr); }
static CommandList* Underlying(RadrayRenderPassEncoder encoder) noexcept { return reinterpret_cast<CommandList*>(encoder.Ptr); }

Device::Device(const RadrayDeviceDescriptorD3D12& desc) {
    uint32_t dxgiFactoryFlags = 0;
    if (desc.IsEnableDebugLayer) {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
    RADRAY_DX_FTHROW(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(dxgiFactory.GetAddressOf())));
    if (desc.AdapterIndex != RADRAY_RHI_AUTO_SELECT_DEVICE) {
        auto adapterIndex = desc.AdapterIndex;
        RADRAY_DX_FTHROW(dxgiFactory->EnumAdapters1(adapterIndex, adapter.GetAddressOf()));
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, _uuidof(ID3D12Device), nullptr))) {
            adapter = nullptr;
        }
    } else {
        ComPtr<IDXGIAdapter1> temp;
        for (
            auto adapterIndex = 0u;
            dxgiFactory->EnumAdapterByGpuPreference(adapterIndex, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(temp.GetAddressOf())) != DXGI_ERROR_NOT_FOUND;
            adapterIndex++) {
            DXGI_ADAPTER_DESC1 desc;
            temp->GetDesc1(&desc);
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                radray::wstring s{desc.Description};
                RADRAY_DEBUG_LOG("D3D12 find device: {}", Utf8ToString(s));
            }
            temp = nullptr;
        }
        for (
            auto adapterIndex = 0u;
            dxgiFactory->EnumAdapterByGpuPreference(adapterIndex, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(adapter.GetAddressOf())) != DXGI_ERROR_NOT_FOUND;
            adapterIndex++) {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, _uuidof(ID3D12Device), nullptr))) {
                    break;
                }
            }
            adapter = nullptr;
        }
    }
    if (adapter == nullptr) {
        RADRAY_DX_THROW("cannot find devices support D3D12");
    }
    RADRAY_DX_FTHROW(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(device.GetAddressOf())));
    {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        radray::wstring s{desc.Description};
        RADRAY_INFO_LOG("D3D12 device create on device: {}", Utf8ToString(s));
    }
    {
        D3D12MA::ALLOCATOR_DESC desc{};
        desc.Flags = D3D12MA::ALLOCATOR_FLAG_NONE;
        desc.pDevice = device.Get();
        desc.pAdapter = adapter.Get();
        D3D12MA::ALLOCATION_CALLBACKS allocationCallbacks{};
        allocationCallbacks.pAllocate = [](size_t size, size_t alignment, void*) {
            return radray::aligned_alloc(alignment, size);
        };
        allocationCallbacks.pFree = [](void* ptr, void*) {
            if (ptr != nullptr) {
                radray::free(ptr);
            }
        };
        desc.pAllocationCallbacks = &allocationCallbacks;
        desc.Flags |= D3D12MA::ALLOCATOR_FLAG_MSAA_TEXTURES_ALWAYS_COMMITTED;
        RADRAY_DX_FTHROW(D3D12MA::CreateAllocator(&desc, resourceAlloc.GetAddressOf()));
    }
    {
        cbvSrvUavHeap = radray::make_unique<DescriptorHeap>(this, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1 << 18, false);
        rtvHeap = radray::make_unique<DescriptorHeap>(this, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 << 16, false);
        dsvHeap = radray::make_unique<DescriptorHeap>(this, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 << 16, false);
        gpuCbvSrvUavHeap = radray::make_unique<DescriptorHeap>(this, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1 << 16, true);
        gpuSamplerHeap = radray::make_unique<DescriptorHeap>(this, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1 << 11, true);
    }
    {
        canSetDebugName = true;
    }
    {
        shaderCompiler = radray::make_shared<ShaderCompilerBridge>();
    }
}

RadrayCommandQueue Device::CreateCommandQueue(RadrayQueueType type) {
    auto q = RhiNew<CommandQueue>(this, EnumConvert(type));
    return {.Ptr = q, .Native = q->queue.Get()};
}

void Device::DestroyCommandQueue(RadrayCommandQueue queue) {
    auto q = Underlying(queue);
    RhiDelete(q);
}

void Device::SubmitQueue(const RadraySubmitQueueDescriptor& desc) {
    RADRAY_ASSERT(desc.ListCount <= std::numeric_limits<UINT>::max());
    auto q = Underlying(desc.Queue);
    // execute
    {
        radray::vector<ID3D12CommandList*> cmds{desc.ListCount};
        for (size_t i = 0; i < desc.ListCount; i++) {
            cmds[i] = Underlying(desc.Lists[i])->list.Get();
        }
        q->queue->ExecuteCommandLists(static_cast<UINT>(cmds.size()), cmds.data());
    }
    // signal
    if (!RADRAY_RHI_IS_EMPTY_RES(desc.SignalFence)) {
        auto f = Underlying(desc.SignalFence);
        f->fenceValue++;
        RADRAY_DX_FTHROW(q->queue->Signal(f->fence.Get(), f->fenceValue));
    }
}

void Device::WaitQueue(RadrayCommandQueue queue) {
    auto q = Underlying(queue);
    auto f = q->fence.get();
    f->fenceValue++;
    q->queue->Signal(f->fence.Get(), f->fenceValue);
    RadrayFence fences[]{RadrayFence{f, f->fence.Get()}};
    WaitFences(fences);
}

RadrayFence Device::CreateFence() {
    auto f = RhiNew<Fence>(this);
    return {.Ptr = f, .Native = f->fence.Get()};
}

void Device::DestroyFence(RadrayFence fence) {
    auto f = Underlying(fence);
    RhiDelete(f);
}

RadrayFenceState Device::GetFenceState(RadrayFence fence) {
    auto f = Underlying(fence);
    uint64_t completeValue = f->fence->GetCompletedValue();
    return completeValue < f->fenceValue ? RADRAY_FENCE_STATE_INCOMPLETE : RADRAY_FENCE_STATE_COMPLETE;
}

void Device::WaitFences(std::span<const RadrayFence> fences) {
    for (auto&& fence : fences) {
        auto f = Underlying(fence);
        if (GetFenceState(fence) == RADRAY_FENCE_STATE_INCOMPLETE) {
            RADRAY_DX_FTHROW(f->fence->SetEventOnCompletion(f->fenceValue, f->waitEvent));
            WaitForSingleObject(f->waitEvent, INFINITE);
        }
    }
}

RadrayCommandAllocator Device::CreateCommandAllocator(RadrayCommandQueue queue) {
    auto q = Underlying(queue);
    auto a = RhiNew<CommandAllocator>(this, q->type);
    return {.Ptr = a, .Native = a->alloc.Get()};
}

void Device::DestroyCommandAllocator(RadrayCommandAllocator alloc) {
    auto a = Underlying(alloc);
    RhiDelete(a);
}

RadrayCommandList Device::CreateCommandList(RadrayCommandAllocator alloc) {
    auto a = Underlying(alloc);
    auto l = RhiNew<CommandList>(this, a->alloc.Get(), a->type);
    return {.Ptr = l, .Native = l->list.Get()};
}

void Device::DestroyCommandList(RadrayCommandList list) {
    auto l = Underlying(list);
    RhiDelete(l);
}

void Device::ResetCommandAllocator(RadrayCommandAllocator alloc) {
    auto a = Underlying(alloc);
    RADRAY_DX_FTHROW(a->alloc->Reset());
}

void Device::BeginCommandList(RadrayCommandList list) {
    auto l = Underlying(list);
    RADRAY_DX_FTHROW(l->list->Reset(l->alloc.Get(), nullptr));
}

void Device::EndCommandList(RadrayCommandList list) {
    auto l = Underlying(list);
    RADRAY_DX_FTHROW(l->list->Close());
}

RadrayRenderPassEncoder Device::BeginRenderPass(const RadrayRenderPassDescriptor& desc) {
    if (desc.ColorCount >= RADRAY_RHI_MAX_MRT) {
        RADRAY_DX_THROW("dx12 BeginRenderPass cannot set too many rt (ColorCount = {})", desc.ColorCount);
    }
    auto list = Underlying(desc.List);
    ComPtr<ID3D12GraphicsCommandList4> cmdList{};
    RADRAY_DX_FTHROW(list->list->QueryInterface(IID_PPV_ARGS(cmdList.GetAddressOf())));
    D3D12_RENDER_PASS_RENDER_TARGET_DESC rprt[RADRAY_RHI_MAX_MRT]{};
    D3D12_RENDER_PASS_DEPTH_STENCIL_DESC rpds{};
    const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* rpdsPtr = nullptr;
    for (uint32_t i = 0; i < desc.ColorCount; i++) {
        auto&& color = desc.Colors[i];
        auto&& desc = rprt[i];
        auto texView = Underlying(color.View);
        auto tex = texView->tex;
        D3D12_CLEAR_VALUE clrColor{};
        clrColor.Format = tex->desc.Format;
        clrColor.Color[0] = color.Clear.R;
        clrColor.Color[1] = color.Clear.G;
        clrColor.Color[2] = color.Clear.B;
        clrColor.Color[3] = color.Clear.A;
        desc.cpuDescriptor = texView->heap->HandleCpu(texView->index);
        desc.BeginningAccess = {.Type = EnumConvert(color.Load), .Clear = {clrColor}};
        desc.EndingAccess = {.Type = EnumConvert(color.Store), .Resolve = {}};
    }
    if (desc.DepthStencil != nullptr) {
        auto&& ds = *desc.DepthStencil;
        auto texView = Underlying(ds.View);
        auto tex = texView->tex;
        rpds.cpuDescriptor = texView->heap->HandleCpu(texView->index);
        D3D12_CLEAR_VALUE clrDepth{};
        clrDepth.Format = tex->desc.Format;
        clrDepth.DepthStencil.Depth = ds.DepthClear;
        rpds.DepthBeginningAccess = {.Type = EnumConvert(ds.DepthLoad), .Clear = {clrDepth}};
        rpds.DepthEndingAccess = {.Type = EnumConvert(ds.DepthStore)};
        D3D12_CLEAR_VALUE clrStencil{};
        clrStencil.Format = tex->desc.Format;
        clrStencil.DepthStencil.Stencil = ds.StencilClear;
        rpds.StencilBeginningAccess = {.Type = EnumConvert(ds.StencilLoad), .Clear = {clrStencil}};
        rpds.StencilEndingAccess = {.Type = EnumConvert(ds.StencilStore)};
        rpdsPtr = &rpds;
    }
    cmdList->BeginRenderPass(desc.ColorCount, rprt, rpdsPtr, D3D12_RENDER_PASS_FLAG_NONE);
    return RadrayRenderPassEncoder{.Ptr = desc.List.Ptr, .Native = desc.List.Native};
}

void Device::EndRenderPass(RadrayRenderPassEncoder encoder) {
    auto list = Underlying(encoder);
    ComPtr<ID3D12GraphicsCommandList4> cmdList{};
    RADRAY_DX_FTHROW(list->list->QueryInterface(IID_PPV_ARGS(cmdList.GetAddressOf())));
    cmdList->EndRenderPass();
}

void Device::ResourceBarriers(RadrayCommandList list, const RadrayResourceBarriersDescriptor& desc) {
    radray::vector<D3D12_RESOURCE_BARRIER> dx12Barriers;
    dx12Barriers.reserve(desc.TextureBarrierCount);
    for (uint32_t i = 0; i < desc.TextureBarrierCount; i++) {
        auto&& barrier = desc.TextureBarriers[i];
        auto tex = Underlying(barrier.Texture);
        if (barrier.SrcState == RADRAY_RESOURCE_STATE_UNORDERED_ACCESS &&
            barrier.DstState == RADRAY_RESOURCE_STATE_UNORDERED_ACCESS) {
            D3D12_RESOURCE_BARRIER& drb = dx12Barriers.emplace_back();
            drb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            drb.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            drb.UAV.pResource = tex->texture.Get();
            continue;
        }
        if (barrier.SrcState == barrier.DstState) {
            continue;
        }
        D3D12_RESOURCE_BARRIER& drb = dx12Barriers.emplace_back();
        drb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        drb.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        drb.Transition.pResource = tex->texture.Get();
        drb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;  // TODO: subresource
        drb.Transition.StateBefore = EnumConvert(barrier.SrcState);
        drb.Transition.StateAfter = EnumConvert(barrier.DstState);
    }
    auto cmdList = Underlying(list);
    cmdList->list->ResourceBarrier(static_cast<UINT>(dx12Barriers.size()), dx12Barriers.data());
}

RadraySwapChain Device::CreateSwapChain(const RadraySwapChainDescriptor& desc) {
    DXGI_SWAP_CHAIN_DESC1 chain{
        .Width = desc.Width,
        .Height = desc.Height,
        .Format = EnumConvert(desc.Format),
        .Stereo = FALSE,
        .SampleDesc = {.Count = 1, .Quality = 0},
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
        .BufferCount = desc.BackBufferCount,
        .Scaling = DXGI_SCALING_STRETCH,
        .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
        .AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED,
        .Flags = 0};
    auto q = Underlying(desc.PresentQueue);
    ID3D12CommandQueue* queue = q->queue.Get();
    HWND hwnd = reinterpret_cast<HWND>(desc.NativeWindow);
    auto s = RhiNew<SwapChain>(this, queue, hwnd, chain, desc.EnableSync);
    return {.Ptr = s, .Native = s->swapchain.Get()};
}

void Device::DestroySwapChian(RadraySwapChain swapchain) {
    auto s = Underlying(swapchain);
    RhiDelete(s);
}

RadrayTexture Device::AcquireNextRenderTarget(RadraySwapChain swapchain) {
    auto sc = Underlying(swapchain);
    UINT index = sc->swapchain->GetCurrentBackBufferIndex();
    auto&& color = sc->colors.at(index);
    return RadrayTexture{color.get(), color->texture.Get()};
}

void Device::Present(RadraySwapChain swapchain, RadrayTexture currentRt) {
    RADRAY_UNUSED(currentRt);
    auto sc = Underlying(swapchain);
    RADRAY_DX_FTHROW(sc->swapchain->Present(0, sc->presentFlags));
}

RadrayBuffer Device::CreateBuffer(const RadrayBufferDescriptor& desc) {
    RADRAY_DX_THROW("no impl");
}

void Device::DestroyBuffer(RadrayBuffer buffer) {
    RADRAY_DX_THROW("no impl");
}

RadrayBufferView Device::CreateBufferView(const RadrayBufferViewDescriptor& desc) {
    RADRAY_DX_THROW("no impl");
}

void Device::DestroyBufferView(RadrayBuffer buffer, RadrayBufferView view) {
    RADRAY_DX_THROW("no impl");
}

RadrayTexture Device::CreateTexture(const RadrayTextureDescriptor& desc) {
    RADRAY_DX_THROW("no impl");
}

void Device::DestroyTexture(RadrayTexture texture) {
    auto t = Underlying(texture);
    RhiDelete(t);
}

RadrayTextureView Device::CreateTextureView(const RadrayTextureViewDescriptor& desc) {
    auto texture = Underlying(desc.Texture);
    TextureView* view = nullptr;
    if (desc.Type == RADRAY_RESOURCE_TYPE_RENDER_TARGET) {
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = EnumConvert(desc.Format);
        switch (desc.Dimension) {
            case RADRAY_TEXTURE_DIM_1D: {
                rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1D;
                rtvDesc.Texture1D.MipSlice = desc.BaseMipLevel;
                break;
            }
            case RADRAY_TEXTURE_DIM_2D: {
                rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                rtvDesc.Texture2D.MipSlice = desc.BaseMipLevel;
                rtvDesc.Texture2D.PlaneSlice = 0;
                break;
            }
            case RADRAY_TEXTURE_DIM_3D: {
                rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
                rtvDesc.Texture3D.MipSlice = desc.BaseMipLevel;
                rtvDesc.Texture3D.FirstWSlice = desc.BaseArrayLayer;
                rtvDesc.Texture3D.WSize = desc.ArrayLayerCount;
                break;
            }
            case RADRAY_TEXTURE_DIM_1D_ARRAY: {
                rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
                rtvDesc.Texture1DArray.MipSlice = desc.BaseMipLevel;
                rtvDesc.Texture1DArray.FirstArraySlice = desc.BaseArrayLayer;
                rtvDesc.Texture1DArray.ArraySize = desc.ArrayLayerCount;
                break;
            }
            case RADRAY_TEXTURE_DIM_2D_ARRAY: {
                rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                rtvDesc.Texture2DArray.MipSlice = desc.BaseMipLevel;
                rtvDesc.Texture2DArray.PlaneSlice = 0;
                rtvDesc.Texture2DArray.FirstArraySlice = desc.BaseArrayLayer;
                rtvDesc.Texture2DArray.ArraySize = desc.ArrayLayerCount;
                break;
            }
            default: {
                RADRAY_DX_THROW("cannot create RTV for {}", desc.Dimension);
                break;
            }
        }
        view = RhiNew<TextureView>(rtvHeap.get(), texture, rtvDesc);
    } else if (desc.Type == RADRAY_RESOURCE_TYPE_DEPTH_STENCIL) {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = EnumConvert(desc.Format);
        switch (desc.Dimension) {
            case RADRAY_TEXTURE_DIM_1D: {
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1D;
                dsvDesc.Texture1D.MipSlice = desc.BaseMipLevel;
                break;
            }
            case RADRAY_TEXTURE_DIM_2D: {
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                dsvDesc.Texture2D.MipSlice = desc.BaseMipLevel;
                break;
            }
            case RADRAY_TEXTURE_DIM_1D_ARRAY: {
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1DARRAY;
                dsvDesc.Texture1DArray.MipSlice = desc.BaseMipLevel;
                dsvDesc.Texture1DArray.FirstArraySlice = desc.BaseArrayLayer;
                dsvDesc.Texture1DArray.ArraySize = desc.ArrayLayerCount;
                break;
            }
            case RADRAY_TEXTURE_DIM_2D_ARRAY: {
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                dsvDesc.Texture2DArray.MipSlice = desc.BaseMipLevel;
                dsvDesc.Texture2DArray.FirstArraySlice = desc.BaseArrayLayer;
                dsvDesc.Texture2DArray.ArraySize = desc.ArrayLayerCount;
                break;
            }
            default: {
                RADRAY_DX_THROW("cannot create DSV for {}", desc.Dimension);
                break;
            }
        }
        view = RhiNew<TextureView>(dsvHeap.get(), texture, dsvDesc);
    } else if (desc.Type == RADRAY_RESOURCE_TYPE_TEXTURE) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = EnumConvert(desc.Format);
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        switch (desc.Dimension) {
            case RADRAY_TEXTURE_DIM_1D: {
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
                srvDesc.Texture1D.MipLevels = desc.MipLevelCount;
                srvDesc.Texture1D.MostDetailedMip = desc.BaseMipLevel;
                break;
            }
            case RADRAY_TEXTURE_DIM_2D: {
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MipLevels = desc.MipLevelCount;
                srvDesc.Texture2D.MostDetailedMip = desc.BaseMipLevel;
                srvDesc.Texture2D.PlaneSlice = 0;
                break;
            }
            case RADRAY_TEXTURE_DIM_3D: {
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                srvDesc.Texture3D.MipLevels = desc.MipLevelCount;
                srvDesc.Texture3D.MostDetailedMip = desc.BaseMipLevel;
                break;
            }
            case RADRAY_TEXTURE_DIM_CUBE: {
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                srvDesc.TextureCube.MipLevels = desc.MipLevelCount;
                srvDesc.TextureCube.MostDetailedMip = desc.BaseMipLevel;
                break;
            }
            case RADRAY_TEXTURE_DIM_1D_ARRAY: {
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
                srvDesc.Texture1DArray.MipLevels = desc.MipLevelCount;
                srvDesc.Texture1DArray.MostDetailedMip = desc.BaseMipLevel;
                srvDesc.Texture1DArray.FirstArraySlice = desc.BaseArrayLayer;
                srvDesc.Texture1DArray.ArraySize = desc.ArrayLayerCount;
                break;
            }
            case RADRAY_TEXTURE_DIM_2D_ARRAY: {
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                srvDesc.Texture2DArray.MipLevels = desc.MipLevelCount;
                srvDesc.Texture2DArray.MostDetailedMip = desc.BaseMipLevel;
                srvDesc.Texture2DArray.PlaneSlice = 0;
                srvDesc.Texture2DArray.FirstArraySlice = desc.BaseArrayLayer;
                srvDesc.Texture2DArray.ArraySize = desc.ArrayLayerCount;
                break;
            }
            case RADRAY_TEXTURE_DIM_CUBE_ARRAY: {
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
                srvDesc.TextureCubeArray.MipLevels = desc.MipLevelCount;
                srvDesc.TextureCubeArray.MostDetailedMip = desc.BaseMipLevel;
                srvDesc.TextureCubeArray.NumCubes = desc.ArrayLayerCount;
                srvDesc.TextureCubeArray.First2DArrayFace = desc.BaseArrayLayer;
                break;
            }
            default: {
                RADRAY_DX_THROW("cannot create SRV for {}", desc.Dimension);
                break;
            }
        }
        view = RhiNew<TextureView>(cbvSrvUavHeap.get(), texture, srvDesc);
    } else if (desc.Type == RADRAY_RESOURCE_TYPE_TEXTURE_RW) {
        if (desc.MipLevelCount > 1) {
            RADRAY_DX_THROW("UAV cannot create multi mip slice");
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = EnumConvert(desc.Format);
        switch (desc.Dimension) {
            case RADRAY_TEXTURE_DIM_1D: {
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
                uavDesc.Texture1D.MipSlice = desc.BaseMipLevel;
                break;
            }
            case RADRAY_TEXTURE_DIM_2D: {
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                uavDesc.Texture2D.MipSlice = desc.BaseMipLevel;
                uavDesc.Texture2D.PlaneSlice = 0;
                break;
            }
            case RADRAY_TEXTURE_DIM_3D: {
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                uavDesc.Texture3D.MipSlice = desc.BaseMipLevel;
                uavDesc.Texture3D.FirstWSlice = desc.BaseArrayLayer;
                uavDesc.Texture3D.WSize = desc.ArrayLayerCount;
                break;
            }
            case RADRAY_TEXTURE_DIM_1D_ARRAY: {
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
                uavDesc.Texture1DArray.MipSlice = desc.BaseMipLevel;
                uavDesc.Texture1DArray.FirstArraySlice = desc.BaseArrayLayer;
                uavDesc.Texture1DArray.ArraySize = desc.ArrayLayerCount;
                break;
            }
            case RADRAY_TEXTURE_DIM_2D_ARRAY: {
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                uavDesc.Texture2DArray.MipSlice = desc.BaseMipLevel;
                uavDesc.Texture2DArray.PlaneSlice = 0;
                uavDesc.Texture2DArray.FirstArraySlice = desc.BaseArrayLayer;
                uavDesc.Texture2DArray.ArraySize = desc.ArrayLayerCount;
                break;
            }
            default: {
                RADRAY_DX_THROW("cannot create UAV for {}", desc.Dimension);
                break;
            }
        }
        view = RhiNew<TextureView>(cbvSrvUavHeap.get(), texture, uavDesc);
    } else {
        RADRAY_DX_THROW("cannot create texture view for {}", desc.Type);
    }
    return RadrayTextureView{view};
}

void Device::DestroyTextureView(RadrayTextureView view) {
    auto v = Underlying(view);
    RhiDelete(v);
}

RadrayShader Device::CompileShader(const RadrayCompileRasterizationShaderDescriptor& desc) {
    if (!shaderCompiler->IsValid() || !shaderCompiler->IsAvailable(RADRAY_SHADER_COMPILER_DXC)) {
        RADRAY_ERR_LOG("cannot compile shader {}", desc.Name);
        RADRAY_DX_THROW("radray shader compiler is invalid");
    }
    Stopwatch sw{};
    sw.Start();
    auto result = shaderCompiler->DxcHlslToDxil(desc);
    auto shader = RADRAY_RHI_EMPTY_RES(RadrayShader);
    if (auto err = std::get_if<radray::string>(&result)) {
        RADRAY_ERR_LOG("cannot compile shader {}", desc.Name);
        RADRAY_DX_THROW("{}", *err);
    } else if (auto bc = std::get_if<DxilWithReflection>(&result)) {
        auto dxil = bc->Dxil.GetView();
        auto refl = shaderCompiler->DxcCreateReflection(bc->Refl.GetView());
        if (auto err = std::get_if<radray::string>(&refl)) {
            RADRAY_ERR_LOG("cannot compile shader {}", desc.Name);
            RADRAY_DX_THROW("{}", *err);
        } else if (auto reflRes = std::get_if<ID3D12ShaderReflection*>(&refl)) {
            ComPtr<ID3D12ShaderReflection> comRefl{};
            comRefl.Attach(*reflRes);
            auto rs = RhiNew<RasterShader>();
            rs->code = radray::vector<uint8_t>{dxil.begin(), dxil.end()};
            rs->refl = std::move(comRefl);
            rs->stage = desc.Stage;
            shader = {rs, rs->refl.Get()};
        }
    }
    sw.Stop();
    RADRAY_INFO_LOG(
        "compile shader name={} stage={} entry={} ({}ms)",
        desc.Name,
        desc.Stage,
        desc.EntryPoint,
        sw.ElapsedMilliseconds());
    return shader;
}

void Device::DestroyShader(RadrayShader shader) {
    auto rs = Underlying(shader);
    RhiDelete(rs);
}

RadrayRootSignature Device::CreateRootSignature(const RadrayRootSignatureDescriptor& desc) {
    struct ShaderResource {
        radray::string Name;
        RadrayResourceType Type;
        RadrayTextureDimension Dim;
        RadrayShaderStages Stage;
        uint32_t BindPoint;
        uint32_t Space;
        uint32_t BindCount;
    };
    auto d3d12ResTypeToRadType = [](D3D_SHADER_INPUT_TYPE type) {
        switch (type) {
            case D3D_SIT_CBUFFER: return RADRAY_RESOURCE_TYPE_CBUFFER;
            case D3D_SIT_TBUFFER: return RADRAY_RESOURCE_TYPE_BUFFER;
            case D3D_SIT_TEXTURE: return RADRAY_RESOURCE_TYPE_TEXTURE;
            case D3D_SIT_SAMPLER: return RADRAY_RESOURCE_TYPE_SAMPLER;
            case D3D_SIT_UAV_RWTYPED: return RADRAY_RESOURCE_TYPE_TEXTURE_RW;
            case D3D_SIT_STRUCTURED: return RADRAY_RESOURCE_TYPE_BUFFER;
            case D3D_SIT_UAV_RWSTRUCTURED: return RADRAY_RESOURCE_TYPE_BUFFER_RW;
            case D3D_SIT_BYTEADDRESS: return RADRAY_RESOURCE_TYPE_BUFFER;
            case D3D_SIT_UAV_RWBYTEADDRESS: return RADRAY_RESOURCE_TYPE_BUFFER_RW;
            case D3D_SIT_UAV_APPEND_STRUCTURED: return RADRAY_RESOURCE_TYPE_BUFFER_RW;
            case D3D_SIT_UAV_CONSUME_STRUCTURED: return RADRAY_RESOURCE_TYPE_BUFFER_RW;
            case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER: return RADRAY_RESOURCE_TYPE_BUFFER_RW;
            case D3D_SIT_RTACCELERATIONSTRUCTURE: return RADRAY_RESOURCE_TYPE_RAYTRACING;
            case D3D_SIT_UAV_FEEDBACKTEXTURE: return RADRAY_RESOURCE_TYPE_TEXTURE_RW;
            default: return RADRAY_RESOURCE_TYPE_UNKNOWN;
        }
    };
    auto d3d12DimToRadDim = [](D3D_SRV_DIMENSION dim) {
        switch (dim) {
            case D3D_SRV_DIMENSION_UNKNOWN: return RADRAY_TEXTURE_DIM_UNKNOWN;
            case D3D_SRV_DIMENSION_BUFFER: return RADRAY_TEXTURE_DIM_UNKNOWN;
            case D3D_SRV_DIMENSION_TEXTURE1D: return RADRAY_TEXTURE_DIM_1D;
            case D3D_SRV_DIMENSION_TEXTURE1DARRAY: return RADRAY_TEXTURE_DIM_1D_ARRAY;
            case D3D_SRV_DIMENSION_TEXTURE2D: return RADRAY_TEXTURE_DIM_2D;
            case D3D_SRV_DIMENSION_TEXTURE2DARRAY: return RADRAY_TEXTURE_DIM_2D_ARRAY;
            case D3D_SRV_DIMENSION_TEXTURE2DMS: return RADRAY_TEXTURE_DIM_UNKNOWN;
            case D3D_SRV_DIMENSION_TEXTURE2DMSARRAY: return RADRAY_TEXTURE_DIM_UNKNOWN;
            case D3D_SRV_DIMENSION_TEXTURE3D: return RADRAY_TEXTURE_DIM_3D;
            case D3D_SRV_DIMENSION_TEXTURECUBE: return RADRAY_TEXTURE_DIM_CUBE;
            case D3D_SRV_DIMENSION_TEXTURECUBEARRAY: return RADRAY_TEXTURE_DIM_CUBE_ARRAY;
            case D3D_SRV_DIMENSION_BUFFEREX: return RADRAY_TEXTURE_DIM_UNKNOWN;
            default: return RADRAY_TEXTURE_DIM_UNKNOWN;
        }
    };
    auto isStaticSampler = [](const ShaderResource& res, const RadrayRootSignatureDescriptor& desc) {
        if (res.Type != RADRAY_RESOURCE_TYPE_SAMPLER) {
            return false;
        }
        for (size_t i = 0; i < desc.StaticSamplerCount; i++) {
            std::string_view t{desc.StaticSamplerNames[i]};
            if (res.Name == t) {
                return true;
            }
        }
        return false;
    };
    /**
     * vk 里叫 push constant 的在 D3D12 里应该对应根常量 (root constant)
     * 处理起来好麻烦啊, 暂时先全当需要绑定的 cbuffer 了
     * 暂时先全都用 descriptor table
     * 之后可能会改成先尝试用 descriptor, 超出空间限制再转 table ?
     *
     * 先把所有 bind resource 反射出来
     */
    radray::vector<ShaderResource> resources;
    radray::vector<ShaderResource> staticSamplers;
    for (size_t i = 0; i < desc.ShaderCount; i++) {
        auto shaderWrap = desc.Shaders[i];
        Shader* shader = Underlying(shaderWrap);
        ID3D12ShaderReflection* refl = shader->refl.Get();
        D3D12_SHADER_DESC shaderDesc;
        refl->GetDesc(&shaderDesc);
        for (size_t j = 0; j < shaderDesc.BoundResources; j++) {
            D3D12_SHADER_INPUT_BIND_DESC bindDesc;
            refl->GetResourceBindingDesc(j, &bindDesc);
            radray::string radName{bindDesc.Name};
            ShaderResource radRes{
                radName,
                d3d12ResTypeToRadType(bindDesc.Type),
                d3d12DimToRadDim(bindDesc.Dimension),
                static_cast<uint32_t>(shader->stage),
                bindDesc.BindPoint,
                bindDesc.Space,
                bindDesc.BindCount};
            if (bindDesc.Type == D3D_SHADER_INPUT_TYPE::D3D_SIT_UAV_RWTYPED && bindDesc.Dimension == D3D_SRV_DIMENSION_BUFFER) {
                radRes.Type = RADRAY_RESOURCE_TYPE_BUFFER_RW;
            }
            if (bindDesc.Type == D3D_SHADER_INPUT_TYPE::D3D_SIT_TEXTURE && bindDesc.Dimension == D3D_SRV_DIMENSION_BUFFER) {
                radRes.Type = RADRAY_RESOURCE_TYPE_BUFFER;
            }
            if (isStaticSampler(radRes, desc)) {
                bool isConflit = false;
                for (auto&& i : staticSamplers) {
                    if (i.BindPoint == radRes.BindPoint && i.Space == radRes.Space) {
                        i.Stage |= radRes.Stage;
                        isConflit = true;
                    }
                }
                if (!isConflit) {
                    staticSamplers.emplace_back(radRes);
                }
            } else {
                resources.emplace_back(radRes);
            }
        }
    }
    /**
     * 合并不同 stage 所需的相同资源, 也就是 Space 和 Bind 一致的资源
     * 有没有可能出现, 阶段 a 的资源数组占有的绑定点覆盖了阶段 b 的资源绑定点 ?
     * 例如:
     * vertex stage: Texture2D arr[6] : register(t0);
     * pixel stage : Texture2D v : register(t2);
     * 这个时候给t0s0绑一个长度6, t2绑长度1会发生什么事
     */
    radray::vector<ShaderResource> merge;
    radray::set<uint32_t> spaces;
    merge.reserve(resources.size());
    for (auto&& res : resources) {
        bool isConflit = false;
        for (auto&& i : merge) {
            if (i.Space == res.Space && i.BindPoint == res.BindPoint && i.Type == res.Type) {
                i.Stage |= res.Stage;
                isConflit = true;
            }
        }
        if (!isConflit) {
            spaces.insert(res.Space);
            merge.emplace_back(res);
        }
    }
    std::sort(merge.begin(), merge.end(), [](const auto& lhs, const auto& rhs) noexcept {
        if (lhs.Space == rhs.Space) {
            return lhs.BindPoint < rhs.BindPoint;
        } else {
            return lhs.Space < rhs.Space;
        }
    });
    /**
     * 根据 Space 划分 table
     * //TODO:
     */

    return RadrayRootSignature{};
}

void Device::DestroyRootSignature(RadrayRootSignature shader) {
}

RadrayGraphicsPipeline Device::CreateGraphicsPipeline(const RadrayGraphicsPipelineDescriptor& desc) {
    RADRAY_DX_THROW("no impl");
}

void Device::DestroyGraphicsPipeline(RadrayGraphicsPipeline pipe) {
    RADRAY_DX_THROW("no impl");
}

}  // namespace radray::rhi::d3d12
