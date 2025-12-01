#include <radray/render/spvc.h>

#ifdef RADRAY_ENABLE_SPIRV_CROSS

#include <spirv_cross.hpp>

#include <radray/errors.h>
#include <radray/logger.h>
#include <radray/utility.h>

namespace radray::render {

static spv::ExecutionModel _MapType(ShaderStage stage) noexcept {
    switch (stage) {
        case ShaderStage::UNKNOWN: return spv::ExecutionModel::ExecutionModelMax;
        case ShaderStage::Vertex: return spv::ExecutionModel::ExecutionModelVertex;
        case ShaderStage::Pixel: return spv::ExecutionModel::ExecutionModelFragment;
        case ShaderStage::Compute: return spv::ExecutionModel::ExecutionModelGLCompute;
        case ShaderStage::Graphics: return spv::ExecutionModel::ExecutionModelMax;
    }
    Unreachable();
}

std::optional<SpirvShaderDesc> ReflectSpirv(std::string_view entryPointName, ShaderStage stage, std::span<const byte> data) {
    if (data.size() % 4 != 0) {
        RADRAY_ERR_LOG("{} {}", Errors::SPIRV_CROSS, "Invalid SPIR-V data size");
        return std::nullopt;
    }
    SpirvShaderDesc desc;
    try {
        spirv_cross::Compiler compiler{std::bit_cast<const uint32_t*>(data.data()), data.size() / sizeof(uint32_t)};

        compiler.get_shader_resources();

        // std::string epStr{entryPointName};
        // spv::ExecutionModel exeModel = _MapType(stage);
        // const auto& entryPoint = compiler.get_entry_point(epStr, exeModel);
        // desc.workgroupSize

    } catch (const spirv_cross::CompilerError& e) {
        RADRAY_ERR_LOG("{} {}: {}", Errors::SPIRV_CROSS, "Compiler Error", e.what());
        return std::nullopt;
    } catch (const std::exception& e) {
        RADRAY_ERR_LOG("{} {}", Errors::SPIRV_CROSS, e.what());
        return std::nullopt;
    } catch (...) {
        RADRAY_ERR_LOG("{} {}", Errors::SPIRV_CROSS, "unknown error");
        return std::nullopt;
    }
    return desc;
}

}  // namespace radray::render

#endif
