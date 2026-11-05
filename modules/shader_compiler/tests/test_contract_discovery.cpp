#include <radray/shader_compiler/contract_discovery.h>

#include <gtest/gtest.h>

namespace radray::shader_compiler {
namespace {

// Little-endian contract blob: one graphics entry, no keyword groups, and the given trailing hash.
vector<byte> MakeContractBlob(uint32_t magic, uint8_t kind, const shader::ContractHash& hash) {
    vector<byte> blob;
    const auto u8 = [&](uint8_t value) { blob.push_back(static_cast<byte>(value)); };
    const auto u16 = [&](uint16_t value) { u8(static_cast<uint8_t>(value)); u8(static_cast<uint8_t>(value >> 8)); };
    const auto u32 = [&](uint32_t value) { u16(static_cast<uint16_t>(value)); u16(static_cast<uint16_t>(value >> 16)); };
    u32(magic);
    u16(shader::kShaderContractWireSchemaVersion);
    u8(kind);
    u8(0);
    u16(0);
    u32(0);  // keyword groups
    u32(1);  // entries
    u32(6);
    for (const char character : std::string_view{"VSMain"}) {
        u8(static_cast<uint8_t>(character));
    }
    u8(static_cast<uint8_t>(shader::ShaderStage::Vertex));
    u8(0);
    u16(0);
    for (const uint8_t value : hash.Bytes) {
        u8(value);
    }
    return blob;
}

TEST(RadRayContractDiscovery, RejectsEmptyBlob) {
    const auto decoded = DecodeWireShaderContract({});

    EXPECT_FALSE(decoded.Succeeded());
    EXPECT_NE(decoded.Diagnostics.find("header"), string::npos);
}

TEST(RadRayContractDiscovery, RejectsUnknownMagic) {
    const auto blob = MakeContractBlob(shader::kShaderWireMagic, static_cast<uint8_t>(shader::ShaderKind::Graphics), {});

    const auto decoded = DecodeWireShaderContract(blob);

    EXPECT_FALSE(decoded.Succeeded());
    EXPECT_NE(decoded.Diagnostics.find("header"), string::npos);
}

TEST(RadRayContractDiscovery, RejectsUnknownKind) {
    const auto blob = MakeContractBlob(shader::kShaderContractWireMagic, 2, {});

    const auto decoded = DecodeWireShaderContract(blob);

    EXPECT_FALSE(decoded.Succeeded());
    EXPECT_NE(decoded.Diagnostics.find("kind"), string::npos);
}

TEST(RadRayContractDiscovery, RejectsHashThatDoesNotMatchContents) {
    const auto blob = MakeContractBlob(shader::kShaderContractWireMagic, static_cast<uint8_t>(shader::ShaderKind::Graphics), {});

    const auto decoded = DecodeWireShaderContract(blob);

    EXPECT_FALSE(decoded.Succeeded());
    EXPECT_NE(decoded.Diagnostics.find("hash"), string::npos);
}

}  // namespace
}  // namespace radray::shader_compiler
