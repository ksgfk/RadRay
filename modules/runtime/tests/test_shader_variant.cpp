#include <gtest/gtest.h>

#include <radray/runtime/shader_variant.h>

using namespace radray;

namespace {

string SignatureOf(const ShaderVariantKey& key) {
    string signature = "shader";
    key.AppendSignature(signature);
    return signature;
}

}  // namespace

TEST(ShaderVariantKeyTest, DefineValueParticipatesInSignature) {
    ShaderDefine defineOff{.Name = "ALPHA_TEST", .Value = "0"};
    ShaderDefine defineOn{.Name = "ALPHA_TEST", .Value = "1"};

    ShaderVariantKey off{std::span<const ShaderDefine>{&defineOff, 1}};
    ShaderVariantKey on{std::span<const ShaderDefine>{&defineOn, 1}};

    EXPECT_NE(off, on);
    EXPECT_NE(SignatureOf(off), SignatureOf(on));
}

TEST(ShaderVariantKeyTest, DefineOrderDoesNotParticipateInSignature) {
    ShaderDefine definesA[]{
        {.Name = "SHADOW_CASTER", .Value = "1"},
        {.Name = "ALPHA_TEST", .Value = "1"},
    };
    ShaderDefine definesB[]{
        {.Name = "ALPHA_TEST", .Value = "1"},
        {.Name = "SHADOW_CASTER", .Value = "1"},
    };

    ShaderVariantKey lhs{definesA};
    ShaderVariantKey rhs{definesB};

    EXPECT_EQ(lhs, rhs);
    EXPECT_EQ(SignatureOf(lhs), SignatureOf(rhs));
    EXPECT_EQ(ShaderVariantKeyHash{}(lhs), ShaderVariantKeyHash{}(rhs));
}
