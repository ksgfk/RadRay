#include "metal_device.h"

#include "metal_shader_lib.h"

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
                queues.emplace_back(radray::unique_ptr<CmdQueueMetal>{nullptr});
            }
        }
        radray::unique_ptr<CmdQueueMetal>& q = queues[slot];
        if (q == nullptr) {
            auto queue = _device->newCommandQueue();
            auto ins = radray::make_unique<CmdQueueMetal>(this, NS::TransferPtr(queue));
            q = std::move(ins);
        }
        return q->IsValid() ? std::make_optional(q.get()) : std::nullopt;
    });
}

std::optional<radray::shared_ptr<Shader>> DeviceMetal::CreateShader(
    std::span<const byte> blob,
    ShaderBlobCategory category,
    ShaderStage stage,
    std::string_view entryPoint,
    std::string_view name) noexcept {
    return AutoRelease([this, blob, category, stage, entryPoint, name]() noexcept -> std::optional<radray::shared_ptr<Shader>> {
        if (category != ShaderBlobCategory::MSL) {
            RADRAY_ERR_LOG("metal can only use MSL, not {}", category);
            return std::nullopt;
        }
        auto mslStr = NSStringInit(
            NS::String::alloc()->autorelease(),
            reinterpret_cast<const void*>(blob.data()),
            blob.size(),
            NS::StringEncoding::UTF8StringEncoding);
        auto co = MTL::CompileOptions::alloc()->autorelease();
        NS::Error* err{nullptr};
        MTL::Library* lib = _device->newLibrary(mslStr, co, &err);
        if (err != nullptr) {
            err->autorelease();
            RADRAY_ERR_LOG("metal cannot new library", err->localizedDescription()->utf8String());
            return std::nullopt;
        }
        auto slm = radray::make_shared<ShaderLibMetal>(
            NS::TransferPtr(lib),
            name,
            entryPoint,
            stage);
        return slm;
    });
}

std::optional<radray::shared_ptr<RootSignature>> DeviceMetal::CreateRootSignature(
    std::span<Shader*> shaders,
    std::span<SamplerDescriptor> staticSamplers,
    std::span<std::string_view> pushConstants) noexcept {
    return AutoRelease([shaders, staticSamplers, pushConstants]() noexcept -> std::optional<radray::shared_ptr<RootSignature>> {
        // for (auto shader : shaders) {
        //     ShaderLibMetal* lib = static_cast<ShaderLibMetal*>(shader);
        // }
        return std::nullopt;
    });
}

std::optional<radray::shared_ptr<DeviceMetal>> CreateDevice(const MetalDeviceDescriptor& desc) noexcept {
    return AutoRelease([&desc]() noexcept -> std::optional<radray::shared_ptr<DeviceMetal>> {
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
        auto result = radray::make_shared<DeviceMetal>(NS::TransferPtr(device));
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
