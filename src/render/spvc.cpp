#include <radray/render/spvc.h>

#ifdef RADRAY_ENABLE_SPIRV_CROSS

#include <spirv_cross/spirv_msl.hpp>
#include <spirv_cross/spirv_reflect.hpp>

#include <radray/logger.h>
#include <radray/utility.h>

namespace spv {
std::string_view format_as(ExecutionModel v) noexcept {
    switch (v) {
        case ExecutionModelVertex: return "Vertex";
        case ExecutionModelTessellationControl: return "TessellationControl";
        case ExecutionModelTessellationEvaluation: return "TessellationEvaluation";
        case ExecutionModelGeometry: return "Geometry";
        case ExecutionModelFragment: return "Fragment";
        case ExecutionModelGLCompute: return "GLCompute";
        case ExecutionModelKernel: return "Kernel";
        case ExecutionModelTaskNV: return "TaskNV";
        case ExecutionModelMeshNV: return "MeshNV";
        case ExecutionModelRayGenerationKHR: return "RayGenerationKHR";
        case ExecutionModelIntersectionKHR: return "IntersectionKHR";
        case ExecutionModelAnyHitKHR: return "AnyHitKHR";
        case ExecutionModelClosestHitKHR: return "ClosestHitKHR";
        case ExecutionModelMissKHR: return "MissKHR";
        case ExecutionModelCallableKHR: return "CallableKHR";
        case ExecutionModelTaskEXT: return "TaskEXT";
        case ExecutionModelMeshEXT: return "MeshEXT";
        case ExecutionModelMax: return "Max";
    }
    radray::Unreachable();
}
}  // namespace spv

namespace spirv_cross {
std::string_view format_as(SPIRType::BaseType v) noexcept {
    switch (v) {
        case SPIRType::Unknown: return "UNKNOWN";
        case SPIRType::Void: return "Void";
        case SPIRType::Boolean: return "Boolean";
        case SPIRType::SByte: return "SByte";
        case SPIRType::UByte: return "UByte";
        case SPIRType::Short: return "Short";
        case SPIRType::UShort: return "UShort";
        case SPIRType::Int: return "Int";
        case SPIRType::UInt: return "UInt";
        case SPIRType::Int64: return "Int64";
        case SPIRType::UInt64: return "UInt64";
        case SPIRType::AtomicCounter: return "AtomicCounter";
        case SPIRType::Half: return "Half";
        case SPIRType::Float: return "Float";
        case SPIRType::Double: return "Double";
        case SPIRType::Struct: return "Struct";
        case SPIRType::Image: return "Image";
        case SPIRType::SampledImage: return "SampledImage";
        case SPIRType::Sampler: return "Sampler";
        case SPIRType::AccelerationStructure: return "AccelerationStructure";
        case SPIRType::RayQuery: return "RayQuery";
        case SPIRType::ControlPointArray: return "ControlPointArray";
        case SPIRType::Interpolant: return "Interpolant";
        case SPIRType::Char: return "Char";
    }
    radray::Unreachable();
}
}  // namespace spirv_cross

namespace radray::render {

static spirv_cross::CompilerMSL::Options::Platform EnumConvertSR(MslPlatform plat) noexcept {
    switch (plat) {
        case MslPlatform::Macos: return spirv_cross::CompilerMSL::Options::Platform::macOS;
        case MslPlatform::Ios: return spirv_cross::CompilerMSL::Options::Platform::iOS;
    }
    radray::Unreachable();
}

static std::optional<ShaderStage> EnumConvertRS(spv::ExecutionModel v) noexcept {
    switch (v) {
        case spv::ExecutionModelVertex: return ShaderStage::Vertex;
        case spv::ExecutionModelFragment: return ShaderStage::Pixel;
        case spv::ExecutionModelKernel: return ShaderStage::Compute;
        default: return std::nullopt;
    }
}

std::pair<uint32_t, uint32_t> GetMslVersionNumber(MslVersion ver) noexcept {
    switch (ver) {
        case MslVersion::MSL11: return {1, 1};
        case MslVersion::MSL12: return {1, 2};
        case MslVersion::MSL20: return {2, 0};
        case MslVersion::MSL21: return {2, 1};
        case MslVersion::MSL22: return {2, 2};
        case MslVersion::MSL23: return {2, 3};
        case MslVersion::MSL24: return {2, 4};
        case MslVersion::MSL30: return {3, 0};
        case MslVersion::MSL31: return {3, 1};
        case MslVersion::MSL32: return {3, 2};
    }
    radray::Unreachable();
}

std::optional<SpvcMslOutput> SpirvToMsl(
    std::span<byte> spirv,
    MslVersion ver,
    MslPlatform plat) {
    auto dword = ByteToDWORD({reinterpret_cast<uint8_t*>(spirv.data()), spirv.size()});
    auto [verMaj, verMin] = GetMslVersionNumber(ver);
    try {
        string msl;
        vector<SpvcEntryPoint> ep;
        {
            spirv_cross::CompilerMSL mslc{dword.data(), dword.size()};
            {
                auto opts = mslc.get_msl_options();
                opts.set_msl_version(verMaj, verMin);
                opts.platform = EnumConvertSR(plat);
                opts.invariant_float_math = true;
                mslc.set_msl_options(opts);
            }
            msl = string{mslc.compile()};
            ep.reserve(mslc.get_entry_points_and_stages().size());
            for (const auto& i : mslc.get_entry_points_and_stages()) {
                const auto& spvEP = mslc.get_entry_point(i.name, i.execution_model);
                auto stage = EnumConvertRS(spvEP.model);
                if (!stage.has_value()) {
                    RADRAY_ERR_LOG("cannot convert SPIR-V execution model to stage {}", spvEP.model);
                    return std::nullopt;
                }
                ep.emplace_back(SpvcEntryPoint{string{spvEP.name}, stage.value()});
                // spirv_cross::ShaderResources res = mslc.get_shader_resources(mslc.get_active_interface_variables());
                // for (const auto& j : res.uniform_buffers) {
                //     auto set = mslc.get_decoration(j.id, spv::DecorationDescriptorSet);
                //     auto binding = mslc.get_decoration(j.id, spv::DecorationBinding);
                //     auto loc = mslc.get_decoration(j.id, spv::DecorationLocation);
                //     auto t = mslc.get_type(j.type_id);
                //     RADRAY_INFO_LOG("uniform buffer: {}", j.name);
                //     RADRAY_INFO_LOG("- set={}", set);
                //     RADRAY_INFO_LOG("- bind={}", binding);
                //     RADRAY_INFO_LOG("- location={}", loc);
                // }
                // for (const auto& j : res.push_constant_buffers) {
                //     auto set = mslc.get_decoration(j.id, spv::DecorationDescriptorSet);
                //     auto binding = mslc.get_decoration(j.id, spv::DecorationBinding);
                //     auto loc = mslc.get_decoration(j.id, spv::DecorationLocation);
                //     RADRAY_INFO_LOG("push constant: {}\tset={} bind={} loc={}", j.name, set, binding, loc);
                // }
                // for (const auto& j : res.separate_images) {
                //     auto set = mslc.get_decoration(j.id, spv::DecorationDescriptorSet);
                //     auto binding = mslc.get_decoration(j.id, spv::DecorationBinding);
                //     auto loc = mslc.get_decoration(j.id, spv::DecorationLocation);
                //     RADRAY_INFO_LOG("separate images: {}\tset={} bind={} loc={}", j.name, set, binding, loc);
                // }
                // for (const auto& j : res.storage_images) {
                //     auto set = mslc.get_decoration(j.id, spv::DecorationDescriptorSet);
                //     auto binding = mslc.get_decoration(j.id, spv::DecorationBinding);
                //     auto loc = mslc.get_decoration(j.id, spv::DecorationLocation);
                //     RADRAY_INFO_LOG("storage images: {}\tset={} bind={} loc={}", j.name, set, binding, loc);
                // }
                // for (const auto& j : res.separate_samplers) {
                //     auto set = mslc.get_decoration(j.id, spv::DecorationDescriptorSet);
                //     auto binding = mslc.get_decoration(j.id, spv::DecorationBinding);
                //     auto loc = mslc.get_decoration(j.id, spv::DecorationLocation);
                //     RADRAY_INFO_LOG("separate samplers: {}\tset={} bind={} loc={}", j.name, set, binding, loc);
                // }
                // for (const auto& j : res.storage_buffers) {
                //     auto set = mslc.get_decoration(j.id, spv::DecorationDescriptorSet);
                //     auto binding = mslc.get_decoration(j.id, spv::DecorationBinding);
                //     auto loc = mslc.get_decoration(j.id, spv::DecorationLocation);
                //     RADRAY_INFO_LOG("storage buffers: {}\tset={} bind={} loc={}", j.name, set, binding, loc);
                // }
            }
            spirv_cross::ShaderResources res = mslc.get_shader_resources(mslc.get_active_interface_variables());
            for (const auto& j : res.stage_inputs) {
                auto loc = mslc.get_decoration(j.id, spv::DecorationLocation);
                auto t = mslc.get_type(j.type_id);
                RADRAY_INFO_LOG("stage input: {}", j.name);
                RADRAY_INFO_LOG("- location={}", loc);
                RADRAY_INFO_LOG("- type={}{}", spirv_cross::format_as(t.basetype), t.vecsize);
            }
        }
        return SpvcMslOutput{msl, ep};
    } catch (const std::exception& e) {
        RADRAY_ERR_LOG("cannot convert SPIR-V to MSL\n{}", e.what());
        return std::nullopt;
    }
}

}  // namespace radray::render

#endif
