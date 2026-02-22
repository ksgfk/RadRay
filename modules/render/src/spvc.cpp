#include <radray/render/spvc.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <utility>

#include <radray/logger.h>

namespace radray::render {

ResourceBindType SpirvResourceBinding::MapResourceBindType() const noexcept {
    switch (Kind) {
        case SpirvResourceKind::UniformBuffer:
            return ResourceBindType::CBuffer;
        case SpirvResourceKind::StorageBuffer:
            return (ReadOnly && !WriteOnly) ? ResourceBindType::Buffer : ResourceBindType::RWBuffer;
        case SpirvResourceKind::SampledImage:
        case SpirvResourceKind::SeparateImage:
            return ResourceBindType::Texture;
        case SpirvResourceKind::SeparateSampler:
            return ResourceBindType::Sampler;
        case SpirvResourceKind::StorageImage:
            return ResourceBindType::RWTexture;
        case SpirvResourceKind::AccelerationStructure:
            return ResourceBindType::Buffer;
        default:
            return ResourceBindType::UNKNOWN;
    }
}

}  // namespace radray::render

#ifdef RADRAY_ENABLE_SPIRV_CROSS

#include <spirv_cross.hpp>
#include <spirv_cross_c.h>
#include <spirv_msl.hpp>
#include <radray/render/dxc.h>

namespace radray::render {

static uint32_t _GetScalarSize(SpirvBaseType baseType) {
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

static SpirvBaseType _GetBaseTypeFromSpirv(const spirv_cross::SPIRType& type) {
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

static VertexFormat _InferVertexFormat(const spirv_cross::SPIRType& type) {
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

static SpirvImageDim _GetImageDim(spv::Dim dim) {
    switch (dim) {
        case spv::Dim1D: return SpirvImageDim::Dim1D;
        case spv::Dim2D: return SpirvImageDim::Dim2D;
        case spv::Dim3D: return SpirvImageDim::Dim3D;
        case spv::DimCube: return SpirvImageDim::Cube;
        case spv::DimBuffer: return SpirvImageDim::Buffer;
        default: return SpirvImageDim::UNKNOWN;
    }
}

struct SpirvReflectionContext {
    const spirv_cross::Compiler& compiler;
    SpirvShaderDesc& desc;
    unordered_map<uint32_t, uint32_t>& typeCache;
    Nullable<const DxcReflectionRadrayExt*> ext;
};

static uint32_t _ReflectType(
    SpirvReflectionContext& ctx,
    const spirv_cross::SPIRType& rootType) {
    auto& compiler = ctx.compiler;
    auto& desc = ctx.desc;
    auto& typeCache = ctx.typeCache;

    {
        auto it = typeCache.find(rootType.self);
        if (it != typeCache.end()) {
            return it->second;
        }
    }
    struct Frame {
        const spirv_cross::SPIRType* type;
        uint32_t* resultPtr;
        int depth;
        SpirvTypeInfo info{};
        size_t currentMemberIndex = 0;
        bool hasExplicitLayout = false;
        uint32_t typeIndex = 0;
        bool initialized = false;
    };
    stack<Frame> stack;
    uint32_t rootResult = 0;
    stack.push({&rootType, &rootResult, 0});
    while (!stack.empty()) {
        auto& frame = stack.top();
        if (!frame.initialized) {
            auto it = typeCache.find(frame.type->self);
            if (it != typeCache.end()) {
                *frame.resultPtr = it->second;
                stack.pop();
                continue;
            }
            if (frame.depth > 256) {
                throw spirv_cross::CompilerError{"SPIR-V reflection recursion depth exceeded limit"};
            }
            frame.typeIndex = static_cast<uint32_t>(desc.Types.size());
            typeCache[frame.type->self] = frame.typeIndex;
            desc.Types.emplace_back();
            *frame.resultPtr = frame.typeIndex;
            std::string typeName = compiler.get_name(frame.type->self);
            if (typeName.empty()) {
                typeName = compiler.get_fallback_name(frame.type->self);
            }
            frame.info.Name = string(typeName);
            frame.info.BaseType = _GetBaseTypeFromSpirv(*frame.type);
            frame.info.VectorSize = frame.type->vecsize;
            frame.info.Columns = frame.type->columns;
            if (!frame.type->array.empty()) {
                frame.info.ArraySize = frame.type->array[0];
                frame.info.ArrayStride = compiler.get_decoration(frame.type->self, spv::DecorationArrayStride);
            }
            if (frame.type->columns > 1) {
                frame.info.MatrixStride = compiler.get_decoration(frame.type->self, spv::DecorationMatrixStride);
                if (compiler.has_decoration(frame.type->self, spv::DecorationRowMajor)) {
                    frame.info.RowMajor = true;
                }
            }
            if (frame.type->basetype == spirv_cross::SPIRType::Struct) {
                if (!frame.type->member_types.empty()) {
                    frame.hasExplicitLayout = compiler.has_member_decoration(frame.type->self, 0, spv::DecorationOffset);
                }
                if (frame.hasExplicitLayout) {
                    frame.info.Size = static_cast<uint32_t>(compiler.get_declared_struct_size(*frame.type));
                } else {
                    frame.info.Size = 0;
                }
                frame.info.Members.resize(frame.type->member_types.size());
            } else {
                uint32_t scalarSize = _GetScalarSize(frame.info.BaseType);
                frame.info.Size = scalarSize * frame.type->vecsize * frame.type->columns;
                for (auto dim : frame.type->array) {
                    if (dim == 0) {
                        frame.info.Size = 0;
                        break;
                    }
                    frame.info.Size *= dim;
                }
                desc.Types[frame.typeIndex] = std::move(frame.info);
                stack.pop();
                continue;
            }
            frame.initialized = true;
        }
        if (frame.currentMemberIndex < frame.info.Members.size()) {
            size_t i = frame.currentMemberIndex;
            auto& member = frame.info.Members[i];
            std::string memberName = compiler.get_member_name(frame.type->self, static_cast<uint32_t>(i));
            if (memberName.empty()) {
                member.Name = string("member_") + string(std::to_string(i));
            } else {
                member.Name = string(memberName);
            }
            member.Offset = compiler.type_struct_member_offset(*frame.type, static_cast<uint32_t>(i));
            const auto& memberType = compiler.get_type(frame.type->member_types[i]);
            frame.currentMemberIndex++;
            stack.push({&memberType, &member.TypeIndex, frame.depth + 1});
        } else {
            for (size_t i = 0; i < frame.info.Members.size(); ++i) {
                auto& member = frame.info.Members[i];
                if (frame.hasExplicitLayout) {
                    member.Size = static_cast<uint32_t>(compiler.get_declared_struct_member_size(*frame.type, static_cast<uint32_t>(i)));
                } else {
                    if (member.TypeIndex < desc.Types.size()) {
                        member.Size = desc.Types[member.TypeIndex].Size;
                    } else {
                        member.Size = 0;
                    }
                }
            }
            desc.Types[frame.typeIndex] = std::move(frame.info);
            stack.pop();
        }
    }
    return rootResult;
}

static void _ProcessResource(
    SpirvReflectionContext& ctx,
    const spirv_cross::Resource& res,
    SpirvResourceKind kind,
    ShaderStage stage) {
    auto& compiler = ctx.compiler;
    auto& desc = ctx.desc;
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
    binding.TypeIndex = _ReflectType(ctx, type);
    if (!type.array.empty()) {
        binding.ArraySize = type.array[0];
        if (binding.ArraySize == 0) {
            binding.IsUnboundedArray = true;
        }
    }
    if (type.basetype == spirv_cross::SPIRType::Image ||
        type.basetype == spirv_cross::SPIRType::SampledImage) {
        SpirvImageInfo imgInfo{};
        imgInfo.Dim = _GetImageDim(type.image.dim);
        imgInfo.Arrayed = type.image.arrayed;
        imgInfo.Multisampled = type.image.ms;
        imgInfo.Depth = type.image.depth;
        const auto& sampledType = compiler.get_type(type.image.type);
        imgInfo.SampledType = _ReflectType(ctx, sampledType);
        binding.ImageInfo = imgInfo;
    }
    if (kind == SpirvResourceKind::StorageBuffer || kind == SpirvResourceKind::StorageImage) {
        auto access = compiler.get_decoration(res.id, spv::DecorationNonReadable);
        binding.WriteOnly = (access != 0);
        access = compiler.get_decoration(res.id, spv::DecorationNonWritable);
        binding.ReadOnly = (access != 0);
    }
    if (kind == SpirvResourceKind::UniformBuffer || kind == SpirvResourceKind::PushConstant) {
        // 检查扩展信息，确定是否为HLSL视图
        if (ctx.ext.HasValue()) {
            auto it = std::find_if(
                ctx.ext->CBuffers.begin(), ctx.ext->CBuffers.end(),
                [&](const DxcReflectionRadrayExtCBuffer& cb) {
                    return cb.Name == binding.Name && cb.Space == binding.Set && cb.BindPoint == binding.Binding;
                });
            if (it != ctx.ext->CBuffers.end()) {
                binding.IsViewInHlsl = it->IsViewInHlsl;
            }
        }
        const auto* typePtr = &type;
        while (!typePtr->array.empty()) {
            typePtr = &compiler.get_type(typePtr->parent_type);
        }
        binding.UniformBufferSize = static_cast<uint32_t>(compiler.get_declared_struct_size(*typePtr));
        if (binding.ArraySize > 0) {
            // ConstantBuffer<T> _v[4] : register(b0); 这种情况, 如果spv里没带 stride 推不出正确大小, 以后再想想有没有办法
            uint32_t arrayStride = compiler.get_decoration(type.self, spv::DecorationArrayStride);
            if (arrayStride > 0) {
                binding.UniformBufferSize = arrayStride * binding.ArraySize;
            } else {
                throw spirv_cross::CompilerError{"Struct member does not have ArrayStride set."};
            }
        }
    }
    desc.ResourceBindings.push_back(std::move(binding));
}

static void _ReflectResourceBindings(
    SpirvReflectionContext& ctx,
    const spirv_cross::ShaderResources& resources,
    ShaderStage stage) {
    for (const auto& ubo : resources.uniform_buffers) {
        _ProcessResource(ctx, ubo, SpirvResourceKind::UniformBuffer, stage);
    }
    for (const auto& ssbo : resources.storage_buffers) {
        _ProcessResource(ctx, ssbo, SpirvResourceKind::StorageBuffer, stage);
    }
    for (const auto& img : resources.sampled_images) {
        _ProcessResource(ctx, img, SpirvResourceKind::SampledImage, stage);
    }
    for (const auto& img : resources.storage_images) {
        _ProcessResource(ctx, img, SpirvResourceKind::StorageImage, stage);
    }
    for (const auto& img : resources.separate_images) {
        _ProcessResource(ctx, img, SpirvResourceKind::SeparateImage, stage);
    }
    for (const auto& smp : resources.separate_samplers) {
        _ProcessResource(ctx, smp, SpirvResourceKind::SeparateSampler, stage);
    }
    for (const auto& as : resources.acceleration_structures) {
        _ProcessResource(ctx, as, SpirvResourceKind::AccelerationStructure, stage);
    }
}

static void _ReflectVertexInputs(
    SpirvReflectionContext& ctx,
    const spirv_cross::ShaderResources& resources) {
    auto& compiler = ctx.compiler;
    auto& desc = ctx.desc;
    for (const auto& input : resources.stage_inputs) {
        SpirvVertexInput vertexInput{};
        vertexInput.Name = string(input.name);
        vertexInput.Location = compiler.get_decoration(input.id, spv::DecorationLocation);
        const auto& type = compiler.get_type(input.type_id);
        vertexInput.TypeIndex = _ReflectType(ctx, type);
        vertexInput.Format = _InferVertexFormat(type);
        desc.VertexInputs.push_back(std::move(vertexInput));
    }
    std::sort(
        desc.VertexInputs.begin(),
        desc.VertexInputs.end(),
        [](const SpirvVertexInput& a, const SpirvVertexInput& b) {
            return a.Location < b.Location;
        });
}

static void _ReflectPushConstants(
    SpirvReflectionContext& ctx,
    const spirv_cross::ShaderResources& resources,
    ShaderStage stage) {
    auto& compiler = ctx.compiler;
    auto& desc = ctx.desc;
    for (const auto& pc : resources.push_constant_buffers) {
        SpirvPushConstantRange range{};
        range.Name = string(pc.name);
        range.Stages = stage;
        const auto& type = compiler.get_type(pc.type_id);
        range.TypeIndex = _ReflectType(ctx, type);
        range.Size = static_cast<uint32_t>(compiler.get_declared_struct_size(type));
        range.Offset = 0;
        desc.PushConstants.push_back(std::move(range));
    }
}

static void _ReflectComputeInfo(
    SpirvReflectionContext& ctx) {
    auto& compiler = ctx.compiler;
    auto& desc = ctx.desc;
    SpirvComputeInfo computeInfo{};
    auto workGroupSize = compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 0);
    if (workGroupSize != 0) {
        computeInfo.LocalSizeX = compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 0);
        computeInfo.LocalSizeY = compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 1);
        computeInfo.LocalSizeZ = compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 2);
        desc.ComputeInfo = computeInfo;
    }
}

static void _MergeResourceBindings(SpirvShaderDesc& desc) {
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

std::optional<SpirvShaderDesc> ReflectSpirv(std::span<const SpirvBytecodeView> bytecodes, std::span<const DxcReflectionRadrayExt*> extInfos) {
    SpirvShaderDesc desc;
    for (size_t i = 0; i < bytecodes.size(); i++) {
        const auto& bytecode = bytecodes[i];
        if (bytecode.Data.size() % 4 != 0) {
            RADRAY_ERR_LOG("invalid SPIR-V data size, not multiple of 4 bytes");
            return std::nullopt;
        }
        unordered_map<uint32_t, uint32_t> typeCache;
        try {
            spirv_cross::Compiler compiler{
                std::bit_cast<const uint32_t*>(bytecode.Data.data()),
                bytecode.Data.size() / sizeof(uint32_t)};
            spirv_cross::ShaderResources resources = compiler.get_shader_resources();
            desc.UsedStages = desc.UsedStages | bytecode.Stage;
            Nullable<const DxcReflectionRadrayExt*> ext{nullptr};
            if (i < extInfos.size()) {
                ext = extInfos[i];
            }
            SpirvReflectionContext ctx{compiler, desc, typeCache, ext};
            if (bytecode.Stage == ShaderStage::Vertex) {
                _ReflectVertexInputs(ctx, resources);
            }
            _ReflectResourceBindings(ctx, resources, bytecode.Stage);
            _ReflectPushConstants(ctx, resources, bytecode.Stage);
            if (bytecode.Stage == ShaderStage::Compute) {
                _ReflectComputeInfo(ctx);
            }
        } catch (const spirv_cross::CompilerError& e) {
            RADRAY_ERR_LOG("SPIRV-Cross Compiler Error: {}", e.what());
            return std::nullopt;
        } catch (const std::exception& e) {
            RADRAY_ERR_LOG("SPIRV-Cross error: {}", e.what());
            return std::nullopt;
        } catch (...) {
            RADRAY_ERR_LOG("SPIRV-Cross error: {}", "unknown error");
            return std::nullopt;
        }
    }
    _MergeResourceBindings(desc);
    return desc;
}

static spv::ExecutionModel _MapShaderStageToExecutionModel(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Vertex: return spv::ExecutionModelVertex;
        case ShaderStage::Pixel: return spv::ExecutionModelFragment;
        case ShaderStage::Compute: return spv::ExecutionModelGLCompute;
        default: return spv::ExecutionModelMax;
    }
}

std::optional<SpirvToMslOutput> ConvertSpirvToMsl(
    std::span<const byte> spirvData,
    std::string_view entryPoint,
    ShaderStage stage,
    const SpirvToMslOption& option) {
    if (spirvData.size() % 4 != 0) {
        RADRAY_ERR_LOG("invalid SPIR-V data size, not multiple of 4 bytes");
        return std::nullopt;
    }
    try {
        spirv_cross::CompilerMSL compiler{
            std::bit_cast<const uint32_t*>(spirvData.data()),
            spirvData.size() / sizeof(uint32_t)};

        spirv_cross::CompilerMSL::Options mslOpt;
        mslOpt.set_msl_version(option.MslMajor, option.MslMinor, option.MslPatch);
        mslOpt.platform = (option.Platform == MslPlatform::IOS)
                              ? spirv_cross::CompilerMSL::Options::iOS
                              : spirv_cross::CompilerMSL::Options::macOS;
        mslOpt.argument_buffers = option.UseArgumentBuffers;
        mslOpt.force_native_arrays = option.ForceNativeArrays;
        compiler.set_msl_options(mslOpt);

        auto execModel = _MapShaderStageToExecutionModel(stage);
        if (execModel == spv::ExecutionModelMax) {
            RADRAY_ERR_LOG("unsupported shader stage for MSL conversion");
            return std::nullopt;
        }
        compiler.set_entry_point(string(entryPoint), execModel);

        // 为 vertex/fragment stage 的 buffer 资源绑定添加偏移，避免和 vertex buffer slot 0-15 冲突
        // texture 和 sampler 在 Metal 中是独立的索引空间，不需要偏移
        if (stage == ShaderStage::Vertex || stage == ShaderStage::Pixel) {
            spirv_cross::ShaderResources resources = compiler.get_shader_resources();
            auto addBufferBinding = [&](const spirv_cross::Resource& res) {
                uint32_t descSet = compiler.get_decoration(res.id, spv::DecorationDescriptorSet);
                uint32_t binding = compiler.get_decoration(res.id, spv::DecorationBinding);
                spirv_cross::MSLResourceBinding mslBinding{};
                mslBinding.stage = execModel;
                mslBinding.desc_set = descSet;
                mslBinding.binding = binding;
                mslBinding.msl_buffer = binding + MetalMaxVertexInputBindings;
                mslBinding.msl_texture = binding;
                mslBinding.msl_sampler = binding;
                compiler.add_msl_resource_binding(mslBinding);
            };
            for (const auto& r : resources.uniform_buffers) addBufferBinding(r);
            for (const auto& r : resources.storage_buffers) addBufferBinding(r);
            // push constant 也需要偏移
            if (!resources.push_constant_buffers.empty()) {
                spirv_cross::MSLResourceBinding pcBinding{};
                pcBinding.stage = execModel;
                pcBinding.desc_set = spirv_cross::kPushConstDescSet;
                pcBinding.binding = spirv_cross::kPushConstBinding;
                pcBinding.msl_buffer = MetalMaxVertexInputBindings;
                compiler.add_msl_resource_binding(pcBinding);
            }
        }

        std::string msl = compiler.compile();
        std::string mslEntryPoint = compiler.get_cleansed_entry_point_name(
            string(entryPoint), execModel);

        SpirvToMslOutput output;
        output.MslSource = string(msl);
        output.EntryPointName = string(mslEntryPoint);
        return output;
    } catch (const spirv_cross::CompilerError& e) {
        RADRAY_ERR_LOG("SPIRV-Cross MSL Compiler Error: {}", e.what());
        return std::nullopt;
    } catch (const std::exception& e) {
        RADRAY_ERR_LOG("SPIRV-Cross MSL error: {}", e.what());
        return std::nullopt;
    } catch (...) {
        RADRAY_ERR_LOG("SPIRV-Cross MSL error: unknown error");
        return std::nullopt;
    }
}

}  // namespace radray::render

#endif
