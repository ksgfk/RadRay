#include <radray/render/backend/metal_impl.h>
#include <radray/render/backend/metal_impl_cpp.h>

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Cocoa/Cocoa.h>

#include <radray/logger.h>
#include <radray/utility.h>
#include <radray/render/backend/metal_helper.h>

namespace radray::render::metal {

// Metal shares buffer binding space between vertex buffers and shader buffers.
// Bind vertex buffers at high indices (30, 29, ...) to avoid conflicts with
// shader buffer bindings (push constants, descriptor sets) at low indices.
static constexpr uint32_t kMtlVertexBufferBase = 30;

#if defined(RADRAY_PLATFORM_MACOS)
static uint32_t GetIORegistryEntryProperty(io_registry_entry_t entry, CFStringRef propertyName) {
    uint32_t value = 0;
    CFTypeRef cfProp = IORegistryEntrySearchCFProperty(
        entry, kIOServicePlane, propertyName, kCFAllocatorDefault,
        kIORegistryIterateRecursively | kIORegistryIterateParents);
    if (cfProp) {
        if (CFGetTypeID(cfProp) == CFDataGetTypeID()) {
            const uint32_t* pValue = reinterpret_cast<const uint32_t*>(CFDataGetBytePtr(static_cast<CFDataRef>(cfProp)));
            if (pValue) {
                value = *pValue;
            }
        }
        CFRelease(cfProp);
    }
    return value;
}

static void QueryVendorAndDeviceId(id<MTLDevice> device, uint32_t* outVendorId, uint32_t* outDeviceId) {
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    bool isFound = false;

    // Apple Silicon: no PCI device-id, synthesize from GPU family
    if ([device supportsFamily:MTLGPUFamilyApple5]) {
        vendorId = 0x106b;  // Apple
        if ([device supportsFamily:MTLGPUFamilyApple7]) {
            deviceId = 0xa140;
        } else if ([device supportsFamily:MTLGPUFamilyApple6]) {
            deviceId = 0xa130;
        } else {
            deviceId = 0xa120;
        }
    }

    // Try registryID to find IOKit node
    uint64_t regID = 0;
    if ([device respondsToSelector:@selector(registryID)]) {
        regID = device.registryID;
    }
    if (regID != 0) {
        io_registry_entry_t entry = IOServiceGetMatchingService(
            kIOMainPortDefault, IORegistryEntryIDMatching(regID));
        if (entry) {
            io_registry_entry_t parent;
            if (IORegistryEntryGetParentEntry(entry, kIOServicePlane, &parent) == kIOReturnSuccess) {
                isFound = true;
                vendorId = GetIORegistryEntryProperty(parent, CFSTR("vendor-id"));
                deviceId = GetIORegistryEntryProperty(parent, CFSTR("device-id"));
                IOObjectRelease(parent);
            }
            IOObjectRelease(entry);
        }
    }

    // Fallback: iterate IOPCIDevice entries
    if (!isFound) {
        io_iterator_t entryIterator;
        if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                         IOServiceMatching("IOPCIDevice"), &entryIterator) == kIOReturnSuccess) {
            io_registry_entry_t entry;
            bool isUMA = device.hasUnifiedMemory;
            while (!isFound && (entry = IOIteratorNext(entryIterator))) {
                if (GetIORegistryEntryProperty(entry, CFSTR("class-code")) == 0x30000) {
                    uint32_t vid = GetIORegistryEntryProperty(entry, CFSTR("vendor-id"));
                    bool isIntegrated = (vid == 0x8086);  // Intel
                    if (isIntegrated == isUMA) {
                        isFound = true;
                        vendorId = vid;
                        deviceId = GetIORegistryEntryProperty(entry, CFSTR("device-id"));
                    }
                }
                IOObjectRelease(entry);
            }
            IOObjectRelease(entryIterator);
        }
    }

    *outVendorId = vendorId;
    *outDeviceId = deviceId;
}
#endif

DeviceMetal::DeviceMetal(id<MTLDevice> device) noexcept : _device(device) {}

DeviceMetal::~DeviceMetal() noexcept { DestroyImpl(); }

bool DeviceMetal::IsValid() const noexcept { return _device != nil; }

void DeviceMetal::Destroy() noexcept { DestroyImpl(); }

void DeviceMetal::DestroyImpl() noexcept {
    for (auto& queueArr : _queues) {
        queueArr.clear();
    }
    _device = nil;
}

DeviceDetail DeviceMetal::GetDetail() const noexcept { return _detail; }

Nullable<CommandQueue*> DeviceMetal::GetCommandQueue(QueueType type, uint32_t slot) noexcept {
    @autoreleasepool {
        uint32_t index = static_cast<uint32_t>(type);
        if (index >= (uint32_t)QueueType::MAX_COUNT) {
            RADRAY_ERR_LOG("metal invalid queue type {}", index);
            return nullptr;
        }
        auto& queues = _queues[index];
        if (queues.size() <= slot) {
            queues.reserve(slot + 1);
            for (size_t i = queues.size(); i <= slot; i++) {
                queues.emplace_back(unique_ptr<CmdQueueMetal>{nullptr});
            }
        }
        auto& q = queues[slot];
        if (q == nullptr) {
            id<MTLCommandQueue> mtlQueue = [_device newCommandQueueWithMaxCommandBufferCount:512];
            if (mtlQueue == nil) {
                RADRAY_ERR_LOG("metal cannot create command queue");
                return nullptr;
            }
            q = make_unique<CmdQueueMetal>(this, mtlQueue);
        }
        return q.get();
    }
}

Nullable<unique_ptr<CommandBuffer>> DeviceMetal::CreateCommandBuffer(CommandQueue* queue) noexcept {
    @autoreleasepool {
        auto mtlQueue = CastMtlObject(queue);
        return make_unique<CmdBufferMetal>(this, mtlQueue);
    }
}

Nullable<unique_ptr<Fence>> DeviceMetal::CreateFence() noexcept {
    @autoreleasepool {
        id<MTLSharedEvent> event = [_device newSharedEvent];
        if (event == nil) {
            RADRAY_ERR_LOG("metal cannot create shared event for fence");
            return nullptr;
        }
        return make_unique<FenceMetal>(this, event, 1);
    }
}

Nullable<unique_ptr<Semaphore>> DeviceMetal::CreateSemaphoreDevice() noexcept {
    @autoreleasepool {
        id<MTLEvent> event = [_device newEvent];
        if (event == nil) {
            RADRAY_ERR_LOG("metal cannot create event for semaphore");
            return nullptr;
        }
        return make_unique<SemaphoreMetal>(this, event, 1);
    }
}

Nullable<unique_ptr<SwapChain>> DeviceMetal::CreateSwapChain(const SwapChainDescriptor& desc) noexcept {
    @autoreleasepool {
        NSView* nsView = (__bridge NSView*)desc.NativeHandler;
        if (nsView == nil) {
            RADRAY_ERR_LOG("metal swap chain requires a valid NSView");
            return nullptr;
        }
        CAMetalLayer* layer = (CAMetalLayer*)[nsView layer];
        if (layer == nil || ![layer isKindOfClass:[CAMetalLayer class]]) {
            layer = [CAMetalLayer layer];
            [nsView setLayer:layer];
            [nsView setWantsLayer:YES];
        }
        layer.device = _device;
        layer.pixelFormat = MapPixelFormat(desc.Format);
        layer.drawableSize = CGSizeMake(desc.Width, desc.Height);
        layer.maximumDrawableCount = desc.BackBufferCount;
        switch (desc.PresentMode) {
            case PresentMode::FIFO: layer.displaySyncEnabled = YES; break;
            case PresentMode::Immediate: layer.displaySyncEnabled = NO; break;
            case PresentMode::Mailbox: layer.displaySyncEnabled = YES; break;
        }
        auto presentQueue = CastMtlObject(desc.PresentQueue);
        return make_unique<SwapChainMetal>(this, presentQueue, layer, desc.BackBufferCount, desc.Format);
    }
}

Nullable<unique_ptr<Buffer>> DeviceMetal::CreateBuffer(const BufferDescriptor& desc_) noexcept {
    BufferDescriptor desc = desc_;
    if (desc.Usage.HasFlag(BufferUse::CBuffer)) {
        desc.Size = Align(desc.Size, _detail.CBufferAlignment);
    }
    @autoreleasepool {
        MemoryType type;
        MTLResourceOptions options = MapResourceOptions(desc.Memory);
        id<MTLBuffer> mtlBuf = [_device newBufferWithLength:desc.Size options:options];
        if (mtlBuf == nil) {
            RADRAY_ERR_LOG("metal cannot create buffer (size={})", desc.Size);
            return nullptr;
        }
        if (desc.Name.size() > 0) {
            mtlBuf.label = [[NSString alloc] initWithBytes:desc.Name.data()
                                                    length:desc.Name.size()
                                                  encoding:NSUTF8StringEncoding];
        }
        auto buf = make_unique<BufferMetal>(this, mtlBuf);
        if (desc.Usage.HasFlag(BufferUse::CBuffer)) {
            buf->_mappedPtr = mtlBuf.contents;
        }
        buf->_name = desc.Name;
        buf->_desc = desc;
        buf->_desc.Name = buf->_name;
        return buf;
    }
}

Nullable<unique_ptr<Texture>> DeviceMetal::CreateTexture(const TextureDescriptor& desc) noexcept {
    @autoreleasepool {
        MTLTextureDescriptor* td = [[MTLTextureDescriptor alloc] init];
        td.textureType = MapTextureType(desc.Dim, desc.SampleCount);
        td.pixelFormat = MapPixelFormat(desc.Format);
        td.width = desc.Width;
        td.height = desc.Height;
        td.depth = (desc.Dim == TextureDimension::Dim3D) ? desc.DepthOrArraySize : 1;
        td.arrayLength = (desc.Dim != TextureDimension::Dim3D && desc.DepthOrArraySize > 0) ? desc.DepthOrArraySize : 1;
        td.mipmapLevelCount = (desc.MipLevels > 0) ? desc.MipLevels : 1;
        td.sampleCount = (desc.SampleCount > 0) ? desc.SampleCount : 1;
        td.storageMode = MapStorageMode(desc.Memory);
        MTLTextureUsage usage = MTLTextureUsageUnknown;
        if (desc.Usage.HasFlag(TextureUse::Resource)) usage |= MTLTextureUsageShaderRead;
        if (desc.Usage.HasFlag(TextureUse::UnorderedAccess)) usage |= MTLTextureUsageShaderWrite;
        if (desc.Usage.HasFlag(TextureUse::RenderTarget) ||
            desc.Usage.HasFlag(TextureUse::DepthStencilWrite) ||
            desc.Usage.HasFlag(TextureUse::DepthStencilRead)) {
            usage |= MTLTextureUsageRenderTarget;
        }
        td.usage = usage;
        id<MTLTexture> mtlTex = [_device newTextureWithDescriptor:td];
        if (mtlTex == nil) {
            RADRAY_ERR_LOG("metal cannot create texture ({}x{})", desc.Width, desc.Height);
            return nullptr;
        }
        if (desc.Name.size() > 0) {
            mtlTex.label = [[NSString alloc] initWithBytes:desc.Name.data()
                                                    length:desc.Name.size()
                                                  encoding:NSUTF8StringEncoding];
        }
        auto tex = make_unique<TextureMetal>();
        tex->_device = this;
        tex->_texture = mtlTex;
        tex->_desc = desc;
        return tex;
    }
}

Nullable<unique_ptr<BufferView>> DeviceMetal::CreateBufferView(const BufferViewDescriptor& desc) noexcept {
    @autoreleasepool {
        auto* mtlBuf = CastMtlObject(desc.Target);
        auto view = make_unique<BufferViewMetal>(this, mtlBuf);
        view->_desc = desc;
        return view;
    }
}

Nullable<unique_ptr<TextureView>> DeviceMetal::CreateTextureView(const TextureViewDescriptor& desc) noexcept {
    @autoreleasepool {
        auto* mtlTex = CastMtlObject(desc.Target);
        // frameBufferOnly textures (e.g. CAMetalLayer drawables) cannot create views;
        // use the original texture directly as the "view"
        if (mtlTex->_texture.isFramebufferOnly) {
            auto view = make_unique<TextureViewMetal>();
            view->_device = this;
            view->_texture = mtlTex;
            view->_textureView = mtlTex->_texture;
            view->_desc = desc;
            return view;
        }
        MTLPixelFormat fmt = MapPixelFormat(desc.Format);
        MTLTextureType texType = MapTextureViewType(desc.Dim);
        NSRange levelRange = NSMakeRange(desc.Range.BaseMipLevel,
                                         desc.Range.MipLevelCount == SubresourceRange::All ? mtlTex->_texture.mipmapLevelCount - desc.Range.BaseMipLevel : desc.Range.MipLevelCount);
        NSRange sliceRange = NSMakeRange(desc.Range.BaseArrayLayer,
                                         desc.Range.ArrayLayerCount == SubresourceRange::All ? mtlTex->_texture.arrayLength - desc.Range.BaseArrayLayer : desc.Range.ArrayLayerCount);
        id<MTLTexture> texView = [mtlTex->_texture newTextureViewWithPixelFormat:fmt
                                                                     textureType:texType
                                                                          levels:levelRange
                                                                          slices:sliceRange];
        if (texView == nil) {
            RADRAY_ERR_LOG("metal cannot create texture view");
            return nullptr;
        }
        auto view = make_unique<TextureViewMetal>();
        view->_device = this;
        view->_texture = mtlTex;
        view->_textureView = texView;
        view->_desc = desc;
        return view;
    }
}

Nullable<unique_ptr<Shader>> DeviceMetal::CreateShader(const ShaderDescriptor& desc) noexcept {
    @autoreleasepool {
        NSError* error = nil;
        id<MTLLibrary> library = nil;
        if (desc.Category == ShaderBlobCategory::MSL) {
            NSString* source = [[NSString alloc] initWithBytes:desc.Source.data()
                                                        length:desc.Source.size()
                                                      encoding:NSUTF8StringEncoding];
            library = [_device newLibraryWithSource:source options:nil error:&error];
        } else if (desc.Category == ShaderBlobCategory::METALLIB) {
            dispatch_data_t data = dispatch_data_create(desc.Source.data(), desc.Source.size(),
                                                        dispatch_get_main_queue(), DISPATCH_DATA_DESTRUCTOR_DEFAULT);
            library = [_device newLibraryWithData:data error:&error];
        } else {
            RADRAY_ERR_LOG("metal unsupported shader blob category {}", desc.Category);
            return nullptr;
        }
        if (library == nil) {
            if (error != nil) {
                RADRAY_ERR_LOG("metal cannot create shader: {}", [error.localizedDescription UTF8String]);
            } else {
                RADRAY_ERR_LOG("metal cannot create shader");
            }
            return nullptr;
        }
        auto shader = make_unique<ShaderMetal>();
        shader->_device = this;
        shader->_library = library;
        return shader;
    }
}

Nullable<unique_ptr<RootSignature>> DeviceMetal::CreateRootSignature(const RootSignatureDescriptor& desc) noexcept {
    @autoreleasepool {
        auto rootSig = make_unique<RootSignatureMetal>();
        rootSig->_device = this;
        auto& c = rootSig->_container;
        c._rootDescriptors.assign(desc.RootDescriptors.begin(), desc.RootDescriptors.end());
        c._staticSamplers.assign(desc.StaticSamplers.begin(), desc.StaticSamplers.end());
        c._desc.Constant = desc.Constant;
        size_t totalElements = 0;
        size_t totalBindless = 0;
        for (auto& setDesc : desc.DescriptorSets) {
            totalElements += setDesc.Elements.size();
            totalBindless += setDesc.BindlessDescriptors.size();
        }
        c._elements.reserve(totalElements);
        c._bindlessDescriptors.reserve(totalBindless);
        c._descriptorSets.reserve(desc.DescriptorSets.size());
        for (auto& setDesc : desc.DescriptorSets) {
            size_t elemStart = c._elements.size();
            size_t bindlessStart = c._bindlessDescriptors.size();
            c._elements.insert(c._elements.end(), setDesc.Elements.begin(), setDesc.Elements.end());
            c._bindlessDescriptors.insert(c._bindlessDescriptors.end(), setDesc.BindlessDescriptors.begin(), setDesc.BindlessDescriptors.end());
            RootSignatureDescriptorSet ds{};
            ds.Elements = std::span<const RootSignatureSetElement>{
                c._elements.data() + elemStart,
                setDesc.Elements.size()};
            ds.BindlessDescriptors = std::span<const RootSignatureBindlessDescriptor>{
                c._bindlessDescriptors.data() + bindlessStart,
                setDesc.BindlessDescriptors.size()};
            c._descriptorSets.push_back(ds);
        }
        c._desc.RootDescriptors = c._rootDescriptors;
        c._desc.DescriptorSets = c._descriptorSets;
        c._desc.StaticSamplers = c._staticSamplers;
        rootSig->_cachedStaticSamplers.reserve(c._staticSamplers.size());
        for (auto& ss : c._staticSamplers) {
            MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
            sd.sAddressMode = MapAddressMode(ss.Desc.AddressS);
            sd.tAddressMode = MapAddressMode(ss.Desc.AddressT);
            sd.rAddressMode = MapAddressMode(ss.Desc.AddressR);
            sd.minFilter = MapMinMagFilter(ss.Desc.MinFilter);
            sd.magFilter = MapMinMagFilter(ss.Desc.MagFilter);
            sd.mipFilter = MapMipFilter(ss.Desc.MipmapFilter);
            sd.lodMinClamp = ss.Desc.LodMin;
            sd.lodMaxClamp = ss.Desc.LodMax;
            if (ss.Desc.Compare.has_value()) {
                sd.compareFunction = MapCompareFunction(ss.Desc.Compare.value());
            }
            sd.maxAnisotropy = ss.Desc.AnisotropyClamp > 0 ? ss.Desc.AnisotropyClamp : 1;
            id<MTLSamplerState> sampler = [_device newSamplerStateWithDescriptor:sd];
            CachedStaticSampler css;
            css.sampler = sampler;
            css.Slot = ss.Slot;
            css.SetIndex = ss.SetIndex;
            css.Stages = ss.Stages;
            rootSig->_cachedStaticSamplers.push_back(css);
        }
        return rootSig;
    }
}

Nullable<unique_ptr<GraphicsPipelineState>> DeviceMetal::CreateGraphicsPipelineState(const GraphicsPipelineStateDescriptor& desc) noexcept {
    @autoreleasepool {
        MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor alloc] init];
        auto* rootSig = CastMtlObject(desc.RootSig);

        // vertex function
        if (desc.VS.has_value()) {
            auto* shader = CastMtlObject(desc.VS->Target);
            NSString* entry = [[NSString alloc] initWithBytes:desc.VS->EntryPoint.data()
                                                       length:desc.VS->EntryPoint.size()
                                                     encoding:NSUTF8StringEncoding];
            pd.vertexFunction = [shader->_library newFunctionWithName:entry];
        }
        // fragment function
        if (desc.PS.has_value()) {
            auto* shader = CastMtlObject(desc.PS->Target);
            NSString* entry = [[NSString alloc] initWithBytes:desc.PS->EntryPoint.data()
                                                       length:desc.PS->EntryPoint.size()
                                                     encoding:NSUTF8StringEncoding];
            pd.fragmentFunction = [shader->_library newFunctionWithName:entry];
        }

        // vertex descriptor
        if (!desc.VertexLayouts.empty()) {
            MTLVertexDescriptor* vd = [[MTLVertexDescriptor alloc] init];
            for (uint32_t i = 0; i < desc.VertexLayouts.size(); i++) {
                auto& layout = desc.VertexLayouts[i];
                uint32_t mtlBufIdx = kMtlVertexBufferBase - i;
                vd.layouts[mtlBufIdx].stride = layout.ArrayStride;
                vd.layouts[mtlBufIdx].stepFunction = MapVertexStepFunction(layout.StepMode);
                vd.layouts[mtlBufIdx].stepRate = 1;
                for (auto& elem : layout.Elements) {
                    vd.attributes[elem.Location].format = MapVertexFormat(elem.Format);
                    vd.attributes[elem.Location].offset = elem.Offset;
                    vd.attributes[elem.Location].bufferIndex = mtlBufIdx;
                }
            }
            pd.vertexDescriptor = vd;
        }

        // color attachments
        for (uint32_t i = 0; i < desc.ColorTargets.size(); i++) {
            auto& ct = desc.ColorTargets[i];
            pd.colorAttachments[i].pixelFormat = MapPixelFormat(ct.Format);
            pd.colorAttachments[i].writeMask = MapColorWriteMask(ct.WriteMask);
            if (ct.Blend.has_value()) {
                pd.colorAttachments[i].blendingEnabled = YES;
                pd.colorAttachments[i].sourceRGBBlendFactor = MapBlendFactor(ct.Blend->Color.Src);
                pd.colorAttachments[i].destinationRGBBlendFactor = MapBlendFactor(ct.Blend->Color.Dst);
                pd.colorAttachments[i].rgbBlendOperation = MapBlendOperation(ct.Blend->Color.Op);
                pd.colorAttachments[i].sourceAlphaBlendFactor = MapBlendFactor(ct.Blend->Alpha.Src);
                pd.colorAttachments[i].destinationAlphaBlendFactor = MapBlendFactor(ct.Blend->Alpha.Dst);
                pd.colorAttachments[i].alphaBlendOperation = MapBlendOperation(ct.Blend->Alpha.Op);
            }
        }

        // depth stencil
        id<MTLDepthStencilState> dsState = nil;
        if (desc.DepthStencil.has_value()) {
            auto& ds = desc.DepthStencil.value();
            pd.depthAttachmentPixelFormat = MapPixelFormat(ds.Format);
            if (ds.Stencil.has_value()) {
                pd.stencilAttachmentPixelFormat = MapPixelFormat(ds.Format);
            }
            MTLDepthStencilDescriptor* dsd = [[MTLDepthStencilDescriptor alloc] init];
            dsd.depthCompareFunction = MapCompareFunction(ds.DepthCompare);
            dsd.depthWriteEnabled = ds.DepthWriteEnable;
            if (ds.Stencil.has_value()) {
                auto& st = ds.Stencil.value();
                MTLStencilDescriptor* front = [[MTLStencilDescriptor alloc] init];
                front.stencilCompareFunction = MapCompareFunction(st.Front.Compare);
                front.stencilFailureOperation = MapStencilOp(st.Front.FailOp);
                front.depthFailureOperation = MapStencilOp(st.Front.DepthFailOp);
                front.depthStencilPassOperation = MapStencilOp(st.Front.PassOp);
                front.readMask = st.ReadMask;
                front.writeMask = st.WriteMask;
                dsd.frontFaceStencil = front;
                MTLStencilDescriptor* back = [[MTLStencilDescriptor alloc] init];
                back.stencilCompareFunction = MapCompareFunction(st.Back.Compare);
                back.stencilFailureOperation = MapStencilOp(st.Back.FailOp);
                back.depthFailureOperation = MapStencilOp(st.Back.DepthFailOp);
                back.depthStencilPassOperation = MapStencilOp(st.Back.PassOp);
                back.readMask = st.ReadMask;
                back.writeMask = st.WriteMask;
                dsd.backFaceStencil = back;
            }
            dsState = [_device newDepthStencilStateWithDescriptor:dsd];
        }

        // multisample
        pd.rasterSampleCount = desc.MultiSample.Count > 0 ? desc.MultiSample.Count : 1;
        pd.alphaToCoverageEnabled = desc.MultiSample.AlphaToCoverageEnable;

        NSError* error = nil;
        id<MTLRenderPipelineState> pso = [_device newRenderPipelineStateWithDescriptor:pd error:&error];
        if (pso == nil) {
            if (error != nil) {
                RADRAY_ERR_LOG("metal cannot create graphics pipeline: {}", [error.localizedDescription UTF8String]);
            }
            return nullptr;
        }

        auto result = make_unique<GraphicsPipelineStateMetal>();
        result->_device = this;
        result->_pipelineState = pso;
        result->_depthStencilState = dsState;
        result->_primitiveType = MapPrimitiveType(desc.Primitive.Topology);
        result->_cullMode = MapCullMode(desc.Primitive.Cull);
        result->_winding = MapWinding(desc.Primitive.FaceClockwise);
        result->_fillMode = (desc.Primitive.Poly == PolygonMode::Fill) ? MTLTriangleFillModeFill : MTLTriangleFillModeLines;
        if (desc.DepthStencil.has_value()) {
            result->_depthBiasConstant = static_cast<float>(desc.DepthStencil->DepthBias.Constant);
            result->_depthBiasSlopeScale = desc.DepthStencil->DepthBias.SlopScale;
            result->_depthBiasClamp = desc.DepthStencil->DepthBias.Clamp;
        }
        return result;
    }
}

Nullable<unique_ptr<ComputePipelineState>> DeviceMetal::CreateComputePipelineState(const ComputePipelineStateDescriptor& desc) noexcept {
    @autoreleasepool {
        auto* shader = CastMtlObject(desc.CS.Target);
        NSString* entry = [[NSString alloc] initWithBytes:desc.CS.EntryPoint.data()
                                                   length:desc.CS.EntryPoint.size()
                                                 encoding:NSUTF8StringEncoding];
        id<MTLFunction> func = [shader->_library newFunctionWithName:entry];
        if (func == nil) {
            RADRAY_ERR_LOG("metal cannot find compute function '{}'", desc.CS.EntryPoint);
            return nullptr;
        }
        NSError* error = nil;
        id<MTLComputePipelineState> pso = [_device newComputePipelineStateWithFunction:func error:&error];
        if (pso == nil) {
            if (error != nil) {
                RADRAY_ERR_LOG("metal cannot create compute pipeline: {}", [error.localizedDescription UTF8String]);
            }
            return nullptr;
        }
        auto result = make_unique<ComputePipelineStateMetal>();
        result->_device = this;
        result->_pipelineState = pso;
        return result;
    }
}

Nullable<unique_ptr<DescriptorSet>> DeviceMetal::CreateDescriptorSet(RootSignature* rootSig, uint32_t index) noexcept {
    @autoreleasepool {
        auto* mtlRootSig = CastMtlObject(rootSig);
        if (index >= mtlRootSig->_container._descriptorSets.size()) {
            RADRAY_ERR_LOG("metal descriptor set index {} out of range", index);
            return nullptr;
        }
        auto& layout = mtlRootSig->_container._descriptorSets[index];
        NSMutableArray<MTLArgumentDescriptor*>* argDescs = [[NSMutableArray alloc] init];
        ShaderStages combinedStages{ShaderStage::UNKNOWN};
        for (auto& elem : layout.Elements) {
            auto info = MapResourceBindTypeToArgument(elem.Type);
            uint32_t count = std::max(elem.Count, 1u);
            for (uint32_t i = 0; i < count; i++) {
                MTLArgumentDescriptor* argDesc = [[MTLArgumentDescriptor alloc] init];
                argDesc.dataType = info.dataType;
                argDesc.access = info.access;
                argDesc.index = elem.Slot + i;
                if (info.dataType == MTLDataTypeTexture) {
                    argDesc.textureType = MTLTextureType2D;
                }
                [argDescs addObject:argDesc];
            }
            combinedStages = combinedStages | elem.Stages;
        }
        // Include static samplers belonging to this descriptor set
        for (auto& ss : mtlRootSig->_cachedStaticSamplers) {
            if (ss.SetIndex == index) {
                MTLArgumentDescriptor* argDesc = [[MTLArgumentDescriptor alloc] init];
                argDesc.dataType = MTLDataTypeSampler;
                argDesc.access = MTLBindingAccessReadOnly;
                argDesc.index = ss.Slot;
                [argDescs addObject:argDesc];
                combinedStages = combinedStages | ss.Stages;
            }
        }
        if (argDescs.count == 0) {
            RADRAY_ERR_LOG("metal descriptor set {} has no elements", index);
            return nullptr;
        }
        id<MTLArgumentEncoder> encoder = [_device newArgumentEncoderWithArguments:argDescs];
        if (encoder == nil) {
            RADRAY_ERR_LOG("metal cannot create argument encoder for set {}", index);
            return nullptr;
        }
        id<MTLBuffer> argBuffer = [_device newBufferWithLength:encoder.encodedLength
                                                       options:MTLResourceStorageModeShared];
        if (argBuffer == nil) {
            RADRAY_ERR_LOG("metal cannot create argument buffer for set {}", index);
            return nullptr;
        }
        [encoder setArgumentBuffer:argBuffer offset:0];
        // Encode static samplers into the argument buffer
        for (auto& ss : mtlRootSig->_cachedStaticSamplers) {
            if (ss.SetIndex == index) {
                [encoder setSamplerState:ss.sampler atIndex:ss.Slot];
            }
        }
        auto ds = make_unique<DescriptorSetMetal>();
        ds->_device = this;
        ds->_rootSig = mtlRootSig;
        ds->_setIndex = index;
        ds->_argumentEncoder = encoder;
        ds->_argumentBuffer = argBuffer;
        ds->_combinedStages = combinedStages;
        return ds;
    }
}

Nullable<unique_ptr<Sampler>> DeviceMetal::CreateSampler(const SamplerDescriptor& desc) noexcept {
    @autoreleasepool {
        MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
        sd.sAddressMode = MapAddressMode(desc.AddressS);
        sd.tAddressMode = MapAddressMode(desc.AddressT);
        sd.rAddressMode = MapAddressMode(desc.AddressR);
        sd.minFilter = MapMinMagFilter(desc.MinFilter);
        sd.magFilter = MapMinMagFilter(desc.MagFilter);
        sd.mipFilter = MapMipFilter(desc.MipmapFilter);
        sd.lodMinClamp = desc.LodMin;
        sd.lodMaxClamp = desc.LodMax;
        if (desc.Compare.has_value()) {
            sd.compareFunction = MapCompareFunction(desc.Compare.value());
        }
        sd.maxAnisotropy = desc.AnisotropyClamp > 0 ? desc.AnisotropyClamp : 1;
        id<MTLSamplerState> sampler = [_device newSamplerStateWithDescriptor:sd];
        if (sampler == nil) {
            RADRAY_ERR_LOG("metal cannot create sampler");
            return nullptr;
        }
        auto result = make_unique<SamplerMetal>();
        result->_device = this;
        result->_sampler = sampler;
        return result;
    }
}

Nullable<unique_ptr<BindlessArray>> DeviceMetal::CreateBindlessArray(const BindlessArrayDescriptor& desc) noexcept {
    @autoreleasepool {
        // Each slot is 16 bytes (sizeof(uint64_t) * 2), matching Metal GPU resource layout
        static constexpr uint64_t kSlotSize = sizeof(uint64_t) * 2;
        uint64_t bufferSize = static_cast<uint64_t>(desc.Size) * kSlotSize;
        id<MTLBuffer> argBuf = [_device newBufferWithLength:bufferSize
                                                    options:MTLResourceStorageModeShared];
        if (argBuf == nil) {
            RADRAY_ERR_LOG("metal cannot create bindless argument buffer (size={})", bufferSize);
            return nullptr;
        }
        if (desc.Name.size() > 0) {
            argBuf.label = [[NSString alloc] initWithBytes:desc.Name.data()
                                                    length:desc.Name.size()
                                                  encoding:NSUTF8StringEncoding];
        }
        memset(argBuf.contents, 0, bufferSize);
        auto result = make_unique<BindlessArrayMetal>();
        result->_device = this;
        result->_argumentBuffer = argBuf;
        result->_trackedResources.resize(desc.Size);
        result->_size = desc.Size;
        result->_slotType = desc.SlotType;
        return result;
    }
}

CmdQueueMetal::CmdQueueMetal(
    DeviceMetal* device,
    id<MTLCommandQueue> queue) noexcept
    : _device(device),
      _queue(queue) {}

bool CmdQueueMetal::IsValid() const noexcept { return _queue != nil; }

void CmdQueueMetal::Destroy() noexcept {
    _queue = nil;
    _device = nullptr;
}

void CmdQueueMetal::Submit(const CommandQueueSubmitDescriptor& desc) noexcept {
    RADRAY_ASSERT(desc.CmdBuffers.size() > 0);
    @autoreleasepool {
        for (auto* semBase : desc.WaitSemaphores) {
            auto* sem = CastMtlObject(semBase);
            auto* cmdBuf = CastMtlObject(desc.CmdBuffers[0]);
            [cmdBuf->_cmdBuffer encodeWaitForEvent:sem->_event value:sem->_fenceValue - 1];
        }
        if (desc.SignalFence.HasValue()) {
            auto* cmdBuf = CastMtlObject(desc.CmdBuffers[desc.CmdBuffers.size() - 1]);
            auto* fence = CastMtlObject(desc.SignalFence.Get());
            [cmdBuf->_cmdBuffer encodeSignalEvent:fence->_event value:fence->_fenceValue++];
        }
        for (auto* semBase : desc.SignalSemaphores) {
            auto* sem = CastMtlObject(semBase);
            auto* cmdBuf = CastMtlObject(desc.CmdBuffers[desc.CmdBuffers.size() - 1]);
            [cmdBuf->_cmdBuffer encodeSignalEvent:sem->_event value:sem->_fenceValue++];
        }
        for (auto* cmdBufBase : desc.CmdBuffers) {
            auto* cmdBuf = CastMtlObject(cmdBufBase);
            [cmdBuf->_cmdBuffer commit];
        }
    }
}

void CmdQueueMetal::Wait() noexcept {
    @autoreleasepool {
        id<MTLCommandBuffer> cmdBuf = [_queue commandBufferWithUnretainedReferences];
        [cmdBuf commit];
        [cmdBuf waitUntilCompleted];
    }
}

CmdBufferMetal::CmdBufferMetal(
    DeviceMetal* device,
    CmdQueueMetal* queue) noexcept
    : _device(device),
      _queue(queue) {}

CmdBufferMetal::~CmdBufferMetal() noexcept { DestroyImpl(); }

bool CmdBufferMetal::IsValid() const noexcept { return _queue != nullptr; }

void CmdBufferMetal::Destroy() noexcept { DestroyImpl(); }

void CmdBufferMetal::DestroyImpl() noexcept {
    _blitEncoder = nil;
    _cmdBuffer = nil;
    _queue = nullptr;
    _device = nullptr;
}

void CmdBufferMetal::EndBlitEncoderIfActive() noexcept {
    if (_blitEncoder != nil) {
        [_blitEncoder endEncoding];
        _blitEncoder = nil;
    }
}

void CmdBufferMetal::Begin() noexcept {
    @autoreleasepool {
        _blitEncoder = nil;
        _cmdBuffer = [_queue->_queue commandBufferWithUnretainedReferences];
    }
}

void CmdBufferMetal::End() noexcept {
    EndBlitEncoderIfActive();
}

void CmdBufferMetal::ResourceBarrier(
    std::span<const BarrierBufferDescriptor> buffers,
    std::span<const BarrierTextureDescriptor> textures) noexcept {
    RADRAY_UNUSED(buffers);
    RADRAY_UNUSED(textures);
}

Nullable<unique_ptr<GraphicsCommandEncoder>> CmdBufferMetal::BeginRenderPass(const RenderPassDescriptor& desc) noexcept {
    EndBlitEncoderIfActive();
    @autoreleasepool {
        MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor renderPassDescriptor];
        for (uint32_t i = 0; i < desc.ColorAttachments.size(); i++) {
            auto& ca = desc.ColorAttachments[i];
            auto* texView = CastMtlObject(ca.Target);
            rpd.colorAttachments[i].texture = texView->_textureView;
            rpd.colorAttachments[i].loadAction = MapLoadAction(ca.Load);
            rpd.colorAttachments[i].storeAction = MapStoreAction(ca.Store);
            rpd.colorAttachments[i].clearColor = MTLClearColorMake(
                ca.ClearValue.Value[0], ca.ClearValue.Value[1],
                ca.ClearValue.Value[2], ca.ClearValue.Value[3]);
        }
        if (desc.DepthStencilAttachment.has_value()) {
            auto& dsa = desc.DepthStencilAttachment.value();
            auto* texView = CastMtlObject(dsa.Target);
            rpd.depthAttachment.texture = texView->_textureView;
            rpd.depthAttachment.loadAction = MapLoadAction(dsa.DepthLoad);
            rpd.depthAttachment.storeAction = MapStoreAction(dsa.DepthStore);
            rpd.depthAttachment.clearDepth = dsa.ClearValue.Depth;
            rpd.stencilAttachment.texture = texView->_textureView;
            rpd.stencilAttachment.loadAction = MapLoadAction(dsa.StencilLoad);
            rpd.stencilAttachment.storeAction = MapStoreAction(dsa.StencilStore);
            rpd.stencilAttachment.clearStencil = dsa.ClearValue.Stencil;
        }
        id<MTLRenderCommandEncoder> encoder = [_cmdBuffer renderCommandEncoderWithDescriptor:rpd];
        if (encoder == nil) {
            RADRAY_ERR_LOG("metal cannot create render command encoder");
            return nullptr;
        }
        if (!desc.Name.empty()) {
            encoder.label = [[NSString alloc] initWithBytes:desc.Name.data()
                                                     length:desc.Name.size()
                                                   encoding:NSUTF8StringEncoding];
        }
        auto enc = make_unique<GraphicsCmdEncoderMetal>();
        enc->_cmdBuffer = this;
        enc->_encoder = encoder;
        return enc;
    }
}

void CmdBufferMetal::EndRenderPass(unique_ptr<GraphicsCommandEncoder> encoder) noexcept {
    @autoreleasepool {
        auto* enc = static_cast<GraphicsCmdEncoderMetal*>(encoder.get());
        if (enc && enc->_encoder != nil) {
            [enc->_encoder endEncoding];
        }
    }
}

Nullable<unique_ptr<ComputeCommandEncoder>> CmdBufferMetal::BeginComputePass() noexcept {
    EndBlitEncoderIfActive();
    @autoreleasepool {
        id<MTLComputeCommandEncoder> encoder = [_cmdBuffer computeCommandEncoder];
        if (encoder == nil) {
            RADRAY_ERR_LOG("metal cannot create compute command encoder");
            return nullptr;
        }
        auto enc = make_unique<ComputeCmdEncoderMetal>();
        enc->_cmdBuffer = this;
        enc->_encoder = encoder;
        return enc;
    }
}

void CmdBufferMetal::EndComputePass(unique_ptr<ComputeCommandEncoder> encoder) noexcept {
    @autoreleasepool {
        auto* enc = static_cast<ComputeCmdEncoderMetal*>(encoder.get());
        if (enc && enc->_encoder != nil) {
            [enc->_encoder endEncoding];
        }
    }
}

void CmdBufferMetal::CopyBufferToBuffer(Buffer* dst, uint64_t dstOffset, Buffer* src, uint64_t srcOffset, uint64_t size) noexcept {
    @autoreleasepool {
        auto* mtlDst = CastMtlObject(dst);
        auto* mtlSrc = CastMtlObject(src);
        if (_blitEncoder == nil) {
            _blitEncoder = [_cmdBuffer blitCommandEncoder];
        }
        [_blitEncoder copyFromBuffer:mtlSrc->_buffer
                        sourceOffset:srcOffset
                            toBuffer:mtlDst->_buffer
                   destinationOffset:dstOffset
                                size:size];
    }
}

void CmdBufferMetal::CopyBufferToTexture(Texture* dst, SubresourceRange dstRange, Buffer* src, uint64_t srcOffset) noexcept {
    @autoreleasepool {
        auto* mtlDst = CastMtlObject(dst);
        auto* mtlSrc = CastMtlObject(src);
        const auto& texDesc = mtlDst->_desc;
        uint32_t bpp = GetTextureFormatBytesPerPixel(texDesc.Format);
        if (_blitEncoder == nil) {
            _blitEncoder = [_cmdBuffer blitCommandEncoder];
        }
        uint32_t mipLevels = dstRange.MipLevelCount == SubresourceRange::All
                                 ? texDesc.MipLevels - dstRange.BaseMipLevel
                                 : dstRange.MipLevelCount;
        uint32_t layerCount = dstRange.ArrayLayerCount == SubresourceRange::All
                                  ? texDesc.DepthOrArraySize - dstRange.BaseArrayLayer
                                  : dstRange.ArrayLayerCount;
        uint64_t bufferOffset = srcOffset;
        for (uint32_t mip = 0; mip < mipLevels; mip++) {
            uint32_t mipLevel = dstRange.BaseMipLevel + mip;
            uint32_t mipWidth = std::max(texDesc.Width >> mipLevel, 1u);
            uint32_t mipHeight = std::max(texDesc.Height >> mipLevel, 1u);
            NSUInteger bytesPerRow = static_cast<NSUInteger>(mipWidth) * bpp;
            NSUInteger bytesPerImage = bytesPerRow * mipHeight;
            for (uint32_t layer = 0; layer < layerCount; layer++) {
                uint32_t dstSlice = dstRange.BaseArrayLayer + layer;
                [_blitEncoder copyFromBuffer:mtlSrc->_buffer
                                sourceOffset:bufferOffset
                           sourceBytesPerRow:bytesPerRow
                         sourceBytesPerImage:bytesPerImage
                                  sourceSize:MTLSizeMake(mipWidth, mipHeight, 1)
                                   toTexture:mtlDst->_texture
                            destinationSlice:dstSlice
                            destinationLevel:mipLevel
                           destinationOrigin:MTLOriginMake(0, 0, 0)];
                bufferOffset += bytesPerImage;
            }
        }
    }
}

FenceMetal::FenceMetal(
    DeviceMetal* device,
    id<MTLSharedEvent> event,
    uint64_t initValue) noexcept
    : _device(device),
      _event(event),
      _fenceValue(initValue) {}

bool FenceMetal::IsValid() const noexcept { return _event != nil; }

void FenceMetal::Destroy() noexcept {
    _event = nil;
    _device = nullptr;
}

FenceStatus FenceMetal::GetStatus() const noexcept {
    uint64_t completedValue = _event.signaledValue;
    uint64_t signaledValue = _fenceValue - 1;
    return completedValue < signaledValue ? FenceStatus::Incomplete : FenceStatus::Complete;
}

void FenceMetal::Wait() noexcept {
    uint64_t completedValue = _event.signaledValue;
    uint64_t signaledValue = _fenceValue - 1;
    if (completedValue < signaledValue) {
        [_event waitUntilSignaledValue:signaledValue timeoutMS:UINT64_MAX];
    }
}

void FenceMetal::Reset() noexcept {}

SemaphoreMetal::SemaphoreMetal(
    DeviceMetal* device,
    id<MTLEvent> event,
    uint64_t initValue) noexcept
    : _device(device),
      _event(event),
      _fenceValue(initValue) {}

bool SemaphoreMetal::IsValid() const noexcept { return _event != nil; }

void SemaphoreMetal::Destroy() noexcept {
    _event = nil;
    _device = nullptr;
}

SwapChainMetal::SwapChainMetal(
    DeviceMetal* device,
    CmdQueueMetal* presentQueue,
    CAMetalLayer* layer,
    uint32_t backBufferCount,
    TextureFormat format) noexcept
    : _device(device),
      _presentQueue(presentQueue),
      _layer(layer) {
    CFRetain((__bridge CFTypeRef)layer);
    _imageAcquiredSemaphore = dispatch_semaphore_create(backBufferCount - 1);
    _drawables = [[NSMutableArray alloc] initWithCapacity:backBufferCount];
    for (uint32_t i = 0; i < backBufferCount; i++) {
        [_drawables addObject:(id<CAMetalDrawable>)[NSNull null]];
    }
    _backBufferTextures.reserve(backBufferCount);
    for (uint32_t i = 0; i < backBufferCount; i++) {
        auto& tex = _backBufferTextures.emplace_back(make_unique<TextureMetal>());
        tex->_device = _device;
        tex->_isExternalOwned = true;
        auto& texDesc = tex->_desc;
        texDesc.Dim = TextureDimension::Dim2D;
        texDesc.Width = static_cast<uint32_t>(layer.drawableSize.width);
        texDesc.Height = static_cast<uint32_t>(layer.drawableSize.height);
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.SampleCount = 1;
        texDesc.Format = format;
        tex->_texture = nil;
    }
}

SwapChainMetal::~SwapChainMetal() noexcept { DestroyImpl(); }

bool SwapChainMetal::IsValid() const noexcept { return _layer != nil; }

void SwapChainMetal::Destroy() noexcept { DestroyImpl(); }

void SwapChainMetal::DestroyImpl() noexcept {
    @autoreleasepool {
        if (_imageAcquiredSemaphore) {
            for (uint32_t i = 0; i < _backBufferTextures.size() - 1; i++) {
                dispatch_semaphore_signal(_imageAcquiredSemaphore);
            }
            _imageAcquiredSemaphore = nil;
        }
        _backBufferTextures.clear();
        [_drawables removeAllObjects];
        _drawables = nil;
        if (_layer) {
            CFRelease((__bridge CFTypeRef)_layer);
            _layer = nil;
        }
        _presentQueue = nullptr;
        _device = nullptr;
    }
}

Nullable<Texture*> SwapChainMetal::AcquireNext(Nullable<Semaphore*> signalSemaphore, Nullable<Fence*> signalFence) noexcept {
    @autoreleasepool {
        dispatch_semaphore_wait(_imageAcquiredSemaphore, DISPATCH_TIME_FOREVER);
        id<CAMetalDrawable> drawable = [_layer nextDrawable];
        if (drawable == nil) {
            dispatch_semaphore_signal(_imageAcquiredSemaphore);
            RADRAY_ERR_LOG("metal cannot acquire next drawable");
            return nullptr;
        }
        uint32_t index = _currentIndex;
        _drawables[index] = drawable;
        auto* tex = _backBufferTextures[index].get();
        tex->_texture = drawable.texture;
        if (signalSemaphore.HasValue() || signalFence.HasValue()) {
            id<MTLCommandBuffer> signalCmd = [_presentQueue->_queue commandBufferWithUnretainedReferences];
            signalCmd.label = @"acquire signal";
            if (signalSemaphore.HasValue()) {
                auto* sem = CastMtlObject(signalSemaphore.Get());
                [signalCmd encodeSignalEvent:sem->_event value:sem->_fenceValue++];
            }
            if (signalFence.HasValue()) {
                auto* fence = CastMtlObject(signalFence.Get());
                [signalCmd encodeSignalEvent:fence->_event value:fence->_fenceValue++];
                MTLSharedEventListener* listener = [[MTLSharedEventListener alloc] init];
            }
            [signalCmd commit];
        }
        return tex;
    }
}

void SwapChainMetal::Present(std::span<Semaphore*> waitSemaphores) noexcept {
    @autoreleasepool {
        uint32_t presentIndex = _currentIndex;
        id<CAMetalDrawable> drawable = _drawables[presentIndex];
        if (drawable == nil || drawable == (id<CAMetalDrawable>)[NSNull null]) {
            RADRAY_ERR_LOG("metal no drawable at present index {}", presentIndex);
            return;
        }
        id<MTLCommandBuffer> cmdBuf = [_presentQueue->_queue commandBufferWithUnretainedReferences];
        cmdBuf.label = @"present";
        for (auto* semBase : waitSemaphores) {
            auto* sem = CastMtlObject(semBase);
            [cmdBuf encodeWaitForEvent:sem->_event value:sem->_fenceValue - 1];
        }
        [cmdBuf presentDrawable:drawable];
        [cmdBuf addCompletedHandler:^(id<MTLCommandBuffer> _Nonnull) {
            _drawables[presentIndex] = (id<CAMetalDrawable>)[NSNull null];
            dispatch_semaphore_signal(_imageAcquiredSemaphore);
        }];
        [cmdBuf commit];
        _currentIndex = (_currentIndex + 1) % _backBufferTextures.size();
    }
}

Nullable<Texture*> SwapChainMetal::GetCurrentBackBuffer() const noexcept {
    return _backBufferTextures[_currentIndex].get();
}

uint32_t SwapChainMetal::GetCurrentBackBufferIndex() const noexcept {
    return _currentIndex;
}

uint32_t SwapChainMetal::GetBackBufferCount() const noexcept {
    return _backBufferTextures.size();
}

BufferMetal::BufferMetal(
    DeviceMetal* device,
    id<MTLBuffer> buffer) noexcept
    : _device(device),
      _buffer(buffer) {}

bool BufferMetal::IsValid() const noexcept { return _buffer != nil; }

void BufferMetal::Destroy() noexcept {
    _mappedPtr = nullptr;
    _buffer = nil;
    _device = nullptr;
}

void* BufferMetal::Map(uint64_t offset, uint64_t size) noexcept {
    RADRAY_ASSERT(size <= _desc.Size);
    void* ptr = nullptr;
    if (_mappedPtr) {
        ptr = _mappedPtr;
    } else {
        ptr = _buffer.contents;
    }
    return static_cast<uint8_t*>(ptr) + offset;
}

void BufferMetal::Unmap(uint64_t offset, uint64_t size) noexcept {
#if defined(RADRAY_PLATFORM_MACOS)
    if (_buffer.storageMode == MTLStorageModeManaged) {
        [_buffer didModifyRange:NSMakeRange(offset, size)];
    }
    _mappedPtr = nullptr;
#else
    RADRAY_UNUSED(offset);
    RADRAY_UNUSED(size);
#endif
}

BufferDescriptor BufferMetal::GetDesc() const noexcept { return _desc; }

TextureMetal::~TextureMetal() noexcept { DestroyImpl(); }

bool TextureMetal::IsValid() const noexcept { return _texture != nil; }

void TextureMetal::Destroy() noexcept { DestroyImpl(); }

void TextureMetal::DestroyImpl() noexcept {
    if (!_isExternalOwned) {
        _texture = nil;
    }
    _texture = nil;
}

ShaderMetal::~ShaderMetal() noexcept { DestroyImpl(); }

bool ShaderMetal::IsValid() const noexcept { return _library != nil; }

void ShaderMetal::Destroy() noexcept { DestroyImpl(); }

void ShaderMetal::DestroyImpl() noexcept { _library = nil; }

RootSignatureMetal::~RootSignatureMetal() noexcept { DestroyImpl(); }

bool RootSignatureMetal::IsValid() const noexcept { return _device != nullptr; }

void RootSignatureMetal::Destroy() noexcept { DestroyImpl(); }

void RootSignatureMetal::DestroyImpl() noexcept {
    _cachedStaticSamplers.clear();
    _container = {};
}

GraphicsPipelineStateMetal::~GraphicsPipelineStateMetal() noexcept { DestroyImpl(); }

bool GraphicsPipelineStateMetal::IsValid() const noexcept { return _pipelineState != nil; }

void GraphicsPipelineStateMetal::Destroy() noexcept { DestroyImpl(); }

void GraphicsPipelineStateMetal::DestroyImpl() noexcept {
    _pipelineState = nil;
    _depthStencilState = nil;
}

ComputePipelineStateMetal::~ComputePipelineStateMetal() noexcept { DestroyImpl(); }

bool ComputePipelineStateMetal::IsValid() const noexcept { return _pipelineState != nil; }

void ComputePipelineStateMetal::Destroy() noexcept { DestroyImpl(); }

void ComputePipelineStateMetal::DestroyImpl() noexcept { _pipelineState = nil; }

GraphicsCmdEncoderMetal::~GraphicsCmdEncoderMetal() noexcept { DestroyImpl(); }

bool GraphicsCmdEncoderMetal::IsValid() const noexcept { return _encoder != nil; }

void GraphicsCmdEncoderMetal::Destroy() noexcept { DestroyImpl(); }

void GraphicsCmdEncoderMetal::DestroyImpl() noexcept {
    _encoder = nil;
    _indexBuffer = nil;
}

CommandBuffer* GraphicsCmdEncoderMetal::GetCommandBuffer() const noexcept { return _cmdBuffer; }

void GraphicsCmdEncoderMetal::BindRootSignature(RootSignature* rootSig) noexcept {
    _boundRootSig = CastMtlObject(rootSig);
    // bind cached static samplers
    if (_boundRootSig) {
        for (auto& ss : _boundRootSig->_cachedStaticSamplers) {
            if (ss.Stages.HasFlag(ShaderStage::Vertex)) {
                [_encoder setVertexSamplerState:ss.sampler atIndex:ss.Slot];
            }
            if (ss.Stages.HasFlag(ShaderStage::Pixel)) {
                [_encoder setFragmentSamplerState:ss.sampler atIndex:ss.Slot];
            }
        }
    }
}

void GraphicsCmdEncoderMetal::PushConstant(const void* data, size_t length) noexcept {
    if (_boundRootSig && _boundRootSig->_container._desc.Constant.has_value()) {
        auto& pc = _boundRootSig->_container._desc.Constant.value();
        if (pc.Stages.HasFlag(ShaderStage::Vertex)) {
            [_encoder setVertexBytes:data length:length atIndex:pc.Slot];
        }
        if (pc.Stages.HasFlag(ShaderStage::Pixel)) {
            [_encoder setFragmentBytes:data length:length atIndex:pc.Slot];
        }
    }
}

void GraphicsCmdEncoderMetal::BindRootDescriptor(uint32_t slot, Buffer* buffer, uint64_t offset, uint64_t size) noexcept {
    RADRAY_UNUSED(size);
    auto* mtlBuf = CastMtlObject(buffer);
    if (_boundRootSig) {
        for (auto& rd : _boundRootSig->_container._rootDescriptors) {
            if (rd.Slot == slot) {
                if (rd.Stages.HasFlag(ShaderStage::Vertex)) {
                    [_encoder setVertexBuffer:mtlBuf->_buffer offset:offset atIndex:slot];
                }
                if (rd.Stages.HasFlag(ShaderStage::Pixel)) {
                    [_encoder setFragmentBuffer:mtlBuf->_buffer offset:offset atIndex:slot];
                }
                return;
            }
        }
    }
}

void GraphicsCmdEncoderMetal::BindDescriptorSet(uint32_t slot, DescriptorSet* set) noexcept {
    auto* ds = CastMtlObject(set);
    if (!ds || !ds->_argumentBuffer) return;

    uint32_t bufferIndex = ds->_setIndex;
    if (ds->_combinedStages.HasFlag(ShaderStage::Vertex)) {
        [_encoder setVertexBuffer:ds->_argumentBuffer offset:0 atIndex:bufferIndex];
    }
    if (ds->_combinedStages.HasFlag(ShaderStage::Pixel)) {
        [_encoder setFragmentBuffer:ds->_argumentBuffer offset:0 atIndex:bufferIndex];
    }

    // Declare resource residency (batch)
    MTLRenderStages stages = 0;
    if (ds->_combinedStages.HasFlag(ShaderStage::Vertex)) stages |= MTLRenderStageVertex;
    if (ds->_combinedStages.HasFlag(ShaderStage::Pixel)) stages |= MTLRenderStageFragment;
    vector<id<MTLResource>> readResources;
    vector<id<MTLResource>> readWriteResources;
    for (auto& tracked : ds->_trackedResources) {
        if (tracked.resource == nil) continue;
        if (tracked.usage & MTLResourceUsageWrite) {
            readWriteResources.push_back(tracked.resource);
        } else {
            readResources.push_back(tracked.resource);
        }
    }
    if (!readResources.empty()) {
        [_encoder useResources:readResources.data() count:readResources.size() usage:MTLResourceUsageRead stages:stages];
    }
    if (!readWriteResources.empty()) {
        [_encoder useResources:readWriteResources.data() count:readWriteResources.size() usage:(MTLResourceUsageRead | MTLResourceUsageWrite) stages:stages];
    }
}

void GraphicsCmdEncoderMetal::BindBindlessArray(uint32_t slot, BindlessArray* array) noexcept {
    if (!_boundRootSig) return;
    auto* bindless = CastMtlObject(array);
    // Find the bindless descriptor matching this set index
    const RootSignatureBindlessDescriptor* desc = nullptr;
    for (auto& ds : _boundRootSig->_container._descriptorSets) {
        for (auto& bd : ds.BindlessDescriptors) {
            if (bd.SetIndex == slot) {
                desc = &bd;
                break;
            }
        }
        if (desc) break;
    }
    if (!desc) {
        RADRAY_ERR_LOG("metal BindBindlessArray: no bindless descriptor found for set index {}", slot);
        return;
    }
    uint32_t bufferIndex = desc->Slot;
    if (desc->Stages.HasFlag(ShaderStage::Vertex)) {
        [_encoder setVertexBuffer:bindless->_argumentBuffer offset:0 atIndex:bufferIndex];
    }
    if (desc->Stages.HasFlag(ShaderStage::Pixel)) {
        [_encoder setFragmentBuffer:bindless->_argumentBuffer offset:0 atIndex:bufferIndex];
    }
    // Declare resource residency for argument buffer and all tracked resources
    MTLRenderStages stages = 0;
    if (desc->Stages.HasFlag(ShaderStage::Vertex)) stages |= MTLRenderStageVertex;
    if (desc->Stages.HasFlag(ShaderStage::Pixel)) stages |= MTLRenderStageFragment;
    [_encoder useResource:bindless->_argumentBuffer usage:MTLResourceUsageRead stages:stages];
    for (auto& tracked : bindless->_trackedResources) {
        if (tracked.resource != nil) {
            [_encoder useResource:tracked.resource usage:tracked.usage stages:stages];
        }
    }
}

void GraphicsCmdEncoderMetal::SetViewport(Viewport vp) noexcept {
    MTLViewport mtlVp;
    mtlVp.originX = vp.X;
    mtlVp.originY = vp.Y;
    mtlVp.width = vp.Width;
    mtlVp.height = vp.Height;
    mtlVp.znear = vp.MinDepth;
    mtlVp.zfar = vp.MaxDepth;
    [_encoder setViewport:mtlVp];
}

void GraphicsCmdEncoderMetal::SetScissor(Rect rect) noexcept {
    MTLScissorRect sr;
    sr.x = rect.X >= 0 ? static_cast<NSUInteger>(rect.X) : 0;
    sr.y = rect.Y >= 0 ? static_cast<NSUInteger>(rect.Y) : 0;
    sr.width = rect.Width;
    sr.height = rect.Height;
    [_encoder setScissorRect:sr];
}

void GraphicsCmdEncoderMetal::BindVertexBuffer(std::span<const VertexBufferView> vbv) noexcept {
    for (uint32_t i = 0; i < vbv.size(); i++) {
        auto* mtlBuf = CastMtlObject(vbv[i].Target);
        [_encoder setVertexBuffer:mtlBuf->_buffer offset:vbv[i].Offset atIndex:(kMtlVertexBufferBase - i)];
    }
}

void GraphicsCmdEncoderMetal::BindIndexBuffer(IndexBufferView ibv) noexcept {
    auto* mtlBuf = CastMtlObject(ibv.Target);
    _indexBuffer = mtlBuf->_buffer;
    _indexType = (ibv.Stride == 2) ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
    _indexBufferOffset = ibv.Offset;
}

void GraphicsCmdEncoderMetal::BindGraphicsPipelineState(GraphicsPipelineState* pso) noexcept {
    auto* mtlPso = CastMtlObject(pso);
    [_encoder setRenderPipelineState:mtlPso->_pipelineState];
    if (mtlPso->_depthStencilState != nil) {
        [_encoder setDepthStencilState:mtlPso->_depthStencilState];
    }
    [_encoder setCullMode:mtlPso->_cullMode];
    [_encoder setFrontFacingWinding:mtlPso->_winding];
    [_encoder setTriangleFillMode:mtlPso->_fillMode];
    [_encoder setDepthBias:mtlPso->_depthBiasConstant
                slopeScale:mtlPso->_depthBiasSlopeScale
                     clamp:mtlPso->_depthBiasClamp];
    _primitiveType = mtlPso->_primitiveType;
}

void GraphicsCmdEncoderMetal::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) noexcept {
    [_encoder drawPrimitives:_primitiveType
                 vertexStart:firstVertex
                 vertexCount:vertexCount
               instanceCount:instanceCount
                baseInstance:firstInstance];
}

void GraphicsCmdEncoderMetal::DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) noexcept {
    if (_indexBuffer == nil) {
        RADRAY_ERR_LOG("metal DrawIndexed called without bound index buffer");
        return;
    }
    NSUInteger indexStride = (_indexType == MTLIndexTypeUInt16) ? 2 : 4;
    NSUInteger offset = _indexBufferOffset + firstIndex * indexStride;
    [_encoder drawIndexedPrimitives:_primitiveType
                         indexCount:indexCount
                          indexType:_indexType
                        indexBuffer:_indexBuffer
                  indexBufferOffset:offset
                      instanceCount:instanceCount
                         baseVertex:vertexOffset
                       baseInstance:firstInstance];
}

ComputeCmdEncoderMetal::~ComputeCmdEncoderMetal() noexcept { DestroyImpl(); }

bool ComputeCmdEncoderMetal::IsValid() const noexcept { return _encoder != nil; }

void ComputeCmdEncoderMetal::Destroy() noexcept { DestroyImpl(); }

void ComputeCmdEncoderMetal::DestroyImpl() noexcept { _encoder = nil; }

CommandBuffer* ComputeCmdEncoderMetal::GetCommandBuffer() const noexcept { return _cmdBuffer; }

void ComputeCmdEncoderMetal::BindRootSignature(RootSignature* rootSig) noexcept {
    _boundRootSig = CastMtlObject(rootSig);
    if (_boundRootSig) {
        for (auto& ss : _boundRootSig->_cachedStaticSamplers) {
            if (ss.Stages.HasFlag(ShaderStage::Compute)) {
                [_encoder setSamplerState:ss.sampler atIndex:ss.Slot];
            }
        }
    }
}

void ComputeCmdEncoderMetal::PushConstant(const void* data, size_t length) noexcept {
    if (_boundRootSig && _boundRootSig->_container._desc.Constant.has_value()) {
        auto& pc = _boundRootSig->_container._desc.Constant.value();
        [_encoder setBytes:data length:length atIndex:pc.Slot];
    }
}

void ComputeCmdEncoderMetal::BindRootDescriptor(uint32_t slot, Buffer* buffer, uint64_t offset, uint64_t size) noexcept {
    RADRAY_UNUSED(size);
    auto* mtlBuf = CastMtlObject(buffer);
    [_encoder setBuffer:mtlBuf->_buffer offset:offset atIndex:slot];
}

void ComputeCmdEncoderMetal::BindDescriptorSet(uint32_t slot, DescriptorSet* set) noexcept {
    auto* ds = CastMtlObject(set);
    if (!ds || !ds->_argumentBuffer) return;

    [_encoder setBuffer:ds->_argumentBuffer offset:0 atIndex:ds->_setIndex];
    vector<id<MTLResource>> readResources;
    vector<id<MTLResource>> readWriteResources;
    for (auto& tracked : ds->_trackedResources) {
        if (tracked.resource == nil) continue;
        if (tracked.usage & MTLResourceUsageWrite) {
            readWriteResources.push_back(tracked.resource);
        } else {
            readResources.push_back(tracked.resource);
        }
    }
    if (!readResources.empty()) {
        [_encoder useResources:readResources.data() count:readResources.size() usage:MTLResourceUsageRead];
    }
    if (!readWriteResources.empty()) {
        [_encoder useResources:readWriteResources.data() count:readWriteResources.size() usage:(MTLResourceUsageRead | MTLResourceUsageWrite)];
    }
}

void ComputeCmdEncoderMetal::BindBindlessArray(uint32_t slot, BindlessArray* array) noexcept {
    if (!_boundRootSig) return;
    auto* bindless = CastMtlObject(array);
    // Find the bindless descriptor matching this set index
    const RootSignatureBindlessDescriptor* desc = nullptr;
    for (auto& ds : _boundRootSig->_container._descriptorSets) {
        for (auto& bd : ds.BindlessDescriptors) {
            if (bd.SetIndex == slot) {
                desc = &bd;
                break;
            }
        }
        if (desc) break;
    }
    if (!desc) {
        RADRAY_ERR_LOG("metal BindBindlessArray: no bindless descriptor found for set index {}", slot);
        return;
    }
    uint32_t bufferIndex = desc->Slot;
    [_encoder setBuffer:bindless->_argumentBuffer offset:0 atIndex:bufferIndex];
    // Declare resource residency for argument buffer and all tracked resources
    [_encoder useResource:bindless->_argumentBuffer usage:MTLResourceUsageRead];
    for (auto& tracked : bindless->_trackedResources) {
        if (tracked.resource != nil) {
            [_encoder useResource:tracked.resource usage:tracked.usage];
        }
    }
}

void ComputeCmdEncoderMetal::BindComputePipelineState(ComputePipelineState* pso) noexcept {
    auto* mtlPso = CastMtlObject(pso);
    [_encoder setComputePipelineState:mtlPso->_pipelineState];
}

void ComputeCmdEncoderMetal::Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) noexcept {
    MTLSize threadgroups = MTLSizeMake(groupCountX, groupCountY, groupCountZ);
    [_encoder dispatchThreadgroups:threadgroups threadsPerThreadgroup:_threadGroupSize];
}

void ComputeCmdEncoderMetal::SetThreadGroupSize(uint32_t x, uint32_t y, uint32_t z) noexcept {
    _threadGroupSize = MTLSizeMake(x, y, z);
}

TextureViewMetal::~TextureViewMetal() noexcept { DestroyImpl(); }

bool TextureViewMetal::IsValid() const noexcept { return _textureView != nil; }

void TextureViewMetal::Destroy() noexcept { DestroyImpl(); }

void TextureViewMetal::DestroyImpl() noexcept { _textureView = nil; }

BufferViewMetal::BufferViewMetal(
    DeviceMetal* device,
    BufferMetal* buffer) noexcept
    : _device(device),
      _buffer(buffer) {}

bool BufferViewMetal::IsValid() const noexcept { return _buffer != nullptr; }

void BufferViewMetal::Destroy() noexcept {
    _buffer = nullptr;
    _device = nullptr;
}

SamplerMetal::~SamplerMetal() noexcept { DestroyImpl(); }

bool SamplerMetal::IsValid() const noexcept { return _sampler != nil; }

void SamplerMetal::Destroy() noexcept { DestroyImpl(); }

void SamplerMetal::DestroyImpl() noexcept { _sampler = nil; }

DescriptorSetMetal::~DescriptorSetMetal() noexcept { DestroyImpl(); }

bool DescriptorSetMetal::IsValid() const noexcept { return _argumentBuffer != nil; }

void DescriptorSetMetal::Destroy() noexcept { DestroyImpl(); }

void DescriptorSetMetal::DestroyImpl() noexcept {
    _trackedResources.clear();
    _argumentEncoder = nil;
    _argumentBuffer = nil;
}

void DescriptorSetMetal::SetResource(uint32_t slot, uint32_t index, ResourceView* view) noexcept {
    @autoreleasepool {
        if (_argumentEncoder == nil || _argumentBuffer == nil) return;
        [_argumentEncoder setArgumentBuffer:_argumentBuffer offset:0];
        uint32_t argIndex = slot + index;
        id<MTLResource> res = nil;
        MTLResourceUsage usage = MTLResourceUsageRead;
        auto tag = view->GetTag();
        if (tag == RenderObjectTag::TextureView) {
            auto* texView = static_cast<TextureViewMetal*>(view);
            [_argumentEncoder setTexture:texView->_textureView atIndex:argIndex];
            res = texView->_textureView;
        } else if (tag == RenderObjectTag::BufferView) {
            auto* bufView = static_cast<BufferViewMetal*>(view);
            [_argumentEncoder setBuffer:bufView->_buffer->_buffer offset:bufView->_desc.Range.Offset atIndex:argIndex];
            res = bufView->_buffer->_buffer;
        }
        if (res == nil) return;
        for (auto& t : _trackedResources) {
            if (t.argIndex == argIndex) {
                t.resource = res;
                t.usage = usage;
                return;
            }
        }
        _trackedResources.push_back({argIndex, res, usage});
    }
}

BindlessArrayMetal::~BindlessArrayMetal() noexcept { DestroyImpl(); }

bool BindlessArrayMetal::IsValid() const noexcept { return _device != nullptr; }

void BindlessArrayMetal::Destroy() noexcept { DestroyImpl(); }

void BindlessArrayMetal::DestroyImpl() noexcept {
    _trackedResources.clear();
    _argumentBuffer = nil;
}

void BindlessArrayMetal::SetBuffer(uint32_t slot, BufferView* bufView) noexcept {
    if (slot >= _size) {
        RADRAY_ERR_LOG("metal BindlessArray::SetBuffer slot {} out of range (size={})", slot, _size);
        return;
    }
    auto* mtlBufView = CastMtlObject(bufView);
    static constexpr uint64_t kSlotSize = sizeof(uint64_t) * 2;
    uint8_t* pArg = static_cast<uint8_t*>(_argumentBuffer.contents) + slot * kSlotSize;
    // BUFFER layout: { uint64_t gpuAddress; uint64_t size; } — 16 bytes
    uint64_t gpuAddr = mtlBufView->_buffer->_buffer.gpuAddress + mtlBufView->_desc.Range.Offset;
    uint64_t bufSize = mtlBufView->_desc.Range.Size;
    struct {
        uint64_t ptr;
        uint64_t size;
    } bufArg = {gpuAddr, bufSize};
    memcpy(pArg, &bufArg, sizeof(bufArg));
    MTLResourceUsage usage = mtlBufView->_desc.Usage.HasFlag(BufferUse::UnorderedAccess)
                                 ? (MTLResourceUsageRead | MTLResourceUsageWrite)
                                 : MTLResourceUsageRead;
    _trackedResources[slot] = {mtlBufView->_buffer->_buffer, usage};
}

void BindlessArrayMetal::SetTexture(uint32_t slot, TextureView* texView, Sampler* sampler) noexcept {
    RADRAY_UNUSED(sampler);
    if (slot >= _size) {
        RADRAY_ERR_LOG("metal BindlessArray::SetTexture slot {} out of range (size={})", slot, _size);
        return;
    }
    auto* mtlTexView = CastMtlObject(texView);
    static constexpr uint64_t kSlotSize = sizeof(uint64_t) * 2;
    uint8_t* pArg = static_cast<uint8_t*>(_argumentBuffer.contents) + slot * kSlotSize;
    // TEXTURE layout: { MTLResourceID id; } — 8 bytes, padded to 16
    struct {
        MTLResourceID id;
    } texArg = {mtlTexView->_textureView.gpuResourceID};
    memcpy(pArg, &texArg, sizeof(texArg));
    MTLResourceUsage usage = mtlTexView->_desc.Usage.HasFlag(TextureUse::UnorderedAccess)
                                 ? (MTLResourceUsageRead | MTLResourceUsageWrite)
                                 : MTLResourceUsageRead;
    _trackedResources[slot] = {mtlTexView->_textureView, usage};
}

Nullable<shared_ptr<Device>> CreateDevice(const MetalDeviceDescriptor& desc) {
    @autoreleasepool {
        id<MTLDevice> device = nil;
#if defined(RADRAY_PLATFORM_MACOS)
        NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
        if (devices.count == 0) {
            RADRAY_ERR_LOG("metal cannot find any device");
            return nullptr;
        }
        for (NSUInteger i = 0; i < devices.count; i++) {
            RADRAY_INFO_LOG("metal find device: {}", [devices[i].name UTF8String]);
        }
        if (desc.DeviceIndex.has_value()) {
            uint32_t need = desc.DeviceIndex.value();
            if (need >= devices.count) {
                RADRAY_ERR_LOG("metal device index out of range (count={}, need={})", (uint32_t)devices.count, need);
                return nullptr;
            }
            device = devices[need];
        } else {
            device = devices[0];
        }
#elif defined(RADRAY_PLATFORM_IOS)
        device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            RADRAY_ERR_LOG("metal cannot create system default device");
            return nullptr;
        }
#endif
        // TODO: 目前只适配 metal3+tier2
        if (device.argumentBuffersSupport == MTLArgumentBuffersTier1) {
            RADRAY_ERR_LOG("metal device too old, argument buffer {}", device.argumentBuffersSupport);
            return nullptr;
        }
        auto result = make_shared<DeviceMetal>(device);
        result->_detail.GpuName = [device.name UTF8String];
        result->_detail.CBufferAlignment = 256;
        result->_detail.TextureDataPitchAlignment = 256;
        result->_detail.VramBudget = device.recommendedMaxWorkingSetSize;
        result->_detail.IsUMA = device.hasUnifiedMemory;
        result->_detail.IsBindlessArraySupported = [device supportsFamily:MTLGPUFamilyMetal3];
        RADRAY_INFO_LOG("metal select device: {}", result->_detail.GpuName);
        RADRAY_INFO_LOG("========== Metal Feature ==========");
        NSProcessInfo* pi = [NSProcessInfo processInfo];
        NSOperatingSystemVersion ver = [pi operatingSystemVersion];
#if defined(RADRAY_PLATFORM_MACOS)
        RADRAY_INFO_LOG("OS: macOS {}.{}.{}", (int)ver.majorVersion, (int)ver.minorVersion, (int)ver.patchVersion);
#elif defined(RADRAY_PLATFORM_IOS)
        RADRAY_INFO_LOG("OS: iOS {}.{}.{}", (int)ver.majorVersion, (int)ver.minorVersion, (int)ver.patchVersion);
#endif
        RADRAY_INFO_LOG("UMA: {}, VRAM Budget: {}MB", result->_detail.IsUMA, result->_detail.VramBudget / (1024 * 1024));
        MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
        RADRAY_INFO_LOG("Support Metal 3: {}", [device supportsFamily:MTLGPUFamilyMetal3] ? true : false);
        RADRAY_INFO_LOG("Language Version: {}", options.languageVersion);
        RADRAY_INFO_LOG("===================================");
        return result;
    }
}

}  // namespace radray::render::metal
