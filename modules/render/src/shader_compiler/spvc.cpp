#include <radray/render/shader_compiler/spvc.h>

namespace radray::render {

}  // namespace radray::render

#ifdef RADRAY_ENABLE_SPIRV_CROSS

#include <algorithm>
#include <bit>
#include <cstring>
#include <utility>

#include <spirv_cross.hpp>
#include <spirv_cross_c.h>
#include <spirv_msl.hpp>

#include <radray/logger.h>
#include <radray/utility.h>

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
};

static bool _IsConstantBufferViewType(
    const spirv_cross::Compiler& compiler,
    const spirv_cross::SPIRType& cbufferType,
    std::string_view cbufferName) {
    if (cbufferType.basetype != spirv_cross::SPIRType::Struct || cbufferType.member_types.size() != 1) {
        return false;
    }
    std::string memberName = compiler.get_member_name(cbufferType.self, 0);
    if (memberName == cbufferName) {
        return true;
    }
    const uint32_t memberTypeId = cbufferType.member_types[0];
    std::string memberTypeName = compiler.get_name(memberTypeId);
    if (memberTypeName.empty()) {
        memberTypeName = compiler.get_fallback_name(memberTypeId);
    }
    return memberTypeName == cbufferName;
}

static bool _IsLikelyConstantBufferViewTypeName(std::string_view typeName) {
    return typeName.starts_with("type.ConstantBuffer.") || typeName.starts_with("ConstantBuffer<");
}

static bool _InferIsViewInHlsl(
    const spirv_cross::Compiler& compiler,
    const spirv_cross::Resource& res,
    std::string_view cbufferName) {
    const auto& type = compiler.get_type(res.type_id);
    if (_IsConstantBufferViewType(compiler, type, cbufferName)) {
        return true;
    }
    const auto& baseType = compiler.get_type(res.base_type_id);
    if (_IsConstantBufferViewType(compiler, baseType, cbufferName)) {
        return true;
    }
    if (type.parent_type != 0) {
        const auto& parentType = compiler.get_type(type.parent_type);
        if (_IsConstantBufferViewType(compiler, parentType, cbufferName)) {
            return true;
        }
    }
    if (baseType.parent_type != 0) {
        const auto& parentType = compiler.get_type(baseType.parent_type);
        if (_IsConstantBufferViewType(compiler, parentType, cbufferName)) {
            return true;
        }
    }
    if (res.base_type_id == 0) {
        return false;
    }
    std::string baseTypeName = compiler.get_name(res.base_type_id);
    if (baseTypeName.empty()) {
        baseTypeName = compiler.get_fallback_name(res.base_type_id);
    }
    if (_IsLikelyConstantBufferViewTypeName(baseTypeName)) {
        return true;
    }
    if (baseTypeName == cbufferName) {
        return true;
    }
    std::string typeName = compiler.get_name(res.type_id);
    if (typeName.empty()) {
        typeName = compiler.get_fallback_name(res.type_id);
    }
    if (_IsLikelyConstantBufferViewTypeName(typeName)) {
        return true;
    }
    return false;
}

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
    SpirvResourceKind kind) {
    auto& compiler = ctx.compiler;
    auto& desc = ctx.desc;
    SpirvResourceBinding binding{};
    // 优先获取变量名（ID对应的名称），spirv-cross的res.name可能会回退到类型名
    binding.Name = compiler.get_name(res.id);
    if (binding.Name.empty()) {
        binding.Name = string(res.name);
    }
    binding.Kind = kind;
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
    if (kind == SpirvResourceKind::UniformBuffer) {
        binding.IsViewInHlsl = _InferIsViewInHlsl(compiler, res, binding.Name);
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
    const spirv_cross::ShaderResources& resources) {
    for (const auto& ubo : resources.uniform_buffers) {
        _ProcessResource(ctx, ubo, SpirvResourceKind::UniformBuffer);
    }
    for (const auto& ssbo : resources.storage_buffers) {
        _ProcessResource(ctx, ssbo, SpirvResourceKind::StorageBuffer);
    }
    for (const auto& img : resources.sampled_images) {
        _ProcessResource(ctx, img, SpirvResourceKind::SampledImage);
    }
    for (const auto& img : resources.storage_images) {
        _ProcessResource(ctx, img, SpirvResourceKind::StorageImage);
    }
    for (const auto& img : resources.separate_images) {
        _ProcessResource(ctx, img, SpirvResourceKind::SeparateImage);
    }
    for (const auto& smp : resources.separate_samplers) {
        _ProcessResource(ctx, smp, SpirvResourceKind::SeparateSampler);
    }
    for (const auto& as : resources.acceleration_structures) {
        _ProcessResource(ctx, as, SpirvResourceKind::AccelerationStructure);
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
        // vertexInput.Format = _InferVertexFormat(type);
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
    const spirv_cross::ShaderResources& resources) {
    auto& compiler = ctx.compiler;
    auto& desc = ctx.desc;
    for (const auto& pc : resources.push_constant_buffers) {
        SpirvPushConstantRange range{};
        range.Name = string(pc.name);
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

std::optional<SpirvShaderDesc> ReflectSpirv(SpirvBytecodeView bytecode) {
    SpirvShaderDesc desc;
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
        SpirvReflectionContext ctx{compiler, desc, typeCache};
        if (bytecode.Stage == ShaderStage::Vertex) {
            _ReflectVertexInputs(ctx, resources);
        }
        _ReflectResourceBindings(ctx, resources);
        _ReflectPushConstants(ctx, resources);
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
        if (option.UseArgumentBuffers) {
            mslOpt.argument_buffers_tier = spirv_cross::CompilerMSL::Options::ArgumentBuffersTier::Tier2;
        }
        mslOpt.force_native_arrays = option.ForceNativeArrays;
        compiler.set_msl_options(mslOpt);

        auto execModel = _MapShaderStageToExecutionModel(stage);
        if (execModel == spv::ExecutionModelMax) {
            RADRAY_ERR_LOG("unsupported shader stage for MSL conversion");
            return std::nullopt;
        }
        compiler.set_entry_point(string(entryPoint), execModel);

        // 对所有 stage，检查 runtime-sized array（bindless）并标记 device storage
        if (option.UseArgumentBuffers) {
            spirv_cross::ShaderResources allRes = compiler.get_shader_resources();
            auto markDeviceStorage = [&](const spirv_cross::Resource& res) {
                auto& type = compiler.get_type(res.type_id);
                if (!type.array.empty() && type.array.back() == 0) {
                    uint32_t ds = compiler.get_decoration(res.id, spv::DecorationDescriptorSet);
                    compiler.set_argument_buffer_device_address_space(ds, true);
                }
            };
            for (const auto& r : allRes.uniform_buffers) markDeviceStorage(r);
            for (const auto& r : allRes.storage_buffers) markDeviceStorage(r);
            for (const auto& r : allRes.sampled_images) markDeviceStorage(r);
            for (const auto& r : allRes.separate_images) markDeviceStorage(r);
            for (const auto& r : allRes.storage_images) markDeviceStorage(r);
            for (const auto& r : allRes.separate_samplers) markDeviceStorage(r);
        }

        // 为 vertex/fragment stage 的 buffer 资源绑定添加偏移，避免和 vertex buffer slot 0-15 冲突
        if (stage == ShaderStage::Vertex || stage == ShaderStage::Pixel) {
            spirv_cross::ShaderResources resources = compiler.get_shader_resources();
            if (option.UseArgumentBuffers) {
                // 启用 argument buffer 时，每个 descriptor set 变成一个 argument buffer，
                // 需要通过 kArgumentBufferBinding 将 argument buffer 的 buffer index 偏移，
                // 避免和 vertex buffer slot 0-15 冲突
                unordered_set<uint32_t> descSets;
                auto collectDescSet = [&](const spirv_cross::Resource& res) {
                    descSets.insert(compiler.get_decoration(res.id, spv::DecorationDescriptorSet));
                };
                for (const auto& r : resources.uniform_buffers) collectDescSet(r);
                for (const auto& r : resources.storage_buffers) collectDescSet(r);
                for (const auto& r : resources.sampled_images) collectDescSet(r);
                for (const auto& r : resources.separate_images) collectDescSet(r);
                for (const auto& r : resources.storage_images) collectDescSet(r);
                for (const auto& r : resources.separate_samplers) collectDescSet(r);
                for (uint32_t descSet : descSets) {
                    spirv_cross::MSLResourceBinding mslBinding{};
                    mslBinding.stage = execModel;
                    mslBinding.desc_set = descSet;
                    mslBinding.binding = spirv_cross::kArgumentBufferBinding;
                    // +1 to reserve MetalMaxVertexInputBindings for push constants
                    mslBinding.msl_buffer = descSet + MetalMaxVertexInputBindings + 1;
                    compiler.add_msl_resource_binding(mslBinding);
                }
            } else {
                // 未启用 argument buffer 时，只需偏移 buffer 类型资源
                // texture 和 sampler 在 Metal 中是独立的索引空间，不需要偏移
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
            }
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
        RADRAY_DEBUG_LOG("spvc convert msl entry point:{}\n{}", output.EntryPointName, output.MslSource);
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

// 计算 SPIR-V 类型在 Metal 中的对齐要求
static uint32_t _GetMslTypeAlignment(const spirv_cross::Compiler& compiler, const spirv_cross::SPIRType& type) {
    if (type.basetype == spirv_cross::SPIRType::Struct) {
        uint32_t maxAlign = 1;
        for (uint32_t i = 0; i < type.member_types.size(); i++) {
            auto& memberType = compiler.get_type(type.member_types[i]);
            maxAlign = std::max(maxAlign, _GetMslTypeAlignment(compiler, memberType));
        }
        return maxAlign;
    }
    uint32_t componentSize = type.width / 8;
    uint32_t vecsize = type.vecsize;
    // Metal: vec3 对齐同 vec4
    if (vecsize == 3) vecsize = 4;
    return vecsize * componentSize;
}

// 获取 Metal 中 struct 的实际大小（包含尾部 padding）
static size_t _GetMslStructSize(const spirv_cross::Compiler& compiler, const spirv_cross::SPIRType& type) {
    size_t declaredSize = compiler.get_declared_struct_size(type);
    uint32_t alignment = _GetMslTypeAlignment(compiler, type);
    if (alignment > 1) {
        declaredSize = (declaredSize + alignment - 1) & ~(static_cast<size_t>(alignment) - 1);
    }
    return declaredSize;
}

static void _ProcessMslBinding(
    MslReflCtx& ctx,
    const spirv_cross::Resource& res,
    MslArgumentType argType,
    MslStage stage,
    bool isReadOnly,
    bool isWriteOnly,
    bool isPushConstant = false,
    uint32_t descriptorSet = 0) {
    auto& compiler = ctx.compiler;
    MslArgument arg{};
    arg.Name = compiler.get_name(res.id);
    if (arg.Name.empty()) {
        arg.Name = string(res.name);
    }
    arg.Stage = stage;
    arg.Type = argType;
    arg.IsActive = true;
    arg.IsPushConstant = isPushConstant;
    arg.DescriptorSet = descriptorSet;
    uint32_t mslBinding = compiler.get_automatic_msl_resource_binding(res.id);
    uint32_t spvBinding = compiler.get_decoration(res.id, spv::DecorationBinding);
    arg.Index = (mslBinding != ~0u) ? mslBinding : spvBinding;
    const auto& type = compiler.get_type(res.type_id);
    if (!type.array.empty()) {
        arg.ArrayLength = type.array[0];
        if (type.array[0] == 0) {
            arg.IsUnboundedArray = true;
        }
    }
    if (argType == MslArgumentType::Buffer) {
        if (isReadOnly)
            arg.Access = MslAccess::ReadOnly;
        else if (isWriteOnly)
            arg.Access = MslAccess::WriteOnly;
        else
            arg.Access = MslAccess::ReadWrite;
        const auto& baseType = compiler.get_type(res.base_type_id);
        if (baseType.basetype == spirv_cross::SPIRType::Struct) {
            arg.BufferDataType = MslDataType::Struct;
            arg.BufferDataSize = _GetMslStructSize(compiler, baseType);
            arg.BufferStructTypeIndex = _ReflectMslStructType(ctx, baseType);
        } else {
            arg.BufferDataType = _MapSpirvToMslDataType(baseType);
        }
    } else if (argType == MslArgumentType::Texture) {
        if (isReadOnly)
            arg.Access = MslAccess::ReadOnly;
        else if (isWriteOnly)
            arg.Access = MslAccess::WriteOnly;
        else
            arg.Access = MslAccess::ReadWrite;
        arg.TextureType = _MapImageToMslTextureType(type);
        arg.IsDepthTexture = type.image.depth;
        const auto& sampledType = compiler.get_type(type.image.type);
        arg.TextureDataType = _MapSpirvToMslDataType(sampledType);
    }
    ctx.refl.Arguments.push_back(std::move(arg));
}

std::optional<MslShaderReflection> ReflectMsl(std::span<const MslReflectParams> msls) {
    MslShaderReflection refl{};
    bool useArgBuffers = false;
    if (!msls.empty()) {
        useArgBuffers = msls[0].UseArgumentBuffers;
    }
    refl.UseArgumentBuffers = useArgBuffers;
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
            mslOpt.argument_buffers = useArgBuffers;
            if (useArgBuffers) {
                mslOpt.argument_buffers_tier = spirv_cross::CompilerMSL::Options::ArgumentBuffersTier::Tier2;
            }
            compiler.set_msl_options(mslOpt);
            compiler.set_entry_point(string(msl.EntryPoint), execModel);
            spirv_cross::ShaderResources resources = compiler.get_shader_resources();
            // 对所有 stage，检查 runtime-sized array（bindless）并标记 device storage
            if (useArgBuffers) {
                auto markDeviceStorage = [&](const spirv_cross::Resource& res) {
                    auto& type = compiler.get_type(res.type_id);
                    if (!type.array.empty() && type.array.back() == 0) {
                        uint32_t ds = compiler.get_decoration(res.id, spv::DecorationDescriptorSet);
                        compiler.set_argument_buffer_device_address_space(ds, true);
                    }
                };
                for (const auto& r : resources.uniform_buffers) markDeviceStorage(r);
                for (const auto& r : resources.storage_buffers) markDeviceStorage(r);
                for (const auto& r : resources.sampled_images) markDeviceStorage(r);
                for (const auto& r : resources.separate_images) markDeviceStorage(r);
                for (const auto& r : resources.storage_images) markDeviceStorage(r);
                for (const auto& r : resources.separate_samplers) markDeviceStorage(r);
            }
            // 为 vertex/fragment stage 的资源绑定添加偏移，与 ConvertSpirvToMsl 保持一致
            if (msl.Stage == ShaderStage::Vertex || msl.Stage == ShaderStage::Pixel) {
                if (useArgBuffers) {
                    // 启用 argument buffer 时，每个 descriptor set 变成一个 argument buffer
                    unordered_set<uint32_t> descSets;
                    auto collectDescSet = [&](const spirv_cross::Resource& res) {
                        descSets.insert(compiler.get_decoration(res.id, spv::DecorationDescriptorSet));
                    };
                    for (const auto& r : resources.uniform_buffers) collectDescSet(r);
                    for (const auto& r : resources.storage_buffers) collectDescSet(r);
                    for (const auto& r : resources.sampled_images) collectDescSet(r);
                    for (const auto& r : resources.separate_images) collectDescSet(r);
                    for (const auto& r : resources.storage_images) collectDescSet(r);
                    for (const auto& r : resources.separate_samplers) collectDescSet(r);
                    for (uint32_t descSet : descSets) {
                        spirv_cross::MSLResourceBinding mslBinding{};
                        mslBinding.stage = execModel;
                        mslBinding.desc_set = descSet;
                        mslBinding.binding = spirv_cross::kArgumentBufferBinding;
                        mslBinding.msl_buffer = descSet + MetalMaxVertexInputBindings + 1;
                        compiler.add_msl_resource_binding(mslBinding);
                    }
                } else {
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
                }
                // push constant 偏移
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
            auto getDescSet = [&](const spirv_cross::Resource& res) -> uint32_t {
                return compiler.get_decoration(res.id, spv::DecorationDescriptorSet);
            };
            for (const auto& r : resources.uniform_buffers) {
                _ProcessMslBinding(ctx, r, MslArgumentType::Buffer, mslStage, true, false, false, getDescSet(r));
            }
            for (const auto& r : resources.storage_buffers) {
                bool ro = compiler.get_decoration(r.id, spv::DecorationNonWritable) != 0;
                bool wo = compiler.get_decoration(r.id, spv::DecorationNonReadable) != 0;
                _ProcessMslBinding(ctx, r, MslArgumentType::Buffer, mslStage, ro, wo, false, getDescSet(r));
            }
            for (const auto& r : resources.push_constant_buffers) {
                _ProcessMslBinding(ctx, r, MslArgumentType::Buffer, mslStage, true, false, true, 0);
            }
            for (const auto& r : resources.sampled_images) {
                _ProcessMslBinding(ctx, r, MslArgumentType::Texture, mslStage, true, false, false, getDescSet(r));
            }
            for (const auto& r : resources.separate_images) {
                _ProcessMslBinding(ctx, r, MslArgumentType::Texture, mslStage, true, false, false, getDescSet(r));
            }
            for (const auto& r : resources.storage_images) {
                bool ro = compiler.get_decoration(r.id, spv::DecorationNonWritable) != 0;
                bool wo = compiler.get_decoration(r.id, spv::DecorationNonReadable) != 0;
                _ProcessMslBinding(ctx, r, MslArgumentType::Texture, mslStage, ro, wo, false, getDescSet(r));
            }
            for (const auto& r : resources.separate_samplers) {
                _ProcessMslBinding(ctx, r, MslArgumentType::Sampler, mslStage, true, false, false, getDescSet(r));
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

#endif
