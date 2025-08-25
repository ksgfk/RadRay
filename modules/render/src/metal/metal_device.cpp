#include "metal_device.h"

#include <radray/basic_math.h>
#include <radray/render/shader.h>
#include "metal_function.h"
#include "metal_root_sig.h"
#include "metal_pipeline_state.h"

namespace radray::render::metal {

void DeviceMetal::Destroy() noexcept {
    _device.reset();
}

std::optional<CommandQueue*> DeviceMetal::GetCommandQueue(QueueType type, uint32_t slot) noexcept {
    return AutoRelease([this, type, slot]() noexcept {
        uint32_t index = static_cast<size_t>(type);
        RADRAY_ASSERT(index >= 0 && index < 3);
        auto& queues = _queues[index];
        if (queues.size() <= slot) {
            queues.reserve(slot + 1);
            for (size_t i = queues.size(); i <= slot; i++) {
                queues.emplace_back(unique_ptr<CmdQueueMetal>{nullptr});
            }
        }
        unique_ptr<CmdQueueMetal>& q = queues[slot];
        if (q == nullptr) {
            auto queue = _device->newCommandQueue();
            auto ins = make_unique<CmdQueueMetal>(this, NS::TransferPtr(queue));
            q = std::move(ins);
        }
        return q->IsValid() ? std::make_optional(q.get()) : std::nullopt;
    });
}

std::optional<shared_ptr<Shader>> DeviceMetal::CreateShader(
    std::span<const byte> blob,
    const ShaderReflection& refl,
    ShaderStage stage,
    std::string_view entryPoint,
    std::string_view name) noexcept {
    return AutoRelease([this, blob, &refl, stage, entryPoint, name]() noexcept -> std::optional<shared_ptr<Shader>> {
        if (!std::get_if<MslReflection>(&refl)) {
            RADRAY_ERR_LOG("metal can only use MSL");
            return std::nullopt;
        }
        auto mslStr = StringCppToNS(std::string_view{reinterpret_cast<const char*>(blob.data()), blob.size()})->autorelease();
        MTL::Library* lib;
        {
            auto co = MTL::CompileOptions::alloc()->autorelease();
            NS::Error* err{nullptr};
            lib = _device->newLibrary(mslStr, co, &err);
            if (err != nullptr) {
                err->autorelease();
                RADRAY_ERR_LOG("metal cannot new library\n{}", err->localizedDescription()->utf8String());
                return std::nullopt;
            }
        }
        lib->autorelease();
        MTL::Function* func;
        {
            auto entry = StringCppToNS(entryPoint)->autorelease();
            auto funcDesc = MTL::FunctionDescriptor::alloc()->autorelease()->init();
            funcDesc->setName(entry);
            NS::Error* err{nullptr};
            func = lib->newFunction(funcDesc, &err);
            if (err != nullptr) {
                err->autorelease();
                RADRAY_ERR_LOG("metal cannot new function\n{}", err->localizedDescription()->utf8String());
                return std::nullopt;
            }
        }
        RADRAY_INFO_LOG("metal function {}", func->name()->utf8String());
        func->setLabel(StringCppToNS(name)->autorelease());
        auto slm = make_shared<FunctionMetal>(
            NS::TransferPtr(func),
            name,
            entryPoint,
            stage);
        return slm;
    });
}

std::optional<shared_ptr<RootSignature>> DeviceMetal::CreateRootSignature(std::span<Shader*> shaders) noexcept {
    RADRAY_UNUSED(shaders);
    return AutoRelease([]() noexcept -> std::optional<shared_ptr<RootSignature>> {
        return std::make_shared<RootSigMetal>();
    });
}

std::optional<shared_ptr<GraphicsPipelineState>> DeviceMetal::CreateGraphicsPipeline(
    const GraphicsPipelineStateDescriptor& desc) noexcept {
    return AutoRelease([this, &desc]() noexcept -> std::optional<shared_ptr<GraphicsPipelineState>> {
        auto rpd = MTL::RenderPipelineDescriptor::alloc()->init()->autorelease();
        if (desc.Name.size() > 0) {
            rpd->setLabel(StringCppToNS(desc.Name)->autorelease());
        }
        rpd->setVertexFunction(static_cast<FunctionMetal*>(desc.VS)->_func.get());
        rpd->setFragmentFunction(static_cast<FunctionMetal*>(desc.PS)->_func.get());
        MTL::TriangleFillMode rawFillMode;
        if (auto fillMode = MapType(desc.Primitive.Poly);
            fillMode.has_value()) {
            rawFillMode = fillMode.value();
        } else {
            RADRAY_ERR_LOG("metal unsupported polygon mode {}", desc.Primitive.Poly);
            return std::nullopt;
        }
        auto&& [rawPrimClass, rawPrimType] = MapType(desc.Primitive.Topology);
        rpd->setInputPrimitiveTopology(rawPrimClass);
        for (size_t i = 0; i < desc.ColorTargets.size(); i++) {
            const ColorTargetState& cts = desc.ColorTargets[i];
            MTL::RenderPipelineColorAttachmentDescriptor* cad = rpd->colorAttachments()->object(i);
            if (cts.Format == TextureFormat::UNKNOWN) {
                cad->setPixelFormat(MTL::PixelFormatInvalid);
                continue;
            }
            MTL::PixelFormat rawFormat = MapType(cts.Format);
            cad->setPixelFormat(rawFormat);
            cad->setWriteMask(MapType(cts.WriteMask));
            if (cts.BlendEnable) {
                cad->setBlendingEnabled(true);
                auto [colorOp, colorSrc, colorDst] = MapType(cts.Blend.Color);
                auto [alphaOp, alphaSrc, alphaDst] = MapType(cts.Blend.Alpha);
                cad->setRgbBlendOperation(colorOp);
                cad->setSourceRGBBlendFactor(colorSrc);
                cad->setDestinationRGBBlendFactor(colorDst);
                cad->setAlphaBlendOperation(alphaOp);
                cad->setSourceAlphaBlendFactor(alphaSrc);
                cad->setDestinationAlphaBlendFactor(alphaDst);
            }
        }
        MTL::DepthStencilState* rawDepthStencil = nullptr;
        if (desc.DepthStencilEnable) {
            const DepthStencilState& ds = desc.DepthStencil;
            auto dsFmt = ds.Format;
            MTL::PixelFormat rawFormat = MapType(dsFmt);
            if (dsFmt == TextureFormat::S8) {
                rpd->setStencilAttachmentPixelFormat(rawFormat);
            } else if (dsFmt == TextureFormat::D16_UNORM || dsFmt == TextureFormat::D32_FLOAT) {
                rpd->setDepthAttachmentPixelFormat(rawFormat);
            } else if (dsFmt == TextureFormat::D24_UNORM_S8_UINT || dsFmt == TextureFormat::D32_FLOAT_S8_UINT) {
                rpd->setDepthAttachmentPixelFormat(rawFormat);
                rpd->setStencilAttachmentPixelFormat(rawFormat);
            } else {
                RADRAY_ERR_LOG("metal unsupported depth stencil format {}", dsFmt);
                return std::nullopt;
            }
            auto dsd = MTL::DepthStencilDescriptor::alloc()->init()->autorelease();
            dsd->setDepthCompareFunction(MapType(ds.DepthCompare));
            dsd->setDepthWriteEnabled(ds.DepthWriteEnable);
            if (ds.StencilEnable) {
                auto createStencilDesc = [](const StencilFaceState& sfs, uint32_t readMask, uint32_t writeMask) noexcept {
                    auto sd = MTL::StencilDescriptor::alloc()->init()->autorelease();
                    sd->setStencilCompareFunction(MapType(sfs.Compare));
                    sd->setReadMask(readMask);
                    sd->setWriteMask(writeMask);
                    sd->setStencilFailureOperation(MapType(sfs.FailOp));
                    sd->setDepthFailureOperation(MapType(sfs.DepthFailOp));
                    sd->setDepthStencilPassOperation(MapType(sfs.PassOp));
                    return sd;
                };
                dsd->setFrontFaceStencil(createStencilDesc(ds.Stencil.Front, ds.Stencil.ReadMask, ds.Stencil.WriteMask));
                dsd->setBackFaceStencil(createStencilDesc(ds.Stencil.Back, ds.Stencil.ReadMask, ds.Stencil.WriteMask));
            }
            rawDepthStencil = _device->newDepthStencilState(dsd);
        }
        if (desc.VertexBuffers.size() > 0) {
            auto vd = MTL::VertexDescriptor::alloc()->init()->autorelease();
            for (size_t i = 0; i < desc.VertexBuffers.size(); i++) {
                const VertexBufferLayout& vbl = desc.VertexBuffers[i];
                MTL::VertexBufferLayoutDescriptor* vbld = vd->layouts()->object(i);
                if (vbl.ArrayStride == 0) {
                    uint64_t stride = 0;
                    for (const VertexElement& j : vbl.Elements) {
                        stride = std::max(stride, j.Offset + GetVertexFormatSize(j.Format));
                    }
                    vbld->setStride(CalcAlign(stride, 4));
                    vbld->setStepFunction(MTL::VertexStepFunctionConstant);
                    vbld->setStepRate(0);
                } else {
                    vbld->setStride(vbl.ArrayStride);
                    vbld->setStepFunction(MapType(vbl.StepMode));
                }
                for (size_t j = 0; j < vbl.Elements.size(); j++) {
                    const VertexElement& ve = vbl.Elements[j];
                    MTL::VertexAttributeDescriptor* vad = vd->attributes()->object(ve.Location);
                    vad->setFormat(MapType(ve.Format));
                    vad->setBufferIndex(i);
                    vad->setOffset(ve.Offset);
                }
            }
            rpd->setVertexDescriptor(vd);
        }
        if (desc.MultiSample.Count != 1) {
            rpd->setSampleCount(desc.MultiSample.Count);
            rpd->setAlphaToCoverageEnabled(desc.MultiSample.AlphaToCoverageEnable);
        }
        NS::Error* err{nullptr};
        MTL::RenderPipelineState* pso = _device->newRenderPipelineState(rpd, &err);
        if (err != nullptr) {
            err->autorelease();
            RADRAY_ERR_LOG("metal cannot new render pipeline state\n{}", err->localizedDescription()->utf8String());
            return std::nullopt;
        }
        MTL::Winding rawWinding = MapType(desc.Primitive.FaceClockwise);
        MTL::CullMode rawCullMode = MapType(desc.Primitive.Cull);
        MTL::DepthClipMode rawDepthClip = desc.Primitive.UnclippedDepth ? MTL::DepthClipModeClamp : MTL::DepthClipModeClip;
        auto psoMetal = make_shared<RenderPipelineStateMetal>(
            NS::TransferPtr(pso),
            rawPrimType,
            rawFillMode,
            rawWinding,
            rawCullMode,
            rawDepthClip,
            NS::TransferPtr(rawDepthStencil),
            desc.DepthStencil.DepthBias);
        return std::static_pointer_cast<GraphicsPipelineState>(psoMetal);
    });
}

std::optional<shared_ptr<DeviceMetal>> CreateDevice(const MetalDeviceDescriptor& desc) noexcept {
    return AutoRelease([&desc]() noexcept -> std::optional<shared_ptr<DeviceMetal>> {
        MTL::Device* device;
        bool isMac;
#if defined(RADRAY_PLATFORM_MACOS)
        isMac = true;
#elif defined(RADRAY_PLATFORM_IOS)
        isMac = false;
#else
#error "unknown apple os. not macos or ios"
#endif
        if (isMac) {
            NS::Array* devices = MTL::CopyAllDevices()->autorelease();
            NS::UInteger count = devices->count();
            if (count == 0) {
                RADRAY_ERR_LOG("Metal cannot find any device");
                return std::nullopt;
            }
            for (NS::UInteger i = 0; i < count; i++) {
                auto d = devices->object<MTL::Device>(i);
                RADRAY_INFO_LOG("Metal find device: {}", d->name()->utf8String());
            }
            if (desc.DeviceIndex.has_value()) {
                uint32_t need = desc.DeviceIndex.value();
                if (need >= count) {
                    RADRAY_ERR_LOG("Metal device index out of range (count={}, need={})", count, need);
                    return std::nullopt;
                }
                device = devices->object<MTL::Device>(need);
            } else {
                device = devices->object<MTL::Device>(0);
            }
            device->retain();
        } else {
            device = MTL::CreateSystemDefaultDevice();
        }
        auto result = make_shared<DeviceMetal>(NS::TransferPtr(device));
        RADRAY_INFO_LOG("select device: {}", device->name()->utf8String());
        RADRAY_INFO_LOG("========== Feature ==========");
        NS::ProcessInfo* pi = NS::ProcessInfo::processInfo();
        NS::OperatingSystemVersion ver = pi->operatingSystemVersion();
        RADRAY_INFO_LOG("OS: {} {}.{}.{}", isMac ? "macOS" : "iOS", ver.majorVersion, ver.minorVersion, ver.patchVersion);
        {
            auto checkMaxFamily = [device](const MTL::GPUFamily* f, size_t fcnt) noexcept {
                std::optional<MTL::GPUFamily> now;
                for (size_t i = 0; i < fcnt; i++) {
                    bool isSupport = device->supportsFamily(f[i]);
                    if (!isSupport) {
                        break;
                    }
                    now = f[i];
                }
                return now;
            };
            std::optional<MTL::GPUFamily> family;
            if (isMac) {
                const MTL::GPUFamily intelMacos[] = {
                    MTL::GPUFamilyMac1,
                    MTL::GPUFamilyMac2};
                family = checkMaxFamily(intelMacos, ArrayLength(intelMacos));
                if (!family.has_value()) {
                    const MTL::GPUFamily mmacos[] = {
                        MTL::GPUFamilyApple7,
                        MTL::GPUFamilyApple8,
                        MTL::GPUFamilyApple9};
                    family = checkMaxFamily(mmacos, ArrayLength(mmacos));
                }
            } else {
                const MTL::GPUFamily ios[] = {
                    MTL::GPUFamilyApple1,
                    MTL::GPUFamilyApple2,
                    MTL::GPUFamilyApple3,
                    MTL::GPUFamilyApple4,
                    MTL::GPUFamilyApple5,
                    MTL::GPUFamilyApple6,
                    MTL::GPUFamilyApple7,
                    MTL::GPUFamilyApple8,
                    MTL::GPUFamilyApple9};
                family = checkMaxFamily(ios, ArrayLength(ios));
            }
            if (!family.has_value()) {
                RADRAY_ERR_LOG("device too old");
                return std::nullopt;
            }
            RADRAY_INFO_LOG("GPU Family: {}", family.value());
        }
        {
            RADRAY_INFO_LOG("Support Metal 3: {}", device->supportsFamily(MTL::GPUFamilyMetal3));
        }
        {
            auto o = MTL::CompileOptions::alloc()->init()->autorelease();
            RADRAY_INFO_LOG("MSL version: {}", o->languageVersion());
        }
        device->supportsFamily(MTL::GPUFamilyApple1);
        RADRAY_INFO_LOG("=============================");
        return result;
    });
}

}  // namespace radray::render::metal
