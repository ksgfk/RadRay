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
    uint32_t X = 0, Y = 0, Z = 0;
    uint32_t IdX = 0, IdY = 0, IdZ = 0;
    uint32_t Constant = 0;
};

struct SpirvStructDesc;

struct SpirvTypeDesc {
    SpirvBaseType Base{SpirvBaseType::UNKNOWN};
    const struct SpirvStructDesc* StructInfo{nullptr};
    uint32_t BitWidth{0};
    uint32_t VectorSize{0};
    uint32_t ColumnCount{0};
    uint32_t ArraySize{0};
};

struct SpirvStructMemberDesc {
    string Name{};
    const SpirvTypeDesc* Type{nullptr};
    uint32_t Offset{0};
    uint32_t Size{0};
};

struct SpirvStructDesc {
    string Name{};
    vector<SpirvStructMemberDesc> Members{};
    uint32_t Size{0};
};

struct SpirvParameterDesc {
    string Name{};
    const SpirvTypeDesc* Type{nullptr};
    uint32_t Location{0};
};

struct SpirvResourceBindingDesc {
    string Name{};
    SpirvResourceKind Kind{SpirvResourceKind::UNKNOWN};
    uint32_t Binding{0};
    uint32_t DescriptorSet{0};
    uint32_t ArraySize{0};
    const SpirvTypeDesc* ValueType{nullptr};
};

class SpirvShaderDesc {
public:
    vector<SpirvStructDesc> Structs;
    vector<SpirvTypeDesc> Types;
    vector<SpirvParameterDesc> StageInput;
    vector<SpirvParameterDesc> StageOutput;
    vector<SpirvResourceBindingDesc> Resources;
    SpirvWorkgroupSize WorkgroupSize;
};

}  // namespace radray::render

#ifdef RADRAY_ENABLE_SPIRV_CROSS

namespace radray::render {

std::optional<SpirvShaderDesc> ReflectSpirv(std::string_view entryPointName, ShaderStage stage, std::span<const byte> data);

}  // namespace radray::render

#endif
