#import <Metal/Metal.h>

#include <iostream>

#include <gtest/gtest.h>
#include <radray/render/backend/metal_helper.h>

using namespace radray::render;
using namespace radray::render::metal;

class MslReflTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        _device = MTLCreateSystemDefaultDevice();
        ASSERT_NE(_device, nil) << "No Metal device available";
    }

    static void TearDownTestSuite() {
        _device = nil;
    }

    static id<MTLDevice> _device;

    static id<MTLLibrary> CompileLibrary(const char* src) {
        @autoreleasepool {
            NSError* error = nil;
            NSString* s = [NSString stringWithUTF8String:src];
            id<MTLLibrary> lib = [_device newLibraryWithSource:s options:nil error:&error];
            if (error && !lib) {
                std::cerr << "MTLLibrary error: "
                          << [[error localizedDescription] UTF8String] << "\n";
            }
            return lib;
        }
    }
};

id<MTLDevice> MslReflTest::_device = nil;

// ---- Pure MSL sources ----

const char* MSL_COMPUTE = R"(
#include <metal_stdlib>
using namespace metal;

struct Params {
    float scale;
    uint count;
};

kernel void CSMain(
    constant Params& params [[buffer(0)]],
    device float4* output   [[buffer(1)]],
    uint tid                [[thread_position_in_grid]])
{
    if (tid < params.count)
        output[tid] = float4(tid * params.scale, 0, 0, 1.0f);
}
)";

const char* MSL_RENDER = R"(
#include <metal_stdlib>
using namespace metal;

struct PreObject {
    float4x4 mvp;
};

struct VertexIn {
    float3 pos [[attribute(0)]];
    float2 uv  [[attribute(1)]];
};

struct VertexOut {
    float4 pos [[position]];
    float2 uv;
};

vertex VertexOut VSMain(
    VertexIn in              [[stage_in]],
    constant PreObject& obj  [[buffer(16)]])
{
    VertexOut out;
    out.pos = obj.mvp * float4(in.pos, 1.0f);
    out.uv = in.uv;
    return out;
}

fragment float4 PSMain(
    VertexOut in                    [[stage_in]],
    texture2d<float> tex            [[texture(0)]],
    sampler samp                    [[sampler(0)]])
{
    return tex.sample(samp, in.uv);
}
)";

TEST_F(MslReflTest, ComputeShader) {
    auto lib = CompileLibrary(MSL_COMPUTE);
    ASSERT_NE(lib, nil);
    id<MTLFunction> fn = [lib newFunctionWithName:@"CSMain"];
    ASSERT_NE(fn, nil);

    NSError* error = nil;
    MTLComputePipelineReflection* reflection = nil;
    id<MTLComputePipelineState> pso = [_device
        newComputePipelineStateWithFunction:fn
        options:MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo
        reflection:&reflection
        error:&error];
    ASSERT_NE(pso, nil) << [[error localizedDescription] UTF8String];

    auto refl = DumpPsoReflection(reflection);
    ASSERT_TRUE(refl.has_value());
    EXPECT_FALSE(refl->Arguments.empty());

    bool hasBuffer = false;
    for (auto& arg : refl->Arguments) {
        std::cout << "  arg: " << arg.Name
                  << " stage=" << format_as(arg.Stage)
                  << " type=" << format_as(arg.Type)
                  << " index=" << arg.Index
                  << " active=" << arg.IsActive << "\n";
        EXPECT_EQ(arg.Stage, MslStage::Compute);
        if (arg.Type == MslArgumentType::Buffer) {
            hasBuffer = true;
            EXPECT_GT(arg.BufferDataSize, 0u);
        }
    }
    EXPECT_TRUE(hasBuffer);
}

TEST_F(MslReflTest, ComputeCBufferStruct) {
    auto lib = CompileLibrary(MSL_COMPUTE);
    ASSERT_NE(lib, nil);
    id<MTLFunction> fn = [lib newFunctionWithName:@"CSMain"];
    ASSERT_NE(fn, nil);

    NSError* error = nil;
    MTLComputePipelineReflection* reflection = nil;
    id<MTLComputePipelineState> pso = [_device
        newComputePipelineStateWithFunction:fn
        options:MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo
        reflection:&reflection
        error:&error];
    ASSERT_NE(pso, nil);

    auto refl = DumpPsoReflection(reflection);
    ASSERT_TRUE(refl.has_value());

    bool foundStructRefl = false;
    for (auto& arg : refl->Arguments) {
        if (arg.Type == MslArgumentType::Buffer && arg.BufferStructTypeIndex != UINT32_MAX) {
            foundStructRefl = true;
            ASSERT_LT(arg.BufferStructTypeIndex, refl->StructTypes.size());
            auto& st = refl->StructTypes[arg.BufferStructTypeIndex];
            std::cout << "  struct members (" << st.Members.size() << "):\n";
            for (auto& m : st.Members) {
                std::cout << "    " << m.Name << " offset=" << m.Offset
                          << " type=" << format_as(m.DataType) << "\n";
            }
            EXPECT_FALSE(st.Members.empty());
        }
    }
    EXPECT_TRUE(foundStructRefl) << "should find at least one buffer with struct type info";
}

TEST_F(MslReflTest, RenderPipeline) {
    auto lib = CompileLibrary(MSL_RENDER);
    ASSERT_NE(lib, nil);
    id<MTLFunction> vsFn = [lib newFunctionWithName:@"VSMain"];
    id<MTLFunction> psFn = [lib newFunctionWithName:@"PSMain"];
    ASSERT_NE(vsFn, nil);
    ASSERT_NE(psFn, nil);

    MTLVertexDescriptor* vertDesc = [MTLVertexDescriptor new];
    // attribute 0: float3 pos
    vertDesc.attributes[0].format = MTLVertexFormatFloat3;
    vertDesc.attributes[0].offset = 0;
    vertDesc.attributes[0].bufferIndex = 0;
    // attribute 1: float2 uv
    vertDesc.attributes[1].format = MTLVertexFormatFloat2;
    vertDesc.attributes[1].offset = 12;
    vertDesc.attributes[1].bufferIndex = 0;
    vertDesc.layouts[0].stride = 20;
    vertDesc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

    MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
    desc.vertexFunction = vsFn;
    desc.fragmentFunction = psFn;
    desc.vertexDescriptor = vertDesc;
    desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;

    NSError* error = nil;
    MTLRenderPipelineReflection* reflection = nil;
    id<MTLRenderPipelineState> pso = [_device
        newRenderPipelineStateWithDescriptor:desc
        options:MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo
        reflection:&reflection
        error:&error];
    ASSERT_NE(pso, nil) << [[error localizedDescription] UTF8String];

    auto refl = DumpPsoReflection(reflection);
    ASSERT_TRUE(refl.has_value());
    EXPECT_FALSE(refl->Arguments.empty());

    bool hasVertex = false;
    bool hasFragment = false;
    bool hasTexture = false;
    bool hasSampler = false;
    for (auto& arg : refl->Arguments) {
        std::cout << "  arg: " << arg.Name
                  << " stage=" << format_as(arg.Stage)
                  << " type=" << format_as(arg.Type)
                  << " index=" << arg.Index
                  << " active=" << arg.IsActive << "\n";
        if (arg.Stage == MslStage::Vertex) hasVertex = true;
        if (arg.Stage == MslStage::Fragment) hasFragment = true;
        if (arg.Type == MslArgumentType::Texture) {
            hasTexture = true;
            EXPECT_EQ(arg.TextureType, MslTextureType::Tex2D);
        }
        if (arg.Type == MslArgumentType::Sampler) hasSampler = true;
    }
    EXPECT_TRUE(hasVertex) << "should have vertex stage arguments";
    EXPECT_TRUE(hasFragment) << "should have fragment stage arguments";
    EXPECT_TRUE(hasTexture) << "should have texture argument";
    EXPECT_TRUE(hasSampler) << "should have sampler argument";
}
