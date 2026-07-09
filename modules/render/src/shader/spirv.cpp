#include <radray/render/shader/spirv.h>

#include <radray/render/shader/hlsl.h>
#include <radray/json.h>
#include <radray/logger.h>

namespace radray::render {

// ResourceBindType SpirvResourceBinding::MapResourceBindType() const noexcept {
//     const bool isBufferImage = ImageInfo.has_value() && ImageInfo->Dim == SpirvImageDim::Buffer;
//     switch (Kind) {
//         case SpirvResourceKind::UniformBuffer:
//             return ResourceBindType::CBuffer;
//         case SpirvResourceKind::StorageBuffer:
//             return (ReadOnly && !WriteOnly) ? ResourceBindType::Buffer : ResourceBindType::RWBuffer;
//         case SpirvResourceKind::SampledImage:
//         case SpirvResourceKind::SeparateImage:
//             return isBufferImage ? ResourceBindType::TexelBuffer : ResourceBindType::Texture;
//         case SpirvResourceKind::SeparateSampler:
//             return ResourceBindType::Sampler;
//         case SpirvResourceKind::StorageImage:
//             return isBufferImage ? ResourceBindType::RWTexelBuffer : ResourceBindType::RWTexture;
//         case SpirvResourceKind::AccelerationStructure:
//             return ResourceBindType::AccelerationStructure;
//         default:
//             return ResourceBindType::UNKNOWN;
//     }
// }

// ====================== SpirvShaderDesc <-> JSON ======================

namespace {

// 枚举以底层整数值存储。反序列化时越界回落到默认 (通常 UNKNOWN=0)。
template <typename E>
uint64_t EnumToU(E v) noexcept {
    return static_cast<uint64_t>(static_cast<std::underlying_type_t<E>>(v));
}

template <typename E>
E UToEnum(uint64_t v) noexcept {
    return static_cast<E>(static_cast<std::underlying_type_t<E>>(v));
}

void WriteSpirvType(JsonRef obj, const SpirvTypeInfo& t) {
    obj.AddString("Name", t.Name);
    obj.AddUint("BaseType", EnumToU(t.BaseType));
    obj.AddUint("VectorSize", t.VectorSize);
    obj.AddUint("Columns", t.Columns);
    obj.AddUint("ArraySize", t.ArraySize);
    obj.AddUint("ArrayStride", t.ArrayStride);
    obj.AddUint("MatrixStride", t.MatrixStride);
    obj.AddUint("Size", t.Size);
    obj.AddBool("RowMajor", t.RowMajor);
    JsonRef members = obj.AddArray("Members");
    for (const SpirvTypeMember& m : t.Members) {
        JsonRef mo = members.AppendObject();
        mo.AddString("Name", m.Name);
        mo.AddUint("Offset", m.Offset);
        mo.AddUint("Size", m.Size);
        mo.AddUint("TypeIndex", m.TypeIndex);
    }
}

void WriteSpirvBinding(JsonRef obj, const SpirvResourceBinding& r) {
    obj.AddString("Name", r.Name);
    obj.AddUint("Kind", EnumToU(r.Kind));
    obj.AddUint("Set", r.Set);
    obj.AddUint("Binding", r.Binding);
    if (r.HlslRegister.has_value()) {
        obj.AddUint("HlslRegister", r.HlslRegister.value());
    }
    if (r.HlslSpace.has_value()) {
        obj.AddUint("HlslSpace", r.HlslSpace.value());
    }
    obj.AddUint("ArraySize", r.ArraySize);
    obj.AddUint("TypeIndex", r.TypeIndex);
    obj.AddUint("UniformBufferSize", r.UniformBufferSize);
    obj.AddBool("ReadOnly", r.ReadOnly);
    obj.AddBool("WriteOnly", r.WriteOnly);
    obj.AddBool("IsViewInHlsl", r.IsViewInHlsl);
    obj.AddBool("IsUnboundedArray", r.IsUnboundedArray);
    if (r.ImageInfo.has_value()) {
        const SpirvImageInfo& img = r.ImageInfo.value();
        JsonRef io = obj.AddObject("ImageInfo");
        io.AddUint("Dim", EnumToU(img.Dim));
        io.AddBool("Arrayed", img.Arrayed);
        io.AddBool("Multisampled", img.Multisampled);
        io.AddBool("Depth", img.Depth);
        io.AddUint("SampledType", img.SampledType);
    }
}

void ReadSpirvType(const JsonValue& obj, SpirvTypeInfo& t) {
    t.Name = string{obj["Name"].AsString()};
    t.BaseType = UToEnum<SpirvBaseType>(obj["BaseType"].AsUint());
    t.VectorSize = static_cast<uint32_t>(obj["VectorSize"].AsUint(1));
    t.Columns = static_cast<uint32_t>(obj["Columns"].AsUint(1));
    t.ArraySize = static_cast<uint32_t>(obj["ArraySize"].AsUint());
    t.ArrayStride = static_cast<uint32_t>(obj["ArrayStride"].AsUint());
    t.MatrixStride = static_cast<uint32_t>(obj["MatrixStride"].AsUint());
    t.Size = static_cast<uint32_t>(obj["Size"].AsUint());
    t.RowMajor = obj["RowMajor"].AsBool();
    JsonValue members = obj["Members"];
    const size_t n = members.Size();
    t.Members.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        JsonValue mo = members.At(i);
        SpirvTypeMember m{};
        m.Name = string{mo["Name"].AsString()};
        m.Offset = static_cast<uint32_t>(mo["Offset"].AsUint());
        m.Size = static_cast<uint32_t>(mo["Size"].AsUint());
        m.TypeIndex = static_cast<uint32_t>(mo["TypeIndex"].AsUint());
        t.Members.push_back(std::move(m));
    }
}

void ReadSpirvBinding(const JsonValue& obj, SpirvResourceBinding& r) {
    r.Name = string{obj["Name"].AsString()};
    r.Kind = UToEnum<SpirvResourceKind>(obj["Kind"].AsUint());
    r.Set = static_cast<uint32_t>(obj["Set"].AsUint());
    r.Binding = static_cast<uint32_t>(obj["Binding"].AsUint());
    if (obj.Has("HlslRegister")) {
        r.HlslRegister = static_cast<uint32_t>(obj["HlslRegister"].AsUint());
    }
    if (obj.Has("HlslSpace")) {
        r.HlslSpace = static_cast<uint32_t>(obj["HlslSpace"].AsUint());
    }
    r.ArraySize = static_cast<uint32_t>(obj["ArraySize"].AsUint());
    r.TypeIndex = static_cast<uint32_t>(obj["TypeIndex"].AsUint());
    r.UniformBufferSize = static_cast<uint32_t>(obj["UniformBufferSize"].AsUint());
    r.ReadOnly = obj["ReadOnly"].AsBool(true);
    r.WriteOnly = obj["WriteOnly"].AsBool(false);
    r.IsViewInHlsl = obj["IsViewInHlsl"].AsBool(false);
    r.IsUnboundedArray = obj["IsUnboundedArray"].AsBool(false);
    if (obj.Has("ImageInfo")) {
        JsonValue io = obj["ImageInfo"];
        SpirvImageInfo img{};
        img.Dim = UToEnum<SpirvImageDim>(io["Dim"].AsUint());
        img.Arrayed = io["Arrayed"].AsBool();
        img.Multisampled = io["Multisampled"].AsBool();
        img.Depth = io["Depth"].AsBool();
        img.SampledType = static_cast<uint32_t>(io["SampledType"].AsUint());
        r.ImageInfo = img;
    }
}

}  // namespace

std::optional<string> SerializeSpirvShaderDesc(const SpirvShaderDesc& desc) noexcept {
    JsonWriter w{};
    if (!w.IsValid()) {
        return std::nullopt;
    }
    JsonRef root = w.RootObject();
    root.AddUint("FormatVersion", kReflectionFormatVersion);
    root.AddString("Kind", "spirv");

    JsonRef types = root.AddArray("Types");
    for (const SpirvTypeInfo& t : desc.Types) {
        WriteSpirvType(types.AppendObject(), t);
    }
    JsonRef vinputs = root.AddArray("VertexInputs");
    for (const SpirvVertexInput& v : desc.VertexInputs) {
        JsonRef vo = vinputs.AppendObject();
        vo.AddString("Name", v.Name);
        vo.AddUint("Location", v.Location);
        vo.AddUint("TypeIndex", v.TypeIndex);
    }
    JsonRef binds = root.AddArray("ResourceBindings");
    for (const SpirvResourceBinding& r : desc.ResourceBindings) {
        WriteSpirvBinding(binds.AppendObject(), r);
    }
    JsonRef ranges = root.AddArray("ConstantRanges");
    for (const SpirvPushConstantRange& c : desc.ConstantRanges) {
        JsonRef co = ranges.AppendObject();
        co.AddString("Name", c.Name);
        co.AddUint("Offset", c.Offset);
        co.AddUint("Size", c.Size);
        co.AddUint("TypeIndex", c.TypeIndex);
        co.AddBool("IsViewInHlsl", c.IsViewInHlsl);
    }
    if (desc.ComputeInfo.has_value()) {
        const SpirvComputeInfo& ci = desc.ComputeInfo.value();
        JsonRef co = root.AddObject("ComputeInfo");
        co.AddUint("LocalSizeX", ci.LocalSizeX);
        co.AddUint("LocalSizeY", ci.LocalSizeY);
        co.AddUint("LocalSizeZ", ci.LocalSizeZ);
    }
    return w.Write(true);
}

std::optional<SpirvShaderDesc> DeserializeSpirvShaderDesc(std::string_view json) noexcept {
    std::optional<JsonDocument> docOpt = JsonDocument::Parse(json);
    if (!docOpt.has_value()) {
        RADRAY_ERR_LOG("DeserializeSpirvShaderDesc: JSON parse failed");
        return std::nullopt;
    }
    JsonValue root = docOpt->Root();
    if (!root.IsObject()) {
        RADRAY_ERR_LOG("DeserializeSpirvShaderDesc: root is not object");
        return std::nullopt;
    }
    const uint32_t ver = static_cast<uint32_t>(root["FormatVersion"].AsUint());
    if (ver != kReflectionFormatVersion) {
        RADRAY_ERR_LOG("DeserializeSpirvShaderDesc: format version {} != expected {}", ver, kReflectionFormatVersion);
        return std::nullopt;
    }

    SpirvShaderDesc desc{};
    JsonValue types = root["Types"];
    desc.Types.reserve(types.Size());
    for (size_t i = 0; i < types.Size(); ++i) {
        SpirvTypeInfo t{};
        ReadSpirvType(types.At(i), t);
        desc.Types.push_back(std::move(t));
    }
    JsonValue vinputs = root["VertexInputs"];
    desc.VertexInputs.reserve(vinputs.Size());
    for (size_t i = 0; i < vinputs.Size(); ++i) {
        JsonValue vo = vinputs.At(i);
        SpirvVertexInput v{};
        v.Name = string{vo["Name"].AsString()};
        v.Location = static_cast<uint32_t>(vo["Location"].AsUint());
        v.TypeIndex = static_cast<uint32_t>(vo["TypeIndex"].AsUint());
        desc.VertexInputs.push_back(std::move(v));
    }
    JsonValue binds = root["ResourceBindings"];
    desc.ResourceBindings.reserve(binds.Size());
    for (size_t i = 0; i < binds.Size(); ++i) {
        SpirvResourceBinding r{};
        ReadSpirvBinding(binds.At(i), r);
        desc.ResourceBindings.push_back(std::move(r));
    }
    JsonValue ranges = root["ConstantRanges"];
    desc.ConstantRanges.reserve(ranges.Size());
    for (size_t i = 0; i < ranges.Size(); ++i) {
        JsonValue co = ranges.At(i);
        SpirvPushConstantRange c{};
        c.Name = string{co["Name"].AsString()};
        c.Offset = static_cast<uint32_t>(co["Offset"].AsUint());
        c.Size = static_cast<uint32_t>(co["Size"].AsUint());
        c.TypeIndex = static_cast<uint32_t>(co["TypeIndex"].AsUint());
        c.IsViewInHlsl = co["IsViewInHlsl"].AsBool();
        desc.ConstantRanges.push_back(std::move(c));
    }
    if (root.Has("ComputeInfo")) {
        JsonValue co = root["ComputeInfo"];
        SpirvComputeInfo ci{};
        ci.LocalSizeX = static_cast<uint32_t>(co["LocalSizeX"].AsUint(1));
        ci.LocalSizeY = static_cast<uint32_t>(co["LocalSizeY"].AsUint(1));
        ci.LocalSizeZ = static_cast<uint32_t>(co["LocalSizeZ"].AsUint(1));
        desc.ComputeInfo = ci;
    }
    return desc;
}

}  // namespace radray::render
