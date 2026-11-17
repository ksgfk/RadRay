#include <radray/shader/shader_compiler_contract.h>

#include <gtest/gtest.h>

namespace radray::shader {
namespace {

// Little-endian contract blob with a single compute entry and the supplied digest.
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
    for (const char character : std::string_view{"CSMain"}) {
        u8(static_cast<uint8_t>(character));
    }
    u8(static_cast<uint8_t>(shader::ShaderStage::Compute));
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
    const auto blob = MakeContractBlob(shader::kShaderWireMagic, static_cast<uint8_t>(shader::ShaderKind::Compute), {});

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
    const auto blob = MakeContractBlob(shader::kShaderContractWireMagic, static_cast<uint8_t>(shader::ShaderKind::Compute), {});

    const auto decoded = DecodeWireShaderContract(blob);

    EXPECT_FALSE(decoded.Succeeded());
    EXPECT_NE(decoded.Diagnostics.find("hash"), string::npos);
}

TEST(RadRayContractDiscovery, AcceptsPublishedComputeDigest) {
    // Digest from the schema 10 compute fixture at 7f53d3e1.
    constexpr ContractHash digest{{0x0b, 0xce, 0x77, 0x13, 0x88, 0x92, 0xf0, 0x2a,
                                   0xa3, 0x65, 0xbb, 0x0b, 0x59, 0xa5, 0xf6, 0x20}};
    const auto blob = MakeContractBlob(kShaderContractWireMagic, static_cast<uint8_t>(ShaderKind::Compute), digest);

    const auto decoded = DecodeWireShaderContract(blob);

    ASSERT_TRUE(decoded.Succeeded()) << decoded.Diagnostics;
    EXPECT_EQ(decoded.Value->Hash, digest);
}

}  // namespace
}  // namespace radray::shader
