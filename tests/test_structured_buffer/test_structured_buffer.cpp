#include <gtest/gtest.h>
#include <radray/structured_buffer.h>
#include <radray/basic_math.h>

using namespace radray;

TEST(StructuredBufferStorage, BasicBuilder) {
    StructuredBufferStorage::Builder builder;
    
    // Define a simple float3 type
    // Size = 12 bytes
    size_t float3Type = builder.AddType("float3", 12);
    
    // Define a struct containing two float3
    // struct Transform { float3 pos; float3 scale; };
    // Size = 24 bytes
    size_t transformType = builder.AddType("Transform", 24);
    builder.AddMemberForType(transformType, float3Type, "pos", 0);
    builder.AddMemberForType(transformType, float3Type, "scale", 12);
    
    // Add a root variable
    builder.AddRoot("obj", transformType);
    
    auto storageOpt = builder.Build();
    ASSERT_TRUE(storageOpt.has_value());
    auto& storage = storageOpt.value();
    
    // Verify root variable
    auto objView = storage.GetVar("obj");
    ASSERT_TRUE(objView.IsValid());
    EXPECT_EQ(objView.GetType()->GetName(), "Transform");
    EXPECT_EQ(objView.GetType()->GetSizeInBytes(), 24);
    
    // Verify members
    auto posView = objView.GetVar("pos");
    ASSERT_TRUE(posView.IsValid());
    EXPECT_EQ(posView.GetType()->GetName(), "float3");
    EXPECT_EQ(posView.GetOffset(), 0); // Relative to obj
    
    auto scaleView = objView.GetVar("scale");
    ASSERT_TRUE(scaleView.IsValid());
    EXPECT_EQ(scaleView.GetType()->GetName(), "float3");
    EXPECT_EQ(scaleView.GetOffset(), 12); // Relative to obj
}

TEST(StructuredBufferStorage, DataReadWrite) {
    StructuredBufferStorage::Builder builder;
    size_t floatType = builder.AddType("float", 4);
    size_t vec4Type = builder.AddType("float4", 16);
    
    builder.AddRoot("time", floatType);
    builder.AddRoot("color", vec4Type);
    
    auto storageOpt = builder.Build();
    ASSERT_TRUE(storageOpt.has_value());
    auto& storage = storageOpt.value();
    
    // Write float
    float timeVal = 123.456f;
    storage.GetVar("time").SetValue(timeVal);
    
    // Read back
    auto timeId = storage.GetVar("time").GetId();
    auto timeSpan = storage.GetSpan(timeId);
    EXPECT_EQ(timeSpan.size(), sizeof(float));
    float readTimeFromSpan;
    std::memcpy(&readTimeFromSpan, timeSpan.data(), sizeof(float));
    EXPECT_EQ(readTimeFromSpan, timeVal);

    // Also verify raw buffer access
    auto buffer = storage.GetData();
    float readTime;
    std::memcpy(&readTime, buffer.data() + storage.GetVar("time").GetOffset(), sizeof(float));
    EXPECT_EQ(readTime, timeVal);
    
    // Write Eigen vector
    Eigen::Vector4f colorVal(1.0f, 0.5f, 0.2f, 1.0f);
    storage.GetVar("color").SetValue(colorVal);
    
    // Read back
    float readColor[4];
    std::memcpy(readColor, buffer.data() + storage.GetVar("color").GetOffset(), sizeof(float) * 4);
    EXPECT_EQ(readColor[0], 1.0f);
    EXPECT_EQ(readColor[1], 0.5f);
    EXPECT_EQ(readColor[2], 0.2f);
    EXPECT_EQ(readColor[3], 1.0f);
}

TEST(StructuredBufferStorage, Alignment) {
    StructuredBufferStorage::Builder builder;
    builder.SetAlignment(256); // Common UBO alignment
    
    size_t floatType = builder.AddType("float", 4);
    
    builder.AddRoot("var1", floatType);
    builder.AddRoot("var2", floatType);
    
    auto storageOpt = builder.Build();
    ASSERT_TRUE(storageOpt.has_value());
    auto& storage = storageOpt.value();
    
    auto var1 = storage.GetVar("var1");
    auto var2 = storage.GetVar("var2");
    
    EXPECT_EQ(var1.GetOffset(), 0);
    EXPECT_EQ(var2.GetOffset(), 256);
    EXPECT_EQ(storage.GetData().size(), 512);
}

TEST(StructuredBufferStorage, InvalidBuild) {
    StructuredBufferStorage::Builder builder;
    size_t floatType = builder.AddType("float", 4);
    size_t structType = builder.AddType("Struct", 4); // Too small
    
    // Member offset 4 + size 4 = 8 > 4
    builder.AddMemberForType(structType, floatType, "m1", 4);
    
    builder.AddRoot("root", structType);
    
    EXPECT_FALSE(builder.IsValid());
    EXPECT_FALSE(builder.Build().has_value());
}
