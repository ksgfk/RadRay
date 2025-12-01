#pragma once

#include <optional>
#include <string>
#include <vector>

#include <radray/render/common.h>

namespace radray::render {

enum class SpirvBaseType {
    UNKNOWN,
    Bool,
    Int,
    UInt,
    Float,
    Double,
    Struct,
    Image,
    SampledImage,
    Sampler,
    AccelerationStructure
};

enum class SpirvResourceKind {
    UNKNOWN,
    UniformBuffer,
    PushConstant,
    StorageBuffer,
    SampledImage,
    StorageImage,
    SeparateImage,
    SeparateSampler
};

struct SpirvWorkgroupSize {
    uint32_t x = 0, y = 0, z = 0;
    uint32_t id_x = 0, id_y = 0, id_z = 0;
    uint32_t constant = 0;
};

struct SpirvStructDesc;

struct SpirvTypeDesc {
    SpirvBaseType base{SpirvBaseType::UNKNOWN};
    const struct SpirvStructDesc* structInfo{nullptr};
    uint32_t bitWidth{0};
    uint32_t vectorSize{0};
    uint32_t columnCount{0};
    uint32_t arraySize{0};
};

struct SpirvStructMemberDesc {
    string name{};
    const SpirvTypeDesc* type{nullptr};
    uint32_t offset{0};
    uint32_t size{0};
};

struct SpirvStructDesc {
    string name{};
    vector<SpirvStructMemberDesc> members{};
    uint32_t size{0};
};

struct SpirvParameterDesc {
    string name{};
    const SpirvTypeDesc* type{nullptr};
    uint32_t location{0};
};

struct SpirvResourceBindingDesc {
    string name{};
    SpirvResourceKind kind{SpirvResourceKind::UNKNOWN};
    uint32_t binding{0};
    uint32_t descriptorSet{0};
    uint32_t arraySize{0};
    const SpirvTypeDesc* valueType{nullptr};
};

class SpirvShaderDesc {
public:
    vector<SpirvStructDesc> structs;
    vector<SpirvTypeDesc> types;
    vector<SpirvParameterDesc> stageInput;
    vector<SpirvParameterDesc> stageOutput;
    vector<SpirvResourceBindingDesc> resources;
    SpirvWorkgroupSize workgroupSize;
};

}  // namespace radray::render

#ifdef RADRAY_ENABLE_SPIRV_CROSS

namespace radray::render {

std::optional<SpirvShaderDesc> ReflectSpirv(std::string_view entryPointName, ShaderStage stage, std::span<const byte> data);

}  // namespace radray::render

#endif
