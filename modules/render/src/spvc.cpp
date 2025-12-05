#include <radray/render/spvc.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>

#include <radray/errors.h>
#include <radray/logger.h>
#include <radray/utility.h>

namespace radray::render {

const SpirvTypeInfo* SpirvShaderDesc::GetType(uint32_t index) const {
    if (index < Types.size()) {
        return &Types[index];
    }
    return nullptr;
}

}  // namespace radray::render

#ifdef RADRAY_ENABLE_SPIRV_CROSS

#include <spirv_cross.hpp>

namespace radray::render {

namespace {

// 辅助函数：获取基础类型的标量大小
uint32_t GetScalarSize(SpirvBaseType baseType) {
    switch (baseType) {
        case SpirvBaseType::Bool:
        case SpirvBaseType::Int8:
        case SpirvBaseType::UInt8: return 1;
        case SpirvBaseType::Int16:
        case SpirvBaseType::UInt16:
        case SpirvBaseType::Float16: return 2;
        case SpirvBaseType::Int32:
        case SpirvBaseType::UInt32:
        case SpirvBaseType::Float32: return 4;
        case SpirvBaseType::Int64:
        case SpirvBaseType::UInt64:
        case SpirvBaseType::Float64: return 8;
        default: return 0;
    }
}

// 辅助函数：将SPIR-V类型转换为我们的基础类型
SpirvBaseType GetBaseTypeFromSpirv(const spirv_cross::SPIRType& type) {
    switch (type.basetype) {
        case spirv_cross::SPIRType::Void: return SpirvBaseType::Void;
        case spirv_cross::SPIRType::Boolean: return SpirvBaseType::Bool;
        case spirv_cross::SPIRType::SByte: return SpirvBaseType::Int8;
        case spirv_cross::SPIRType::UByte: return SpirvBaseType::UInt8;
        case spirv_cross::SPIRType::Short: return SpirvBaseType::Int16;
        case spirv_cross::SPIRType::UShort: return SpirvBaseType::UInt16;
        case spirv_cross::SPIRType::Int: return SpirvBaseType::Int32;
        case spirv_cross::SPIRType::UInt: return SpirvBaseType::UInt32;
        case spirv_cross::SPIRType::Int64: return SpirvBaseType::Int64;
        case spirv_cross::SPIRType::UInt64: return SpirvBaseType::UInt64;
        case spirv_cross::SPIRType::Half: return SpirvBaseType::Float16;
        case spirv_cross::SPIRType::Float: return SpirvBaseType::Float32;
        case spirv_cross::SPIRType::Double: return SpirvBaseType::Float64;
        case spirv_cross::SPIRType::Struct: return SpirvBaseType::Struct;
        case spirv_cross::SPIRType::Image: return SpirvBaseType::Image;
        case spirv_cross::SPIRType::SampledImage: return SpirvBaseType::SampledImage;
        case spirv_cross::SPIRType::Sampler: return SpirvBaseType::Sampler;
        case spirv_cross::SPIRType::AccelerationStructure: return SpirvBaseType::AccelerationStructure;
        default: return SpirvBaseType::UNKNOWN;
    }
}

// 辅助函数：从类型推断顶点格式
VertexFormat InferVertexFormat(const spirv_cross::SPIRType& type) {
    uint32_t vecsize = type.vecsize;
    uint32_t columns = type.columns;

    if (columns != 1) return VertexFormat::UNKNOWN;

    switch (type.basetype) {
        case spirv_cross::SPIRType::Float:
            if (vecsize == 1) return VertexFormat::FLOAT32;
            if (vecsize == 2) return VertexFormat::FLOAT32X2;
            if (vecsize == 3) return VertexFormat::FLOAT32X3;
            if (vecsize == 4) return VertexFormat::FLOAT32X4;
            break;
        case spirv_cross::SPIRType::Int:
            if (vecsize == 1) return VertexFormat::SINT32;
            if (vecsize == 2) return VertexFormat::SINT32X2;
            if (vecsize == 3) return VertexFormat::SINT32X3;
            if (vecsize == 4) return VertexFormat::SINT32X4;
            break;
        case spirv_cross::SPIRType::UInt:
            if (vecsize == 1) return VertexFormat::UINT32;
            if (vecsize == 2) return VertexFormat::UINT32X2;
            if (vecsize == 3) return VertexFormat::UINT32X3;
            if (vecsize == 4) return VertexFormat::UINT32X4;
            break;
        case spirv_cross::SPIRType::Half:
            if (vecsize == 2) return VertexFormat::FLOAT16X2;
            if (vecsize == 4) return VertexFormat::FLOAT16X4;
            break;
        default:
            break;
    }

    return VertexFormat::UNKNOWN;
}

// 辅助函数：获取图像维度
SpirvImageDim GetImageDim(spv::Dim dim) {
    switch (dim) {
        case spv::Dim1D: return SpirvImageDim::Dim1D;
        case spv::Dim2D: return SpirvImageDim::Dim2D;
        case spv::Dim3D: return SpirvImageDim::Dim3D;
        case spv::DimCube: return SpirvImageDim::Cube;
        case spv::DimBuffer: return SpirvImageDim::Buffer;
        default: return SpirvImageDim::UNKNOWN;
    }
}

// 前向声明
uint32_t ReflectType(
    const spirv_cross::Compiler& compiler,
    const spirv_cross::SPIRType& type,
    SpirvShaderDesc& desc,
    unordered_map<uint32_t, uint32_t>& typeCache);

// 递归反射类型信息
uint32_t ReflectType(
    const spirv_cross::Compiler& compiler,
    const spirv_cross::SPIRType& type,
    SpirvShaderDesc& desc,
    unordered_map<uint32_t, uint32_t>& typeCache) {
    auto it = typeCache.find(type.self);
    if (it != typeCache.end()) {
        return it->second;
    }

    // 关键修复：先占位，确保索引固定，避免递归导致索引错位
    uint32_t typeIndex = static_cast<uint32_t>(desc.Types.size());
    typeCache[type.self] = typeIndex;
    desc.Types.emplace_back();

    SpirvTypeInfo typeInfo{};

    std::string typeName = compiler.get_name(type.self);
    if (typeName.empty()) {
        typeName = compiler.get_fallback_name(type.self);
    }
    typeInfo.Name = string(typeName);

    typeInfo.BaseType = GetBaseTypeFromSpirv(type);
    typeInfo.VectorSize = type.vecsize;
    typeInfo.Columns = type.columns;

    if (!type.array.empty()) {
        typeInfo.ArraySize = type.array[0];
        typeInfo.ArrayStride = compiler.get_decoration(type.self, spv::DecorationArrayStride);
    }

    if (type.columns > 1) {
        typeInfo.MatrixStride = compiler.get_decoration(type.self, spv::DecorationMatrixStride);
        if (compiler.has_decoration(type.self, spv::DecorationRowMajor)) {
            typeInfo.RowMajor = true;
        }
    }

    // 计算类型大小
    if (type.basetype == spirv_cross::SPIRType::Struct) {
        try {
            typeInfo.Size = static_cast<uint32_t>(compiler.get_declared_struct_size(type));
        } catch (...) {
            typeInfo.Size = 0;
        }
    } else {
        uint32_t scalarSize = GetScalarSize(typeInfo.BaseType);
        typeInfo.Size = scalarSize * type.vecsize * type.columns;

        // 累乘数组维度
        for (auto dim : type.array) {
            if (dim == 0) {
                typeInfo.Size = 0;  // 动态数组大小未知
                break;
            }
            typeInfo.Size *= dim;
        }
    }

    if (type.basetype == spirv_cross::SPIRType::Struct) {
        size_t memberCount = type.member_types.size();
        typeInfo.Members.reserve(memberCount);

        for (size_t i = 0; i < memberCount; ++i) {
            SpirvTypeMember member{};

            std::string memberName = compiler.get_member_name(type.self, static_cast<uint32_t>(i));
            if (memberName.empty()) {
                member.Name = string("member_") + string(std::to_string(i));
            } else {
                member.Name = string(memberName);
            }

            try {
                member.Offset = compiler.type_struct_member_offset(type, static_cast<uint32_t>(i));
            } catch (...) {
                member.Offset = 0;
            }

            const auto& memberType = compiler.get_type(type.member_types[i]);
            member.TypeIndex = ReflectType(compiler, memberType, desc, typeCache);

            try {
                member.Size = static_cast<uint32_t>(compiler.get_declared_struct_member_size(type, static_cast<uint32_t>(i)));
            } catch (...) {
                // 如果无法获取成员大小（无布局），尝试使用类型本身的大小
                if (member.TypeIndex < desc.Types.size()) {
                    member.Size = desc.Types[member.TypeIndex].Size;
                } else {
                    member.Size = 0;
                }
            }

            typeInfo.Members.push_back(std::move(member));
        }
    }

    desc.Types[typeIndex] = std::move(typeInfo);
    return typeIndex;
}

// 处理单个资源
void ProcessResource(
    const spirv_cross::Compiler& compiler,
    const spirv_cross::Resource& res,
    SpirvResourceKind kind,
    ShaderStage stage,
    SpirvShaderDesc& desc,
    unordered_map<uint32_t, uint32_t>& typeCache) {
    SpirvResourceBinding binding{};
    // 优先获取变量名（ID对应的名称），spirv-cross的res.name可能会回退到类型名
    binding.Name = compiler.get_name(res.id);
    if (binding.Name.empty()) {
        binding.Name = string(res.name);
    }
    binding.Kind = kind;
    binding.Stages = stage;

    binding.Set = compiler.get_decoration(res.id, spv::DecorationDescriptorSet);
    binding.Binding = compiler.get_decoration(res.id, spv::DecorationBinding);

    const auto& type = compiler.get_type(res.type_id);
    binding.TypeIndex = ReflectType(compiler, type, desc, typeCache);

    if (!type.array.empty()) {
        binding.ArraySize = type.array[0];
    }

    if (type.basetype == spirv_cross::SPIRType::Image ||
        type.basetype == spirv_cross::SPIRType::SampledImage) {
        SpirvImageInfo imgInfo{};
        imgInfo.Dim = GetImageDim(type.image.dim);
        imgInfo.Arrayed = type.image.arrayed;
        imgInfo.Multisampled = type.image.ms;
        imgInfo.Depth = type.image.depth;

        const auto& sampledType = compiler.get_type(type.image.type);
        imgInfo.SampledType = ReflectType(compiler, sampledType, desc, typeCache);

        binding.ImageInfo = imgInfo;
    }

    if (kind == SpirvResourceKind::StorageBuffer || kind == SpirvResourceKind::StorageImage) {
        auto access = compiler.get_decoration(res.id, spv::DecorationNonReadable);
        binding.WriteOnly = (access != 0);
        access = compiler.get_decoration(res.id, spv::DecorationNonWritable);
        binding.ReadOnly = (access != 0);
    }

    desc.ResourceBindings.push_back(std::move(binding));
}

// 反射资源绑定
void ReflectResourceBindings(
    const spirv_cross::Compiler& compiler,
    const spirv_cross::ShaderResources& resources,
    ShaderStage stage,
    SpirvShaderDesc& desc,
    unordered_map<uint32_t, uint32_t>& typeCache) {
    for (const auto& ubo : resources.uniform_buffers) {
        ProcessResource(compiler, ubo, SpirvResourceKind::UniformBuffer, stage, desc, typeCache);
    }

    for (const auto& ssbo : resources.storage_buffers) {
        ProcessResource(compiler, ssbo, SpirvResourceKind::StorageBuffer, stage, desc, typeCache);
    }

    for (const auto& img : resources.sampled_images) {
        ProcessResource(compiler, img, SpirvResourceKind::SampledImage, stage, desc, typeCache);
    }

    for (const auto& img : resources.storage_images) {
        ProcessResource(compiler, img, SpirvResourceKind::StorageImage, stage, desc, typeCache);
    }

    for (const auto& img : resources.separate_images) {
        ProcessResource(compiler, img, SpirvResourceKind::SeparateImage, stage, desc, typeCache);
    }

    for (const auto& smp : resources.separate_samplers) {
        ProcessResource(compiler, smp, SpirvResourceKind::SeparateSampler, stage, desc, typeCache);
    }

    for (const auto& as : resources.acceleration_structures) {
        ProcessResource(compiler, as, SpirvResourceKind::AccelerationStructure, stage, desc, typeCache);
    }
}

// 反射顶点输入
void ReflectVertexInputs(
    const spirv_cross::Compiler& compiler,
    const spirv_cross::ShaderResources& resources,
    SpirvShaderDesc& desc,
    unordered_map<uint32_t, uint32_t>& typeCache) {
    for (const auto& input : resources.stage_inputs) {
        SpirvVertexInput vertexInput{};
        vertexInput.Name = string(input.name);
        vertexInput.Location = compiler.get_decoration(input.id, spv::DecorationLocation);

        const auto& type = compiler.get_type(input.type_id);
        vertexInput.TypeIndex = ReflectType(compiler, type, desc, typeCache);
        vertexInput.Format = InferVertexFormat(type);

        desc.VertexInputs.push_back(std::move(vertexInput));
    }

    std::sort(desc.VertexInputs.begin(), desc.VertexInputs.end(),
              [](const SpirvVertexInput& a, const SpirvVertexInput& b) {
                  return a.Location < b.Location;
              });
}

// 反射Push Constants
void ReflectPushConstants(
    const spirv_cross::Compiler& compiler,
    const spirv_cross::ShaderResources& resources,
    ShaderStage stage,
    SpirvShaderDesc& desc,
    unordered_map<uint32_t, uint32_t>& typeCache) {
    for (const auto& pc : resources.push_constant_buffers) {
        SpirvPushConstantRange range{};
        range.Name = string(pc.name);
        range.Stages = stage;

        const auto& type = compiler.get_type(pc.type_id);
        range.TypeIndex = ReflectType(compiler, type, desc, typeCache);
        range.Size = static_cast<uint32_t>(compiler.get_declared_struct_size(type));
        range.Offset = 0;

        desc.PushConstants.push_back(std::move(range));
    }
}

// 反射Compute Shader信息
void ReflectComputeInfo(
    const spirv_cross::Compiler& compiler,
    SpirvShaderDesc& desc) {
    SpirvComputeInfo computeInfo{};
    auto workGroupSize = compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 0);

    if (workGroupSize != 0) {
        computeInfo.LocalSizeX = compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 0);
        computeInfo.LocalSizeY = compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 1);
        computeInfo.LocalSizeZ = compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 2);

        desc.ComputeInfo = computeInfo;
    }
}

// 合并多个着色器阶段的资源信息
void MergeResourceBindings(SpirvShaderDesc& desc) {
    for (size_t i = 0; i < desc.ResourceBindings.size(); ++i) {
        for (size_t j = i + 1; j < desc.ResourceBindings.size();) {
            if (desc.ResourceBindings[i].Set == desc.ResourceBindings[j].Set &&
                desc.ResourceBindings[i].Binding == desc.ResourceBindings[j].Binding &&
                desc.ResourceBindings[i].Name == desc.ResourceBindings[j].Name) {
                desc.ResourceBindings[i].Stages =
                    static_cast<ShaderStage>(
                        static_cast<uint32_t>(desc.ResourceBindings[i].Stages) |
                        static_cast<uint32_t>(desc.ResourceBindings[j].Stages));

                desc.ResourceBindings.erase(desc.ResourceBindings.begin() + j);
            } else {
                ++j;
            }
        }
    }

    for (size_t i = 0; i < desc.PushConstants.size(); ++i) {
        for (size_t j = i + 1; j < desc.PushConstants.size();) {
            if (desc.PushConstants[i].Name == desc.PushConstants[j].Name) {
                desc.PushConstants[i].Stages =
                    static_cast<ShaderStage>(
                        static_cast<uint32_t>(desc.PushConstants[i].Stages) |
                        static_cast<uint32_t>(desc.PushConstants[j].Stages));
                desc.PushConstants.erase(desc.PushConstants.begin() + j);
            } else {
                ++j;
            }
        }
    }
}

}  // namespace

std::optional<SpirvShaderDesc> ReflectSpirv(std::span<const SpirvBytecodeView> bytecodes) {
    SpirvShaderDesc desc;
    unordered_map<uint32_t, uint32_t> typeCache;

    for (auto&& bytecode : bytecodes) {
        if (bytecode.Data.size() % 4 != 0) {
            RADRAY_ERR_LOG("{} {}", Errors::SPIRV_CROSS, "Invalid SPIR-V data size");
            return std::nullopt;
        }

        try {
            spirv_cross::Compiler compiler{
                std::bit_cast<const uint32_t*>(bytecode.Data.data()),
                bytecode.Data.size() / sizeof(uint32_t)};

            spirv_cross::ShaderResources resources = compiler.get_shader_resources();

            desc.UsedStages = static_cast<ShaderStage>(
                static_cast<uint32_t>(desc.UsedStages) |
                static_cast<uint32_t>(bytecode.Stage));

            if (bytecode.Stage == ShaderStage::Vertex) {
                ReflectVertexInputs(compiler, resources, desc, typeCache);
            }

            ReflectResourceBindings(compiler, resources, bytecode.Stage, desc, typeCache);

            ReflectPushConstants(compiler, resources, bytecode.Stage, desc, typeCache);

            if (bytecode.Stage == ShaderStage::Compute) {
                ReflectComputeInfo(compiler, desc);
            }

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
    }

    MergeResourceBindings(desc);

    return desc;
}

}  // namespace radray::render

#endif
