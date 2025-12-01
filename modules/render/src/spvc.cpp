#include <radray/render/spvc.h>

#ifdef RADRAY_ENABLE_SPIRV_CROSS

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <type_traits>
#include <unordered_set>
#include <utility>

#include <spirv_cross.hpp>

#include <radray/errors.h>
#include <radray/logger.h>
#include <radray/utility.h>

namespace radray::render {

static void _CollectTypeTree(
    spirv_cross::Compiler& compiler,
    uint32_t typeId,
    unordered_set<uint32_t>& typeIds,
    unordered_set<uint32_t>& structIds) {
    if (typeId == 0) {
        return;
    }
    if (!typeIds.insert(typeId).second) {
        return;
    }

    const auto& type = compiler.get_type(typeId);
    if (type.basetype == spirv_cross::SPIRType::BaseType::Struct) {
        structIds.insert(typeId);
        for (uint32_t memberTypeId : type.member_types) {
            _CollectTypeTree(compiler, memberTypeId, typeIds, structIds);
        }
    }

    if (type.pointer && type.parent_type != 0) {
        _CollectTypeTree(compiler, type.parent_type, typeIds, structIds);
    }
}

struct SpirvReflectionContext {
    spirv_cross::Compiler& compiler;
    SpirvShaderDesc& desc;
    unordered_map<uint32_t, SpirvTypeDesc*> typeCache{};
    unordered_map<uint32_t, SpirvStructDesc*> structCache{};
};

static SpirvBaseType _MapBaseType(spirv_cross::SPIRType::BaseType type) noexcept {
    switch (type) {
        case spirv_cross::SPIRType::BaseType::Boolean: return SpirvBaseType::Bool;
        case spirv_cross::SPIRType::BaseType::Int: return SpirvBaseType::Int;
        case spirv_cross::SPIRType::BaseType::UInt: return SpirvBaseType::UInt;
        case spirv_cross::SPIRType::BaseType::Float: return SpirvBaseType::Float;
        case spirv_cross::SPIRType::BaseType::Double: return SpirvBaseType::Double;
        case spirv_cross::SPIRType::BaseType::Struct: return SpirvBaseType::Struct;
        case spirv_cross::SPIRType::BaseType::Image: return SpirvBaseType::Image;
        case spirv_cross::SPIRType::BaseType::SampledImage: return SpirvBaseType::SampledImage;
        case spirv_cross::SPIRType::BaseType::Sampler: return SpirvBaseType::Sampler;
        case spirv_cross::SPIRType::BaseType::AccelerationStructure: return SpirvBaseType::AccelerationStructure;
        default: return SpirvBaseType::UNKNOWN;
    }
}

static uint32_t _ExtractArraySize(const spirv_cross::SPIRType& type) noexcept {
    if (type.array.empty()) {
        return 1;
    }
    uint64_t result = 1;
    for (size_t i = 0; i < type.array.size(); ++i) {
        if (!type.array_size_literal[i] || type.array[i] == 0) {
            return 0;
        }
        result *= static_cast<uint64_t>(type.array[i]);
    }
    return static_cast<uint32_t>(result);
}

static const SpirvStructDesc* _GetOrCreateStruct(SpirvReflectionContext& ctx, uint32_t typeId);

static const SpirvTypeDesc* _GetOrCreateType(SpirvReflectionContext& ctx, uint32_t typeId) {
    if (auto it = ctx.typeCache.find(typeId); it != ctx.typeCache.end()) {
        return it->second;
    }

    const auto& type = ctx.compiler.get_type(typeId);
    ctx.desc.types.emplace_back();
    auto* typeDesc = &ctx.desc.types.back();
    ctx.typeCache.emplace(typeId, typeDesc);

    typeDesc->base = _MapBaseType(type.basetype);
    typeDesc->bitWidth = type.width;
    typeDesc->vectorSize = type.vecsize;
    typeDesc->columnCount = type.columns;
    typeDesc->arraySize = _ExtractArraySize(type);

    if (type.basetype == spirv_cross::SPIRType::BaseType::Struct) {
        typeDesc->structInfo = _GetOrCreateStruct(ctx, typeId);
    }

    return typeDesc;
}

static string _ResolveName(const spirv_cross::Compiler& compiler, uint32_t id) {
    auto name = compiler.get_name(id);
    if (!name.empty()) {
        return string{name};
    }
    return string{compiler.get_fallback_name(id)};
}

static std::optional<uint32_t> _TryGetDecoration(const spirv_cross::Compiler& compiler, uint32_t id, spv::Decoration deco) {
    if (compiler.has_decoration(id, deco)) {
        return compiler.get_decoration(id, deco);
    }
    return std::nullopt;
}

static const SpirvStructDesc* _GetOrCreateStruct(SpirvReflectionContext& ctx, uint32_t typeId) {
    if (auto it = ctx.structCache.find(typeId); it != ctx.structCache.end()) {
        return it->second;
    }

    const auto& type = ctx.compiler.get_type(typeId);
    ctx.desc.structs.emplace_back();
    auto* structDesc = &ctx.desc.structs.back();
    ctx.structCache.emplace(typeId, structDesc);

    structDesc->name = ctx.compiler.get_name(typeId);
    if (structDesc->name.empty()) {
        structDesc->name = format("struct_{}", typeId);
    }
    structDesc->size = static_cast<uint32_t>(ctx.compiler.get_declared_struct_size(type));
    structDesc->members.clear();
    structDesc->members.reserve(type.member_types.size());
    for (uint32_t idx = 0; idx < type.member_types.size(); ++idx) {
        SpirvStructMemberDesc member{};
        member.name = ctx.compiler.get_member_name(typeId, idx);
        if (member.name.empty()) {
            member.name = format("member_{}", idx);
        }
        member.offset = ctx.compiler.type_struct_member_offset(type, idx);
        member.size = static_cast<uint32_t>(ctx.compiler.get_declared_struct_member_size(type, idx));
        member.type = _GetOrCreateType(ctx, type.member_types[idx]);
        structDesc->members.push_back(std::move(member));
    }

    return structDesc;
}

static SpirvResourceBindingDesc _BuildResourceBinding(
    SpirvReflectionContext& ctx,
    const spirv_cross::Resource& resource,
    SpirvResourceKind kind) {
    SpirvResourceBindingDesc binding{};
    binding.name = _ResolveName(ctx.compiler, resource.id);
    binding.kind = kind;
    binding.binding = _TryGetDecoration(ctx.compiler, resource.id, spv::DecorationBinding).value_or(0);
    binding.descriptorSet = _TryGetDecoration(ctx.compiler, resource.id, spv::DecorationDescriptorSet).value_or(0);
    binding.arraySize = _ExtractArraySize(ctx.compiler.get_type(resource.type_id));
    binding.valueType = _GetOrCreateType(ctx, resource.base_type_id);
    return binding;
}

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
    if (stage == ShaderStage::UNKNOWN) {
        RADRAY_ERR_LOG("{} {}", Errors::SPIRV_CROSS, "Shader stage cannot be UNKNOWN");
        return std::nullopt;
    }
    SpirvShaderDesc desc;
    try {
        spirv_cross::Compiler compiler{std::bit_cast<const uint32_t*>(data.data()), data.size() / sizeof(uint32_t)};
        std::string epStr{entryPointName};
        spv::ExecutionModel exeModel = _MapType(stage);
        const auto& entryPoint = compiler.get_entry_point(epStr, exeModel);
        auto resources = compiler.get_shader_resources();
        unordered_set<uint32_t> typeIds{};
        unordered_set<uint32_t> structTypeIds{};
        const auto collectFromList = [&](const spirv_cross::SmallVector<spirv_cross::Resource>& list) {
            for (const auto& res : list) {
                _CollectTypeTree(compiler, res.base_type_id, typeIds, structTypeIds);
            }
        };
        for (const auto& input : resources.stage_inputs) {
            _CollectTypeTree(compiler, input.base_type_id, typeIds, structTypeIds);
        }
        for (const auto& output : resources.stage_outputs) {
            _CollectTypeTree(compiler, output.base_type_id, typeIds, structTypeIds);
        }
        collectFromList(resources.uniform_buffers);
        collectFromList(resources.push_constant_buffers);
        collectFromList(resources.storage_buffers);
        collectFromList(resources.sampled_images);
        collectFromList(resources.storage_images);
        collectFromList(resources.separate_images);
        collectFromList(resources.separate_samplers);
        desc.types.reserve(typeIds.size());
        desc.structs.reserve(structTypeIds.size());
        SpirvReflectionContext ctx{compiler, desc};
        desc.stageInput.reserve(resources.stage_inputs.size());
        for (const auto& input : resources.stage_inputs) {
            SpirvParameterDesc param{};
            param.name = _ResolveName(compiler, input.id);
            param.location = _TryGetDecoration(compiler, input.id, spv::DecorationLocation).value_or(0);
            param.type = _GetOrCreateType(ctx, input.base_type_id);
            desc.stageInput.push_back(std::move(param));
        }
        desc.stageOutput.reserve(resources.stage_outputs.size());
        for (const auto& output : resources.stage_outputs) {
            SpirvParameterDesc param{};
            param.name = _ResolveName(compiler, output.id);
            param.location = _TryGetDecoration(compiler, output.id, spv::DecorationLocation).value_or(0);
            param.type = _GetOrCreateType(ctx, output.base_type_id);
            desc.stageOutput.push_back(std::move(param));
        }
        const auto appendResources = [&](const spirv_cross::SmallVector<spirv_cross::Resource>& list, SpirvResourceKind kind) {
            for (const auto& res : list) {
                desc.resources.push_back(_BuildResourceBinding(ctx, res, kind));
            }
        };
        appendResources(resources.uniform_buffers, SpirvResourceKind::UniformBuffer);
        appendResources(resources.push_constant_buffers, SpirvResourceKind::PushConstant);
        appendResources(resources.storage_buffers, SpirvResourceKind::StorageBuffer);
        appendResources(resources.sampled_images, SpirvResourceKind::SampledImage);
        appendResources(resources.storage_images, SpirvResourceKind::StorageImage);
        appendResources(resources.separate_images, SpirvResourceKind::SeparateImage);
        appendResources(resources.separate_samplers, SpirvResourceKind::SeparateSampler);
        static_assert(sizeof(SpirvWorkgroupSize) == sizeof(entryPoint.workgroup_size), "Size mismatch between SpirvWorkgroupSize and entryPoint.workgroup_size");
        static_assert(std::is_trivially_copyable_v<SpirvWorkgroupSize>, "SpirvWorkgroupSize must be trivially copyable");
        static_assert(std::is_trivially_copyable_v<spirv_cross::SPIREntryPoint::WorkgroupSize>, "spirv_cross::SPIREntryPoint::WorkgroupSize must be trivially copyable");
        std::memcpy(&desc.workgroupSize, &entryPoint.workgroup_size, sizeof(SpirvWorkgroupSize));
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
