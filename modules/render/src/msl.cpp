#include <radray/render/msl.h>

#include <radray/utility.h>

namespace radray::render {

std::string_view format_as(MslDataType v) noexcept {
    switch (v) {
        case MslDataType::None: return "None";
        case MslDataType::Struct: return "Struct";
        case MslDataType::Array: return "Array";
        case MslDataType::Float: return "Float";
        case MslDataType::Float2: return "Float2";
        case MslDataType::Float3: return "Float3";
        case MslDataType::Float4: return "Float4";
        case MslDataType::Float2x2: return "Float2x2";
        case MslDataType::Float2x3: return "Float2x3";
        case MslDataType::Float2x4: return "Float2x4";
        case MslDataType::Float3x2: return "Float3x2";
        case MslDataType::Float3x3: return "Float3x3";
        case MslDataType::Float3x4: return "Float3x4";
        case MslDataType::Float4x2: return "Float4x2";
        case MslDataType::Float4x3: return "Float4x3";
        case MslDataType::Float4x4: return "Float4x4";
        case MslDataType::Half: return "Half";
        case MslDataType::Half2: return "Half2";
        case MslDataType::Half3: return "Half3";
        case MslDataType::Half4: return "Half4";
        case MslDataType::Half2x2: return "Half2x2";
        case MslDataType::Half2x3: return "Half2x3";
        case MslDataType::Half2x4: return "Half2x4";
        case MslDataType::Half3x2: return "Half3x2";
        case MslDataType::Half3x3: return "Half3x3";
        case MslDataType::Half3x4: return "Half3x4";
        case MslDataType::Half4x2: return "Half4x2";
        case MslDataType::Half4x3: return "Half4x3";
        case MslDataType::Half4x4: return "Half4x4";
        case MslDataType::Int: return "Int";
        case MslDataType::Int2: return "Int2";
        case MslDataType::Int3: return "Int3";
        case MslDataType::Int4: return "Int4";
        case MslDataType::UInt: return "UInt";
        case MslDataType::UInt2: return "UInt2";
        case MslDataType::UInt3: return "UInt3";
        case MslDataType::UInt4: return "UInt4";
        case MslDataType::Short: return "Short";
        case MslDataType::Short2: return "Short2";
        case MslDataType::Short3: return "Short3";
        case MslDataType::Short4: return "Short4";
        case MslDataType::UShort: return "UShort";
        case MslDataType::UShort2: return "UShort2";
        case MslDataType::UShort3: return "UShort3";
        case MslDataType::UShort4: return "UShort4";
        case MslDataType::Char: return "Char";
        case MslDataType::Char2: return "Char2";
        case MslDataType::Char3: return "Char3";
        case MslDataType::Char4: return "Char4";
        case MslDataType::UChar: return "UChar";
        case MslDataType::UChar2: return "UChar2";
        case MslDataType::UChar3: return "UChar3";
        case MslDataType::UChar4: return "UChar4";
        case MslDataType::Bool: return "Bool";
        case MslDataType::Bool2: return "Bool2";
        case MslDataType::Bool3: return "Bool3";
        case MslDataType::Bool4: return "Bool4";
        case MslDataType::Long: return "Long";
        case MslDataType::Long2: return "Long2";
        case MslDataType::Long3: return "Long3";
        case MslDataType::Long4: return "Long4";
        case MslDataType::ULong: return "ULong";
        case MslDataType::ULong2: return "ULong2";
        case MslDataType::ULong3: return "ULong3";
        case MslDataType::ULong4: return "ULong4";
        case MslDataType::Texture: return "Texture";
        case MslDataType::Sampler: return "Sampler";
        case MslDataType::Pointer: return "Pointer";
    }
    radray::Unreachable();
}

std::string_view format_as(MslArgumentType v) noexcept {
    switch (v) {
        case MslArgumentType::Buffer: return "Buffer";
        case MslArgumentType::Texture: return "Texture";
        case MslArgumentType::Sampler: return "Sampler";
        case MslArgumentType::ThreadgroupMemory: return "ThreadgroupMemory";
    }
    radray::Unreachable();
}

std::string_view format_as(MslAccess v) noexcept {
    switch (v) {
        case MslAccess::ReadOnly: return "ReadOnly";
        case MslAccess::ReadWrite: return "ReadWrite";
        case MslAccess::WriteOnly: return "WriteOnly";
    }
    radray::Unreachable();
}

std::string_view format_as(MslTextureType v) noexcept {
    switch (v) {
        case MslTextureType::Tex1D: return "Tex1D";
        case MslTextureType::Tex1DArray: return "Tex1DArray";
        case MslTextureType::Tex2D: return "Tex2D";
        case MslTextureType::Tex2DArray: return "Tex2DArray";
        case MslTextureType::Tex2DMS: return "Tex2DMS";
        case MslTextureType::Tex3D: return "Tex3D";
        case MslTextureType::TexCube: return "TexCube";
        case MslTextureType::TexCubeArray: return "TexCubeArray";
        case MslTextureType::TexBuffer: return "TexBuffer";
    }
    radray::Unreachable();
}

std::string_view format_as(MslStage v) noexcept {
    switch (v) {
        case MslStage::Vertex: return "Vertex";
        case MslStage::Fragment: return "Fragment";
        case MslStage::Compute: return "Compute";
    }
    radray::Unreachable();
}

}  // namespace radray::render

#ifdef RADRAY_ENABLE_SPIRV_CROSS

#include <bit>

#include <spirv_cross.hpp>
#include <spirv_msl.hpp>
#include <radray/logger.h>

namespace radray::render {

static MslStage _MapShaderStageToMslStage(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Vertex: return MslStage::Vertex;
        case ShaderStage::Pixel: return MslStage::Fragment;
        default: return MslStage::Compute;
    }
}

static spv::ExecutionModel _MapStageToExecModel(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Vertex: return spv::ExecutionModelVertex;
        case ShaderStage::Pixel: return spv::ExecutionModelFragment;
        case ShaderStage::Compute: return spv::ExecutionModelGLCompute;
        default: return spv::ExecutionModelMax;
    }
}

static MslDataType _MapSpirvToMslDataType(const spirv_cross::SPIRType& type) {
    uint32_t vec = type.vecsize;
    uint32_t col = type.columns;
    switch (type.basetype) {
        case spirv_cross::SPIRType::Float:
            if (col == 1) {
                if (vec == 1) return MslDataType::Float;
                if (vec == 2) return MslDataType::Float2;
                if (vec == 3) return MslDataType::Float3;
                if (vec == 4) return MslDataType::Float4;
            }
            if (col == 2) {
                if (vec == 2) return MslDataType::Float2x2;
                if (vec == 3) return MslDataType::Float2x3;
                if (vec == 4) return MslDataType::Float2x4;
            }
            if (col == 3) {
                if (vec == 2) return MslDataType::Float3x2;
                if (vec == 3) return MslDataType::Float3x3;
                if (vec == 4) return MslDataType::Float3x4;
            }
            if (col == 4) {
                if (vec == 2) return MslDataType::Float4x2;
                if (vec == 3) return MslDataType::Float4x3;
                if (vec == 4) return MslDataType::Float4x4;
            }
            break;
        case spirv_cross::SPIRType::Half:
            if (col == 1) {
                if (vec == 1) return MslDataType::Half;
                if (vec == 2) return MslDataType::Half2;
                if (vec == 3) return MslDataType::Half3;
                if (vec == 4) return MslDataType::Half4;
            }
            if (col == 2) {
                if (vec == 2) return MslDataType::Half2x2;
                if (vec == 3) return MslDataType::Half2x3;
                if (vec == 4) return MslDataType::Half2x4;
            }
            if (col == 3) {
                if (vec == 2) return MslDataType::Half3x2;
                if (vec == 3) return MslDataType::Half3x3;
                if (vec == 4) return MslDataType::Half3x4;
            }
            if (col == 4) {
                if (vec == 2) return MslDataType::Half4x2;
                if (vec == 3) return MslDataType::Half4x3;
                if (vec == 4) return MslDataType::Half4x4;
            }
            break;
        case spirv_cross::SPIRType::Int:
            if (vec == 1) return MslDataType::Int;
            if (vec == 2) return MslDataType::Int2;
            if (vec == 3) return MslDataType::Int3;
            if (vec == 4) return MslDataType::Int4;
            break;
        case spirv_cross::SPIRType::UInt:
            if (vec == 1) return MslDataType::UInt;
            if (vec == 2) return MslDataType::UInt2;
            if (vec == 3) return MslDataType::UInt3;
            if (vec == 4) return MslDataType::UInt4;
            break;
        case spirv_cross::SPIRType::Short:
            if (vec == 1) return MslDataType::Short;
            if (vec == 2) return MslDataType::Short2;
            if (vec == 3) return MslDataType::Short3;
            if (vec == 4) return MslDataType::Short4;
            break;
        case spirv_cross::SPIRType::UShort:
            if (vec == 1) return MslDataType::UShort;
            if (vec == 2) return MslDataType::UShort2;
            if (vec == 3) return MslDataType::UShort3;
            if (vec == 4) return MslDataType::UShort4;
            break;
        case spirv_cross::SPIRType::SByte:
            if (vec == 1) return MslDataType::Char;
            if (vec == 2) return MslDataType::Char2;
            if (vec == 3) return MslDataType::Char3;
            if (vec == 4) return MslDataType::Char4;
            break;
        case spirv_cross::SPIRType::UByte:
            if (vec == 1) return MslDataType::UChar;
            if (vec == 2) return MslDataType::UChar2;
            if (vec == 3) return MslDataType::UChar3;
            if (vec == 4) return MslDataType::UChar4;
            break;
        case spirv_cross::SPIRType::Boolean:
            if (vec == 1) return MslDataType::Bool;
            if (vec == 2) return MslDataType::Bool2;
            if (vec == 3) return MslDataType::Bool3;
            if (vec == 4) return MslDataType::Bool4;
            break;
        case spirv_cross::SPIRType::Int64:
            if (vec == 1) return MslDataType::Long;
            if (vec == 2) return MslDataType::Long2;
            if (vec == 3) return MslDataType::Long3;
            if (vec == 4) return MslDataType::Long4;
            break;
        case spirv_cross::SPIRType::UInt64:
            if (vec == 1) return MslDataType::ULong;
            if (vec == 2) return MslDataType::ULong2;
            if (vec == 3) return MslDataType::ULong3;
            if (vec == 4) return MslDataType::ULong4;
            break;
        case spirv_cross::SPIRType::Struct:
            return MslDataType::Struct;
        case spirv_cross::SPIRType::Image:
        case spirv_cross::SPIRType::SampledImage:
            return MslDataType::Texture;
        case spirv_cross::SPIRType::Sampler:
            return MslDataType::Sampler;
        default:
            break;
    }
    return MslDataType::None;
}

static MslTextureType _MapImageToMslTextureType(const spirv_cross::SPIRType& type) {
    switch (type.image.dim) {
        case spv::Dim1D:
            return type.image.arrayed ? MslTextureType::Tex1DArray : MslTextureType::Tex1D;
        case spv::Dim2D:
            if (type.image.ms) return MslTextureType::Tex2DMS;
            return type.image.arrayed ? MslTextureType::Tex2DArray : MslTextureType::Tex2D;
        case spv::Dim3D:
            return MslTextureType::Tex3D;
        case spv::DimCube:
            return type.image.arrayed ? MslTextureType::TexCubeArray : MslTextureType::TexCube;
        case spv::DimBuffer:
            return MslTextureType::TexBuffer;
        default:
            return MslTextureType::Tex2D;
    }
}

struct MslReflCtx {
    const spirv_cross::CompilerMSL& compiler;
    MslShaderReflection& refl;
    unordered_map<uint32_t, uint32_t>& structCache;
};

static uint32_t _ReflectMslArrayType(MslReflCtx& ctx, const spirv_cross::SPIRType& type);

static uint32_t _ReflectMslStructType(MslReflCtx& ctx, const spirv_cross::SPIRType& type) {
    auto it = ctx.structCache.find(type.self);
    if (it != ctx.structCache.end()) return it->second;
    uint32_t idx = static_cast<uint32_t>(ctx.refl.StructTypes.size());
    ctx.structCache[type.self] = idx;
    ctx.refl.StructTypes.emplace_back();
    MslStructType st{};
    for (uint32_t i = 0; i < type.member_types.size(); i++) {
        MslStructMember member{};
        std::string name = ctx.compiler.get_member_name(type.self, i);
        if (name.empty()) {
            name = "member_" + std::to_string(i);
        }
        member.Name = string(name);
        member.Offset = ctx.compiler.type_struct_member_offset(type, i);
        const auto& memberType = ctx.compiler.get_type(type.member_types[i]);
        if (!memberType.array.empty()) {
            member.DataType = MslDataType::Array;
            member.ArrayTypeIndex = _ReflectMslArrayType(ctx, memberType);
        } else if (memberType.basetype == spirv_cross::SPIRType::Struct) {
            member.DataType = MslDataType::Struct;
            member.StructTypeIndex = _ReflectMslStructType(ctx, memberType);
        } else {
            member.DataType = _MapSpirvToMslDataType(memberType);
        }
        st.Members.push_back(std::move(member));
    }
    ctx.refl.StructTypes[idx] = std::move(st);
    return idx;
}

static uint32_t _ReflectMslArrayType(MslReflCtx& ctx, const spirv_cross::SPIRType& type) {
    uint32_t idx = static_cast<uint32_t>(ctx.refl.ArrayTypes.size());
    ctx.refl.ArrayTypes.emplace_back();
    MslArrayType at{};
    at.ArrayLength = type.array.empty() ? 0 : type.array[0];
    at.Stride = ctx.compiler.get_decoration(type.self, spv::DecorationArrayStride);
    const auto& elemType = ctx.compiler.get_type(type.parent_type);
    if (elemType.basetype == spirv_cross::SPIRType::Struct) {
        at.ElementType = MslDataType::Struct;
        at.ElementStructTypeIndex = _ReflectMslStructType(ctx, elemType);
    } else if (!elemType.array.empty()) {
        at.ElementType = MslDataType::Array;
        at.ElementArrayTypeIndex = _ReflectMslArrayType(ctx, elemType);
    } else {
        at.ElementType = _MapSpirvToMslDataType(elemType);
    }
    ctx.refl.ArrayTypes[idx] = std::move(at);
    return idx;
}

static void _ProcessMslBinding(
    MslReflCtx& ctx,
    const spirv_cross::Resource& res,
    MslArgumentType argType,
    MslStage stage,
    bool isReadOnly,
    bool isWriteOnly) {
    auto& compiler = ctx.compiler;
    MslArgument arg{};
    arg.Name = compiler.get_name(res.id);
    if (arg.Name.empty()) {
        arg.Name = string(res.name);
    }
    arg.Stage = stage;
    arg.Type = argType;
    arg.IsActive = true;
    uint32_t mslBinding = compiler.get_automatic_msl_resource_binding(res.id);
    arg.Index = (mslBinding != ~0u) ? mslBinding : 0;
    const auto& type = compiler.get_type(res.type_id);
    if (!type.array.empty()) {
        arg.ArrayLength = type.array[0];
    }
    if (argType == MslArgumentType::Buffer) {
        if (isReadOnly) arg.Access = MslAccess::ReadOnly;
        else if (isWriteOnly) arg.Access = MslAccess::WriteOnly;
        else arg.Access = MslAccess::ReadWrite;
        const auto& baseType = compiler.get_type(res.base_type_id);
        if (baseType.basetype == spirv_cross::SPIRType::Struct) {
            arg.BufferDataType = MslDataType::Struct;
            arg.BufferDataSize = compiler.get_declared_struct_size(baseType);
            arg.BufferStructTypeIndex = _ReflectMslStructType(ctx, baseType);
        } else {
            arg.BufferDataType = _MapSpirvToMslDataType(baseType);
        }
    } else if (argType == MslArgumentType::Texture) {
        if (isReadOnly) arg.Access = MslAccess::ReadOnly;
        else if (isWriteOnly) arg.Access = MslAccess::WriteOnly;
        else arg.Access = MslAccess::ReadWrite;
        arg.TextureType = _MapImageToMslTextureType(type);
        arg.IsDepthTexture = type.image.depth;
        const auto& sampledType = compiler.get_type(type.image.type);
        arg.TextureDataType = _MapSpirvToMslDataType(sampledType);
    }
    ctx.refl.Arguments.push_back(std::move(arg));
}

std::optional<MslShaderReflection> ReflectMsl(std::span<const MslReflectParams> msls) {
    MslShaderReflection refl{};
    for (const auto& msl : msls) {
        if (msl.SpirV.size() % 4 != 0) {
            RADRAY_ERR_LOG("invalid SPIR-V data size, not multiple of 4 bytes");
            return std::nullopt;
        }
        auto execModel = _MapStageToExecModel(msl.Stage);
        if (execModel == spv::ExecutionModelMax) {
            RADRAY_ERR_LOG("unsupported shader stage for MSL reflection");
            return std::nullopt;
        }
        MslStage mslStage = _MapShaderStageToMslStage(msl.Stage);
        unordered_map<uint32_t, uint32_t> structCache;
        try {
            spirv_cross::CompilerMSL compiler{
                std::bit_cast<const uint32_t*>(msl.SpirV.data()),
                msl.SpirV.size() / sizeof(uint32_t)};
            spirv_cross::CompilerMSL::Options mslOpt;
            mslOpt.set_msl_version(2, 0, 0);
            mslOpt.platform = spirv_cross::CompilerMSL::Options::macOS;
            compiler.set_msl_options(mslOpt);
            compiler.set_entry_point(string(msl.EntryPoint), execModel);
            spirv_cross::ShaderResources resources = compiler.get_shader_resources();
            // 为 vertex/fragment buffer 添加偏移，与 ConvertSpirvToMsl 保持一致
            if (msl.Stage == ShaderStage::Vertex || msl.Stage == ShaderStage::Pixel) {
                auto addBufferBinding = [&](const spirv_cross::Resource& res) {
                    uint32_t descSet = compiler.get_decoration(res.id, spv::DecorationDescriptorSet);
                    uint32_t binding = compiler.get_decoration(res.id, spv::DecorationBinding);
                    spirv_cross::MSLResourceBinding b{};
                    b.stage = execModel;
                    b.desc_set = descSet;
                    b.binding = binding;
                    b.msl_buffer = binding + MetalMaxVertexInputBindings;
                    b.msl_texture = binding;
                    b.msl_sampler = binding;
                    compiler.add_msl_resource_binding(b);
                };
                for (const auto& r : resources.uniform_buffers) addBufferBinding(r);
                for (const auto& r : resources.storage_buffers) addBufferBinding(r);
                if (!resources.push_constant_buffers.empty()) {
                    spirv_cross::MSLResourceBinding pcb{};
                    pcb.stage = execModel;
                    pcb.desc_set = spirv_cross::kPushConstDescSet;
                    pcb.binding = spirv_cross::kPushConstBinding;
                    pcb.msl_buffer = MetalMaxVertexInputBindings;
                    compiler.add_msl_resource_binding(pcb);
                }
            }
            // 编译触发 binding 分配
            compiler.compile();
            MslReflCtx ctx{compiler, refl, structCache};
            for (const auto& r : resources.uniform_buffers) {
                _ProcessMslBinding(ctx, r, MslArgumentType::Buffer, mslStage, true, false);
            }
            for (const auto& r : resources.storage_buffers) {
                bool ro = compiler.get_decoration(r.id, spv::DecorationNonWritable) != 0;
                bool wo = compiler.get_decoration(r.id, spv::DecorationNonReadable) != 0;
                _ProcessMslBinding(ctx, r, MslArgumentType::Buffer, mslStage, ro, wo);
            }
            for (const auto& r : resources.push_constant_buffers) {
                _ProcessMslBinding(ctx, r, MslArgumentType::Buffer, mslStage, true, false);
            }
            for (const auto& r : resources.sampled_images) {
                _ProcessMslBinding(ctx, r, MslArgumentType::Texture, mslStage, true, false);
            }
            for (const auto& r : resources.separate_images) {
                _ProcessMslBinding(ctx, r, MslArgumentType::Texture, mslStage, true, false);
            }
            for (const auto& r : resources.storage_images) {
                bool ro = compiler.get_decoration(r.id, spv::DecorationNonWritable) != 0;
                bool wo = compiler.get_decoration(r.id, spv::DecorationNonReadable) != 0;
                _ProcessMslBinding(ctx, r, MslArgumentType::Texture, mslStage, ro, wo);
            }
            for (const auto& r : resources.separate_samplers) {
                _ProcessMslBinding(ctx, r, MslArgumentType::Sampler, mslStage, true, false);
            }
        } catch (const spirv_cross::CompilerError& e) {
            RADRAY_ERR_LOG("SPIRV-Cross MSL Reflect Error: {}", e.what());
            return std::nullopt;
        } catch (const std::exception& e) {
            RADRAY_ERR_LOG("SPIRV-Cross MSL Reflect error: {}", e.what());
            return std::nullopt;
        } catch (...) {
            RADRAY_ERR_LOG("SPIRV-Cross MSL Reflect error: unknown error");
            return std::nullopt;
        }
    }
    return refl;
}

}  // namespace radray::render

#endif
