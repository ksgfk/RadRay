#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <string_view>

#include <fmt/format.h>

#include <radray/environment.h>
#include <radray/file.h>
#include <radray/render/dxc.h>
#include <radray/runtime/shader_asset_template.h>

namespace radray {
namespace {

/// 仓库根。环境变量 (ctest 注入) 优先, 缺失时回退到配置期编进来的路径。
///
/// 【为何要有回退】: 直接跑 exe (调试器、手动 --gtest_filter 复现) 时环境是空的。
/// 既不能退化成相对路径 "shaderlib" —— 那样只在 cwd 恰好是仓库根时通过, 把配置
/// 错误伪装成通过 —— 也不该让这些用例只能经 ctest 运行。
std::filesystem::path GetProjectRoot() {
    const string fromEnv = GetEnv("RADRAY_PROJECT_DIR");
    if (!fromEnv.empty()) {
        return std::filesystem::path{fromEnv};
    }
#if defined(RADRAY_PROJECT_DIR_DEFAULT)
    return std::filesystem::path{RADRAY_PROJECT_DIR_DEFAULT};
#else
    return {};
#endif
}

std::filesystem::path GetShaderRoot() {
    const std::filesystem::path root = GetProjectRoot();
    EXPECT_FALSE(root.empty()) << "the project root is unknown";
    return root / "shaderlib";
}

/// 生成结果落盘所需的临时目录。cook 会在 manifest 同名目录下写产物, 故不能直接
/// 往仓库的 shaderlib 里写。
class ScopedDirectory {
public:
    ScopedDirectory() {
        static std::atomic<uint32_t> counter{0};
        std::error_code error;
        _path = std::filesystem::temp_directory_path(error) /
                fmt::format("radray_shader_template_{}", counter.fetch_add(1));
        std::filesystem::remove_all(_path, error);
        std::filesystem::create_directories(_path, error);
        if (error) {
            _path.clear();
        }
    }
    ~ScopedDirectory() {
        std::error_code error;
        if (!_path.empty()) {
            std::filesystem::remove_all(_path, error);
        }
    }
    ScopedDirectory(const ScopedDirectory&) = delete;
    ScopedDirectory& operator=(const ScopedDirectory&) = delete;

    bool IsValid() const noexcept { return !_path.empty(); }
    const std::filesystem::path& Path() const noexcept { return _path; }

private:
    std::filesystem::path _path;
};

string JoinDiagnostics(std::span<const ShaderAssetDiagnostic> diagnostics) {
    string text;
    for (const ShaderAssetDiagnostic& diagnostic : diagnostics) {
        if (!text.empty()) {
            text += '\n';
        }
        text += diagnostic.ToString();
    }
    return text;
}

bool HasTodoContaining(const ShaderAssetTemplate& value, std::string_view fragment) {
    for (const ShaderTemplateTodo& todo : value.Todos) {
        if (todo.Path.find(fragment) != string::npos) {
            return true;
        }
    }
    return false;
}

Nullable<const ShaderBindingDesc*> FindBinding(
    const ShaderPassDesc& pass,
    uint32_t group,
    uint32_t binding) {
    return pass.FindBinding(group, binding);
}

/// 造一段"预处理输出"。真实输入来自 dxc -P, 但解析器只依赖 #line 指令的形状,
/// 故手写更能精确定位被测语法。
string MakeLineDirective(std::string_view path) {
    return fmt::format("#line 1 \"{}\"\n", path);
}

}  // namespace

// ==================== keyword pragma 解析 (无需 DXC) ====================

TEST(ShaderKeywordPragmaTest, ParsesSingleKeywordGroup) {
    ShaderAssetDiagnostic diag;
    const std::optional<vector<ShaderKeywordGroupDesc>> groups = ParseShaderKeywordPragmas(
        "#pragma radray_keyword_group(BaseColorMap, _BASECOLOR_MAP) stages(Pixel)\n",
        {},
        diag);
    ASSERT_TRUE(groups.has_value()) << diag.ToString();
    ASSERT_EQ(groups->size(), 1u);
    EXPECT_EQ((*groups)[0].Name, "BaseColorMap");
    ASSERT_EQ((*groups)[0].Keywords.size(), 1u);
    EXPECT_EQ((*groups)[0].Keywords[0], "_BASECOLOR_MAP");
    EXPECT_TRUE((*groups)[0].IsOptional);
    EXPECT_EQ((*groups)[0].Stages, render::ShaderStages{render::ShaderStage::Pixel});
}

TEST(ShaderKeywordPragmaTest, ParsesMutuallyExclusiveKeywords) {
    ShaderAssetDiagnostic diag;
    const std::optional<vector<ShaderKeywordGroupDesc>> groups = ParseShaderKeywordPragmas(
        "#pragma radray_keyword_group(AlphaMode, _ALPHATEST_ON, _ALPHABLEND_ON) stages(Pixel)\n",
        {},
        diag);
    ASSERT_TRUE(groups.has_value()) << diag.ToString();
    ASSERT_EQ(groups->size(), 1u);
    ASSERT_EQ((*groups)[0].Keywords.size(), 2u);
    EXPECT_EQ((*groups)[0].Keywords[0], "_ALPHATEST_ON");
    EXPECT_EQ((*groups)[0].Keywords[1], "_ALPHABLEND_ON");
}

TEST(ShaderKeywordPragmaTest, StagesDefaultsToGraphicsAndAcceptsMultiple) {
    ShaderAssetDiagnostic diag;
    const std::optional<vector<ShaderKeywordGroupDesc>> groups = ParseShaderKeywordPragmas(
        "#pragma radray_keyword_group(NoStages, _A)\n"
        "#pragma radray_keyword_group(Both, _B) stages(Vertex, Pixel)\n"
        "#pragma radray_keyword_group(Cs, _C) stages(Compute)\n",
        {},
        diag);
    ASSERT_TRUE(groups.has_value()) << diag.ToString();
    ASSERT_EQ(groups->size(), 3u);
    EXPECT_EQ((*groups)[0].Stages, render::ShaderStages{render::ShaderStage::Graphics});
    EXPECT_EQ(
        (*groups)[1].Stages,
        render::ShaderStages{render::ShaderStage::Vertex} | render::ShaderStage::Pixel);
    EXPECT_EQ((*groups)[2].Stages, render::ShaderStages{render::ShaderStage::Compute});
}

TEST(ShaderKeywordPragmaTest, RequiredModifierClearsIsOptional) {
    ShaderAssetDiagnostic diag;
    const std::optional<vector<ShaderKeywordGroupDesc>> groups = ParseShaderKeywordPragmas(
        "#pragma radray_keyword_group(Lighting, _LIT, _UNLIT) stages(Vertex, Pixel) required\n",
        {},
        diag);
    ASSERT_TRUE(groups.has_value()) << diag.ToString();
    ASSERT_EQ(groups->size(), 1u);
    EXPECT_FALSE((*groups)[0].IsOptional);
}

TEST(ShaderKeywordPragmaTest, ModifierOrderDoesNotMatter) {
    ShaderAssetDiagnostic diag;
    const std::optional<vector<ShaderKeywordGroupDesc>> groups = ParseShaderKeywordPragmas(
        "#pragma radray_keyword_group(A, _A) required stages(Pixel)\n",
        {},
        diag);
    ASSERT_TRUE(groups.has_value()) << diag.ToString();
    ASSERT_EQ(groups->size(), 1u);
    EXPECT_FALSE((*groups)[0].IsOptional);
    EXPECT_EQ((*groups)[0].Stages, render::ShaderStages{render::ShaderStage::Pixel});
}

/// entrySourcePath 留空 = 采纳整条 include 链的声明。
TEST(ShaderKeywordPragmaTest, AcceptsDeclarationsFromTheWholeIncludeChain) {
    const string text =
        MakeLineDirective("C:/repo/shaderlib/entry.hlsl") +
        "#pragma radray_keyword_group(FromEntry, _FROM_ENTRY)\n" +
        MakeLineDirective("C:/repo/shaderlib/other.hlsl") +
        "#pragma radray_keyword_group(FromInclude, _FROM_INCLUDE)\n";

    ShaderAssetDiagnostic diag;
    const std::optional<vector<ShaderKeywordGroupDesc>> groups =
        ParseShaderKeywordPragmas(text, {}, diag);
    ASSERT_TRUE(groups.has_value()) << diag.ToString();
    ASSERT_EQ(groups->size(), 2u);
    EXPECT_EQ((*groups)[0].Name, "FromEntry");
    EXPECT_EQ((*groups)[1].Name, "FromInclude");
}

/// 给出 entrySourcePath 则只采纳该文件的声明。
TEST(ShaderKeywordPragmaTest, OnlyAcceptsDeclarationsFromTheEntryFile) {
    const string text =
        MakeLineDirective("C:/repo/shaderlib/entry.hlsl") +
        "#pragma radray_keyword_group(FromEntry, _FROM_ENTRY)\n" +
        MakeLineDirective("C:/repo/shaderlib/other.hlsl") +
        "#pragma radray_keyword_group(FromInclude, _FROM_INCLUDE)\n" +
        MakeLineDirective("C:/repo/shaderlib/entry.hlsl") +
        "#pragma radray_keyword_group(BackInEntry, _BACK)\n";

    ShaderAssetDiagnostic diag;
    const std::optional<vector<ShaderKeywordGroupDesc>> groups =
        ParseShaderKeywordPragmas(text, "C:/repo/shaderlib/entry.hlsl", diag);
    ASSERT_TRUE(groups.has_value()) << diag.ToString();
    ASSERT_EQ(groups->size(), 2u);
    EXPECT_EQ((*groups)[0].Name, "FromEntry");
    EXPECT_EQ((*groups)[1].Name, "BackInEntry");
}

/// 路径比较要容忍分隔符与大小写差异 —— DXC 的 #line 用反斜杠, 我们传的是正斜杠。
TEST(ShaderKeywordPragmaTest, EntryFileMatchIgnoresSeparatorAndCase) {
    const string text =
        "#line 1 \"C:\\\\Repo\\\\ShaderLib\\\\Entry.hlsl\"\n"
        "#pragma radray_keyword_group(A, _A)\n";

    ShaderAssetDiagnostic diag;
    const std::optional<vector<ShaderKeywordGroupDesc>> groups =
        ParseShaderKeywordPragmas(text, "c:/repo/shaderlib/entry.hlsl", diag);
    ASSERT_TRUE(groups.has_value()) << diag.ToString();
    EXPECT_EQ(groups->size(), 1u);
}

TEST(ShaderKeywordPragmaTest, IgnoresUnrelatedPragmasAndPlainLines) {
    ShaderAssetDiagnostic diag;
    const std::optional<vector<ShaderKeywordGroupDesc>> groups = ParseShaderKeywordPragmas(
        "#pragma pack_matrix(row_major)\n"
        "#pragma once\n"
        "float4 PSMain() : SV_Target0 { return 0.0.xxxx; }\n",
        {},
        diag);
    ASSERT_TRUE(groups.has_value()) << diag.ToString();
    EXPECT_TRUE(groups->empty());
}

TEST(ShaderKeywordPragmaTest, AcceptsLeadingWhitespaceAndSpaceAfterHash) {
    ShaderAssetDiagnostic diag;
    const std::optional<vector<ShaderKeywordGroupDesc>> groups = ParseShaderKeywordPragmas(
        "    # pragma radray_keyword_group( Spaced , _SPACED )  stages( Pixel )\n",
        {},
        diag);
    ASSERT_TRUE(groups.has_value()) << diag.ToString();
    ASSERT_EQ(groups->size(), 1u);
    EXPECT_EQ((*groups)[0].Name, "Spaced");
    EXPECT_EQ((*groups)[0].Keywords[0], "_SPACED");
}

// ---- 负例: 语法错误必须报出来而不是静默产生半个组 ----

TEST(ShaderKeywordPragmaTest, RejectsMissingClosingParen) {
    ShaderAssetDiagnostic diag;
    EXPECT_FALSE(
        ParseShaderKeywordPragmas("#pragma radray_keyword_group(A, _A\n", {}, diag).has_value());
    EXPECT_FALSE(diag.Message.empty());
}

TEST(ShaderKeywordPragmaTest, RejectsGroupWithoutKeyword) {
    ShaderAssetDiagnostic diag;
    EXPECT_FALSE(
        ParseShaderKeywordPragmas("#pragma radray_keyword_group(A)\n", {}, diag).has_value());
    EXPECT_NE(diag.Message.find("at least one keyword"), string::npos) << diag.ToString();
}

TEST(ShaderKeywordPragmaTest, RejectsTrailingComma) {
    ShaderAssetDiagnostic diag;
    EXPECT_FALSE(
        ParseShaderKeywordPragmas("#pragma radray_keyword_group(A, _A,)\n", {}, diag).has_value());
    EXPECT_FALSE(diag.Message.empty());
}

TEST(ShaderKeywordPragmaTest, RejectsMissingGroupName) {
    ShaderAssetDiagnostic diag;
    EXPECT_FALSE(
        ParseShaderKeywordPragmas("#pragma radray_keyword_group(, _A)\n", {}, diag).has_value());
    EXPECT_FALSE(diag.Message.empty());
}

TEST(ShaderKeywordPragmaTest, RejectsUnknownStage) {
    ShaderAssetDiagnostic diag;
    EXPECT_FALSE(ParseShaderKeywordPragmas(
                     "#pragma radray_keyword_group(A, _A) stages(Geometry)\n", {}, diag)
                     .has_value());
    EXPECT_NE(diag.Message.find("unknown stage"), string::npos) << diag.ToString();
}

TEST(ShaderKeywordPragmaTest, RejectsEmptyStages) {
    ShaderAssetDiagnostic diag;
    EXPECT_FALSE(
        ParseShaderKeywordPragmas("#pragma radray_keyword_group(A, _A) stages()\n", {}, diag)
            .has_value());
    EXPECT_FALSE(diag.Message.empty());
}

TEST(ShaderKeywordPragmaTest, RejectsUnknownModifier) {
    ShaderAssetDiagnostic diag;
    EXPECT_FALSE(
        ParseShaderKeywordPragmas("#pragma radray_keyword_group(A, _A) optional\n", {}, diag)
            .has_value());
    EXPECT_NE(diag.Message.find("unknown modifier"), string::npos) << diag.ToString();
}

TEST(ShaderKeywordPragmaTest, RejectsDuplicateModifier) {
    ShaderAssetDiagnostic diag;
    EXPECT_FALSE(ParseShaderKeywordPragmas(
                     "#pragma radray_keyword_group(A, _A) required required\n", {}, diag)
                     .has_value());
    EXPECT_NE(diag.Message.find("duplicate"), string::npos) << diag.ToString();
}

TEST(ShaderKeywordPragmaTest, RejectsTrailingGarbage) {
    ShaderAssetDiagnostic diag;
    EXPECT_FALSE(
        ParseShaderKeywordPragmas("#pragma radray_keyword_group(A, _A) !!!\n", {}, diag)
            .has_value());
    EXPECT_FALSE(diag.Message.empty());
}

// ==================== pragma 剥离 ====================

TEST(ShaderKeywordPragmaTest, StripReplacesPragmaLinesWithBlanksKeepingLineCount) {
    const std::string_view text =
        "float A() { return 1.0; }\n"
        "#pragma radray_keyword_group(A, _A) stages(Pixel)\n"
        "#pragma pack_matrix(row_major)\n"
        "float B() { return 2.0; }\n";
    const string stripped = StripShaderKeywordPragmas(text);

    EXPECT_EQ(stripped.find("radray_keyword_group"), string::npos);
    // 无关 pragma 必须留下。
    EXPECT_NE(stripped.find("pack_matrix"), string::npos);
    EXPECT_NE(stripped.find("float A()"), string::npos);
    EXPECT_NE(stripped.find("float B()"), string::npos);
    // 行数不变, 这样 DXC 报的错误位置仍与预处理输出对得上。
    const auto countLines = [](std::string_view value) {
        return std::ranges::count(value, '\n');
    };
    EXPECT_EQ(countLines(stripped), countLines(text));
}

TEST(ShaderKeywordPragmaTest, StripKeepsTextWithoutPragmasIntact) {
    const std::string_view text = "float4 PSMain() : SV_Target0 { return 0.0.xxxx; }\n";
    EXPECT_EQ(StripShaderKeywordPragmas(text), text);
}

#if defined(RADRAY_ENABLE_SHADER_JIT)

namespace {

shared_ptr<render::Dxc> MakeDxc() {
    Nullable<shared_ptr<render::Dxc>> dxc = render::CreateDxc();
    if (!dxc.HasValue()) {
        return nullptr;
    }
    return dxc.Release();
}

/// imgui shader 的种子。这是覆盖面最广的最小正例: 它同时有 push constant、
/// 一个可做 static sampler 的采样器, 以及一个位宽必须手改的顶点属性 (COLOR 实际是
/// UNORM8X4, 反射只能给出 FLOAT32X4)。
ShaderTemplateSeed MakeImGuiSeed() {
    ShaderTemplatePassSeed pass{};
    pass.Name = "Default";
    pass.Stages = {
        ShaderStageDesc{render::ShaderStage::Vertex, "VSMain"},
        ShaderStageDesc{render::ShaderStage::Pixel, "PSMain"}};
    pass.ShaderModel = render::HlslShaderModel::SM60;

    ShaderTemplateSeed seed{};
    seed.Name = "RadRayImGui";
    seed.Source = "radray_imgui.hlsl";
    seed.Passes.push_back(std::move(pass));
    return seed;
}

ShaderTemplateOptions MakeOptions() {
    return ShaderTemplateOptions{
        .ShaderRoot = GetShaderRoot(),
        .UseSpirvReflection = true,
        .GenerateVertexInput = true};
}

}  // namespace

TEST(ShaderAssetTemplateTest, GeneratesBindingsFromReflection) {
    shared_ptr<render::Dxc> dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }

    ShaderAssetDiagnostic diagnostic;
    std::optional<ShaderAssetTemplate> generated =
        GenerateShaderAssetTemplate(*dxc, MakeImGuiSeed(), MakeOptions(), diagnostic);
    ASSERT_TRUE(generated.has_value()) << diagnostic.ToString();

    const ShaderAssetDesc& asset = generated->Asset;
    EXPECT_EQ(asset.Name, "RadRayImGui");
    EXPECT_EQ(asset.Source, "radray_imgui.hlsl");
    ASSERT_EQ(asset.Passes.size(), 1u);

    const ShaderPassDesc& pass = asset.Passes.front();
    EXPECT_EQ(pass.Name, "Default");
    EXPECT_EQ(pass.GetStageMask(), render::ShaderStages{render::ShaderStage::Graphics});
    EXPECT_EQ(pass.FindEntryPoint(render::ShaderStage::Vertex), "VSMain");
    EXPECT_EQ(pass.FindEntryPoint(render::ShaderStage::Pixel), "PSMain");

    // gTexture : register(t0, space1); gSampler : register(s1, space1)
    Nullable<const ShaderBindingDesc*> texture = FindBinding(pass, 1, 0);
    ASSERT_TRUE(texture.HasValue());
    EXPECT_EQ(texture.Unwrap()->Name, "gTexture");
    EXPECT_EQ(texture.Unwrap()->Type, render::ShaderParameterBindingType::Texture);
    EXPECT_EQ(texture.Unwrap()->Count, 1u);
    EXPECT_EQ(texture.Unwrap()->Stages, render::ShaderStages{render::ShaderStage::Pixel});

    Nullable<const ShaderBindingDesc*> sampler = FindBinding(pass, 1, 1);
    ASSERT_TRUE(sampler.HasValue());
    EXPECT_EQ(sampler.Unwrap()->Name, "gSampler");
    EXPECT_EQ(sampler.Unwrap()->Type, render::ShaderParameterBindingType::Sampler);
    // 反射无从得知是否该做 static sampler, 故一律不生成。
    EXPECT_FALSE(sampler.Unwrap()->ImmutableSampler.has_value());
}

TEST(ShaderAssetTemplateTest, RecognizesPushConstantThroughSpirv) {
    shared_ptr<render::Dxc> dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }
#if !defined(RADRAY_ENABLE_SPIRV_CROSS)
    GTEST_SKIP() << "push constant identity requires spirv-cross";
#endif

    ShaderAssetDiagnostic diagnostic;
    std::optional<ShaderAssetTemplate> generated =
        GenerateShaderAssetTemplate(*dxc, MakeImGuiSeed(), MakeOptions(), diagnostic);
    ASSERT_TRUE(generated.has_value()) << diagnostic.ToString();

    const ShaderPassDesc& pass = generated->Asset.Passes.front();
    ASSERT_TRUE(pass.PushConstant.has_value())
        << "SPIR-V reflection should have identified [[vk::push_constant]]";
    EXPECT_EQ(pass.PushConstant->Name, "gPush");
    // 位置来自 DXIL (register(b0, space0)); 大小来自 SPIR-V (float2 + float2)。
    EXPECT_EQ(pass.PushConstant->Location.Group, 0u);
    EXPECT_EQ(pass.PushConstant->Location.Binding, 0u);
    EXPECT_EQ(pass.PushConstant->Size, 16u);
    EXPECT_TRUE(pass.PushConstant->Stages.HasFlag(render::ShaderStage::Vertex));

    // 认领为 push constant 后, 它不应再作为普通 cbuffer 留在 BindingGroups 里。
    EXPECT_FALSE(FindBinding(pass, 0, 0).HasValue());
}

TEST(ShaderAssetTemplateTest, WithoutSpirvPushConstantStaysAnOrdinaryCBuffer) {
    shared_ptr<render::Dxc> dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }

    ShaderTemplateOptions options = MakeOptions();
    options.UseSpirvReflection = false;

    ShaderAssetDiagnostic diagnostic;
    std::optional<ShaderAssetTemplate> generated =
        GenerateShaderAssetTemplate(*dxc, MakeImGuiSeed(), options, diagnostic);
    ASSERT_TRUE(generated.has_value()) << diagnostic.ToString();

    // 这是 DXIL 反射的固有局限, 不是缺陷: 它把 push constant 当普通 cbuffer。
    // 生成器必须把这件事显式告知作者, 而不是悄悄产出一个错的 layout。
    const ShaderPassDesc& pass = generated->Asset.Passes.front();
    EXPECT_FALSE(pass.PushConstant.has_value());
    Nullable<const ShaderBindingDesc*> asCBuffer = FindBinding(pass, 0, 0);
    ASSERT_TRUE(asCBuffer.HasValue());
    EXPECT_EQ(asCBuffer.Unwrap()->Name, "gPush");
    EXPECT_EQ(asCBuffer.Unwrap()->Type, render::ShaderParameterBindingType::CBuffer);
    EXPECT_TRUE(HasTodoContaining(generated.value(), "PushConstant"));
}

TEST(ShaderAssetTemplateTest, GeneratesVertexInputAndFlagsFormatGuesses) {
    shared_ptr<render::Dxc> dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }

    ShaderAssetDiagnostic diagnostic;
    std::optional<ShaderAssetTemplate> generated =
        GenerateShaderAssetTemplate(*dxc, MakeImGuiSeed(), MakeOptions(), diagnostic);
    ASSERT_TRUE(generated.has_value()) << diagnostic.ToString();

    const ShaderPassDesc& pass = generated->Asset.Passes.front();
    ASSERT_TRUE(pass.VertexInput.has_value());
    ASSERT_EQ(pass.VertexInput->Attributes.size(), 3u);
    ASSERT_EQ(pass.VertexInput->Buffers.size(), 1u);

    const vector<ShaderVertexAttributeDesc>& attributes = pass.VertexInput->Attributes;
    EXPECT_EQ(attributes[0].Semantic, "POSITION");
    EXPECT_EQ(attributes[0].Format, render::VertexFormat::FLOAT32X2);
    EXPECT_EQ(attributes[0].Offset, 0u);
    EXPECT_EQ(attributes[1].Semantic, "TEXCOORD");
    EXPECT_EQ(attributes[1].Format, render::VertexFormat::FLOAT32X2);
    EXPECT_EQ(attributes[1].Offset, 8u);
    // COLOR 在真实布局里是 UNORM8X4。反射只能看到 shader 侧的 float4, 故生成
    // FLOAT32X4 并点名要求作者收窄 —— 这正是模板不能直接发布的原因之一。
    EXPECT_EQ(attributes[2].Semantic, "COLOR");
    EXPECT_EQ(attributes[2].Format, render::VertexFormat::FLOAT32X4);
    EXPECT_EQ(pass.VertexInput->Buffers.front().ArrayStride, 8u + 8u + 16u);

    EXPECT_TRUE(HasTodoContaining(generated.value(), "VertexInput.Attributes[*].Format"));

    // SV_Position 是系统值, 不由 vertex buffer 提供, 不该出现在属性表里。
    for (const ShaderVertexAttributeDesc& attribute : attributes) {
        EXPECT_EQ(attribute.Semantic.find("SV_"), string::npos);
    }
}

TEST(ShaderAssetTemplateTest, SuppressesVertexInputOnRequest) {
    shared_ptr<render::Dxc> dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }

    ShaderTemplateOptions options = MakeOptions();
    options.GenerateVertexInput = false;

    ShaderAssetDiagnostic diagnostic;
    std::optional<ShaderAssetTemplate> generated =
        GenerateShaderAssetTemplate(*dxc, MakeImGuiSeed(), options, diagnostic);
    ASSERT_TRUE(generated.has_value()) << diagnostic.ToString();
    EXPECT_FALSE(generated->Asset.Passes.front().VertexInput.has_value());
}

TEST(ShaderAssetTemplateTest, AlwaysReportsResidencyAsUnresolved) {
    shared_ptr<render::Dxc> dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }

    ShaderAssetDiagnostic diagnostic;
    std::optional<ShaderAssetTemplate> generated =
        GenerateShaderAssetTemplate(*dxc, MakeImGuiSeed(), MakeOptions(), diagnostic);
    ASSERT_TRUE(generated.has_value()) << diagnostic.ToString();

    // 驻留方式是性能决策, 生成器一律给最保守的 DescriptorTable 并点名。
    for (const ShaderBindingGroupDesc& group : generated->Asset.Passes.front().BindingGroups) {
        for (const ShaderBindingDesc& binding : group.Bindings) {
            EXPECT_EQ(binding.Residency, ShaderBindingResidency::DescriptorTable) << binding.Name;
        }
    }
    EXPECT_TRUE(HasTodoContaining(generated.value(), "Residency"));
    EXPECT_TRUE(HasTodoContaining(generated.value(), "KeywordGroups"));
}

namespace {

ShaderTemplateSeed MakeForwardSeed() {
    ShaderTemplatePassSeed passSeed{};
    passSeed.Name = "Forward";
    passSeed.Stages = {
        ShaderStageDesc{render::ShaderStage::Vertex, "VSMain"},
        ShaderStageDesc{render::ShaderStage::Pixel, "PSMain"}};
    passSeed.ShaderModel = render::HlslShaderModel::SM62;

    ShaderTemplateSeed seed{};
    seed.Name = "ForwardPrincipled";
    seed.Source = "forward_pipeline/forward_pass.hlsl";
    seed.Passes.push_back(std::move(passSeed));
    return seed;
}

size_t CountBindings(const ShaderAssetTemplate& value) {
    size_t total = 0;
    for (const ShaderBindingGroupDesc& group : value.Asset.Passes.front().BindingGroups) {
        total += group.Bindings.size();
    }
    return total;
}

}  // namespace

/// forward_pass 的贴图与阴影绑定全都包在 #ifdef 里。声明的 keyword 组足以让生成器
/// 自己推导出探测轮次, 因此【不给任何 probe】也应拿到完整的绑定集合 —— 这条正是
/// "不加 --probe 会静默丢掉一半 ABI" 的回归防线。
TEST(ShaderAssetTemplateTest, AutoProbesDeclaredKeywordsToRecoverHiddenBindings) {
    shared_ptr<render::Dxc> dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }

    ShaderAssetDiagnostic diagnostic;
    const std::optional<ShaderAssetTemplate> generated =
        GenerateShaderAssetTemplate(*dxc, MakeForwardSeed(), MakeOptions(), diagnostic);
    ASSERT_TRUE(generated.has_value()) << diagnostic.ToString();

    const ShaderPassDesc& pass = generated->Asset.Passes.front();
    // 阴影三件套 (space1: t1 / t2 / s3) 只在 shadow keyword 开启时进入字节码。
    EXPECT_TRUE(FindBinding(pass, 1, 1).HasValue()) << "gShadowCube must be recovered";
    EXPECT_TRUE(FindBinding(pass, 1, 2).HasValue()) << "gShadowArray must be recovered";
    EXPECT_TRUE(FindBinding(pass, 1, 3).HasValue()) << "gShadowSampler must be recovered";
    // 五张材质贴图 (space2: t1..t5) 同理。
    for (uint32_t slot = 1; slot <= 5; ++slot) {
        EXPECT_TRUE(FindBinding(pass, 2, slot).HasValue())
            << "material texture t" << slot << " must be recovered";
    }
    EXPECT_EQ(CountBindings(generated.value()), 12u);
}

/// 关掉自动探测则退化为"只看默认变体", 缺失的绑定应当明显更少。
/// 这条与上一条成对: 它证明上一条的覆盖确实来自自动探测, 而非碰巧。
TEST(ShaderAssetTemplateTest, DisablingAutoProbeNarrowsTheBindingSet) {
    shared_ptr<render::Dxc> dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }

    ShaderTemplateOptions options = MakeOptions();
    options.ProbeDeclaredKeywords = false;

    ShaderAssetDiagnostic diagnostic;
    const std::optional<ShaderAssetTemplate> narrowed =
        GenerateShaderAssetTemplate(*dxc, MakeForwardSeed(), options, diagnostic);
    ASSERT_TRUE(narrowed.has_value()) << diagnostic.ToString();

    const ShaderPassDesc& pass = narrowed->Asset.Passes.front();
    EXPECT_FALSE(FindBinding(pass, 1, 1).HasValue())
        << "without probing, the #ifdef-guarded shadow cube must stay invisible";
    EXPECT_LT(CountBindings(narrowed.value()), 12u);
    // KeywordGroups 仍然来自 pragma —— 它与探测是两件独立的事。
    EXPECT_EQ(narrowed->Asset.KeywordGroups.size(), 9u);
}

/// 显式 ProbeDefineSets 取代自动推导, 用于"两个宏同时开启才出现"的绑定。
TEST(ShaderAssetTemplateTest, ExplicitProbeDefinesReplaceAutoProbing) {
    shared_ptr<render::Dxc> dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }

    ShaderTemplateSeed seed = MakeForwardSeed();
    seed.Passes.front().ProbeDefineSets = {
        {"_BASECOLOR_MAP", "_METALROUGHNESS_MAP", "_NORMAL_MAP", "_OCCLUSION_MAP",
         "_EMISSIVE_MAP", "_ALPHATEST_ON", "_POINT_SHADOWS", "_DIRECTIONAL_SHADOWS"}};

    ShaderAssetDiagnostic diagnostic;
    const std::optional<ShaderAssetTemplate> probed =
        GenerateShaderAssetTemplate(*dxc, seed, MakeOptions(), diagnostic);
    ASSERT_TRUE(probed.has_value()) << diagnostic.ToString();
    EXPECT_EQ(CountBindings(probed.value()), 12u);
}

/// 阴影两组声明在 forward_interface.hlsl 里, 应经 include 被自动继承。
/// 这是"声明与它守护的 #ifdef 同文件"这一设计的直接验证。
TEST(ShaderAssetTemplateTest, InheritsKeywordGroupsFromIncludedHeaders) {
    shared_ptr<render::Dxc> dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }

    ShaderAssetDiagnostic diagnostic;
    const std::optional<ShaderAssetTemplate> inherited =
        GenerateShaderAssetTemplate(*dxc, MakeForwardSeed(), MakeOptions(), diagnostic);
    ASSERT_TRUE(inherited.has_value()) << diagnostic.ToString();

    const auto hasGroup = [](const ShaderAssetTemplate& value, std::string_view name) {
        return std::ranges::any_of(
            value.Asset.KeywordGroups,
            [&](const ShaderKeywordGroupDesc& group) noexcept { return group.Name == name; });
    };
    EXPECT_TRUE(hasGroup(inherited.value(), "PointShadows"));
    EXPECT_TRUE(hasGroup(inherited.value(), "DirectionalShadows"));
    EXPECT_EQ(inherited->Asset.KeywordGroups.size(), 9u);

    // 切到 EntryFileOnly 则只剩 forward_pass.hlsl 自己声明的 7 组; 随之丢掉阴影
    // 绑定 —— 探测不到的维度就探测不出对应的槽位。
    ShaderTemplateOptions entryOnly = MakeOptions();
    entryOnly.KeywordPragmaScope = ShaderKeywordPragmaScope::EntryFileOnly;
    const std::optional<ShaderAssetTemplate> narrowed =
        GenerateShaderAssetTemplate(*dxc, MakeForwardSeed(), entryOnly, diagnostic);
    ASSERT_TRUE(narrowed.has_value()) << diagnostic.ToString();
    EXPECT_EQ(narrowed->Asset.KeywordGroups.size(), 7u);
    EXPECT_FALSE(hasGroup(narrowed.value(), "PointShadows"));
    EXPECT_FALSE(FindBinding(narrowed->Asset.Passes.front(), 1, 1).HasValue())
        << "without the shadow keywords there is nothing to probe with";
}

/// KeywordGroups 必须与源码里的 pragma 逐字段一致。
TEST(ShaderAssetTemplateTest, KeywordGroupsComeFromSourcePragmas) {
    shared_ptr<render::Dxc> dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }

    ShaderAssetDiagnostic diagnostic;
    const std::optional<ShaderAssetTemplate> generated =
        GenerateShaderAssetTemplate(*dxc, MakeForwardSeed(), MakeOptions(), diagnostic);
    ASSERT_TRUE(generated.has_value()) << diagnostic.ToString();

    const vector<ShaderKeywordGroupDesc>& groups = generated->Asset.KeywordGroups;
    ASSERT_EQ(groups.size(), 9u);
    // 声明顺序即 pragma 出现顺序。
    EXPECT_EQ(groups[0].Name, "BaseColorMap");
    ASSERT_EQ(groups[0].Keywords.size(), 1u);
    EXPECT_EQ(groups[0].Keywords[0], "_BASECOLOR_MAP");
    EXPECT_TRUE(groups[0].IsOptional);
    EXPECT_EQ(groups[0].Stages, render::ShaderStages{render::ShaderStage::Pixel});

    // AlphaMode 是唯一的多取值组: 同组即互斥。
    const auto alphaMode = std::ranges::find_if(groups, [](const ShaderKeywordGroupDesc& group) {
        return group.Name == "AlphaMode";
    });
    ASSERT_NE(alphaMode, groups.end());
    ASSERT_EQ(alphaMode->Keywords.size(), 2u);
    EXPECT_EQ(alphaMode->Keywords[0], "_ALPHATEST_ON");
    EXPECT_EQ(alphaMode->Keywords[1], "_ALPHABLEND_ON");

    // 已能自动生成, 故不再作为待办点名。
    EXPECT_FALSE(HasTodoContaining(generated.value(), "KeywordGroups"));
    // 但 BakeVariants 仍然是作者的发布决策。
    EXPECT_TRUE(HasTodoContaining(generated.value(), "BakeVariants"));
}

/// 关掉 pragma 解析后不应产生任何组, 且重新变成一条待办。
TEST(ShaderAssetTemplateTest, DisablingPragmaParsingLeavesKeywordGroupsEmpty) {
    shared_ptr<render::Dxc> dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }

    ShaderTemplateOptions options = MakeOptions();
    options.ParseKeywordPragmas = false;

    ShaderAssetDiagnostic diagnostic;
    const std::optional<ShaderAssetTemplate> generated =
        GenerateShaderAssetTemplate(*dxc, MakeForwardSeed(), options, diagnostic);
    ASSERT_TRUE(generated.has_value()) << diagnostic.ToString();
    EXPECT_TRUE(generated->Asset.KeywordGroups.empty());
    EXPECT_TRUE(HasTodoContaining(generated.value(), "KeywordGroups"));
}

/// 生成的 manifest 必须能连带 KeywordGroups 一起通过解析与 cook。
/// 这是最强的端到端性质: pragma -> 生成 -> 解析 -> cook 全链路自洽。
TEST(ShaderAssetTemplateTest, GeneratedForwardTemplateParsesAndCooks) {
    shared_ptr<render::Dxc> dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }

    ShaderAssetDiagnostic diagnostic;
    const std::optional<ShaderAssetTemplate> generated =
        GenerateShaderAssetTemplate(*dxc, MakeForwardSeed(), MakeOptions(), diagnostic);
    ASSERT_TRUE(generated.has_value()) << diagnostic.ToString();

    const std::optional<string> json = SerializeShaderAssetTemplate(generated.value());
    ASSERT_TRUE(json.has_value());

    const std::optional<ShaderAssetDesc> reparsed = ParseShaderAssetDesc(json.value(), diagnostic);
    ASSERT_TRUE(reparsed.has_value()) << diagnostic.ToString();
    EXPECT_EQ(reparsed->KeywordGroups.size(), 9u);
    EXPECT_EQ(reparsed.value(), generated->Asset);

    ScopedDirectory output;
    ASSERT_TRUE(output.IsValid());
    const std::filesystem::path manifestPath = output.Path() / "forward_pass.shader.json";
    ASSERT_TRUE(WriteTextFile(manifestPath, json.value()));

    vector<render::ShaderBlobCategory> categories{render::ShaderBlobCategory::DXIL};
#if defined(RADRAY_ENABLE_SPIRV_CROSS)
    categories.push_back(render::ShaderBlobCategory::SPIRV);
#endif
    const ShaderCookOptions cookOptions{
        .ShaderRoot = GetShaderRoot(),
        .ManifestPath = manifestPath,
        .Categories = categories,
        .ValidateReflection = true,
        .Incremental = false};
    const ShaderCookResult cook = CookShaderAssetFile(*dxc, cookOptions);
    EXPECT_TRUE(cook.Succeeded()) << JoinDiagnostics(cook.Diagnostics);
}

TEST(ShaderAssetTemplateTest, GeneratedTemplateParsesAndCooks) {
    shared_ptr<render::Dxc> dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }

    ShaderAssetDiagnostic diagnostic;
    std::optional<ShaderAssetTemplate> generated =
        GenerateShaderAssetTemplate(*dxc, MakeImGuiSeed(), MakeOptions(), diagnostic);
    ASSERT_TRUE(generated.has_value()) << diagnostic.ToString();
    EXPECT_FALSE(generated->Todos.empty());

    std::optional<string> json = SerializeShaderAssetTemplate(generated.value());
    ASSERT_TRUE(json.has_value());
    // "_TODO" 不属于 manifest schema, 但必须与声明同文件交付。
    EXPECT_NE(json->find("\"_TODO\""), string::npos);

    // 关键性质: 带 "_TODO" 的文件仍然是合法 manifest, 可直接解析。
    std::optional<ShaderAssetDesc> reparsed = ParseShaderAssetDesc(json.value(), diagnostic);
    ASSERT_TRUE(reparsed.has_value()) << diagnostic.ToString();
    EXPECT_EQ(reparsed.value(), generated->Asset);

    // 更强的性质: 生成的 manifest 能通过 cook 期的反射核对 (声明 ⊇ 反射)。
    // 这是生成器与校验器共用同一套反射折叠规则的直接后果。
    ScopedDirectory output;
    ASSERT_TRUE(output.IsValid());
    const std::filesystem::path manifestPath = output.Path() / "radray_imgui.shader.json";
    ASSERT_TRUE(WriteTextFile(manifestPath, json.value()));

    vector<render::ShaderBlobCategory> categories{render::ShaderBlobCategory::DXIL};
#if defined(RADRAY_ENABLE_SPIRV_CROSS)
    categories.push_back(render::ShaderBlobCategory::SPIRV);
#endif
    const ShaderCookOptions cookOptions{
        .ShaderRoot = GetShaderRoot(),
        .ManifestPath = manifestPath,
        .Categories = categories,
        .ValidateReflection = true,
        .Incremental = false};
    ShaderCookResult cook = CookShaderAssetFile(*dxc, cookOptions);
    EXPECT_TRUE(cook.Succeeded()) << JoinDiagnostics(cook.Diagnostics);
}

TEST(ShaderAssetTemplateTest, RejectsMissingSource) {
    shared_ptr<render::Dxc> dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }

    ShaderTemplateSeed seed = MakeImGuiSeed();
    seed.Source = "does_not_exist.hlsl";
    seed.Passes.front().Source.clear();

    ShaderAssetDiagnostic diagnostic;
    EXPECT_FALSE(
        GenerateShaderAssetTemplate(*dxc, seed, MakeOptions(), diagnostic).has_value());
    EXPECT_NE(diagnostic.Message.find("does not exist"), string::npos);
}

TEST(ShaderAssetTemplateTest, RejectsSeedWithoutPasses) {
    shared_ptr<render::Dxc> dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }

    ShaderAssetDiagnostic diagnostic;
    EXPECT_FALSE(
        GenerateShaderAssetTemplate(*dxc, ShaderTemplateSeed{}, MakeOptions(), diagnostic)
            .has_value());
    EXPECT_FALSE(diagnostic.Message.empty());
}

TEST(ShaderAssetTemplateTest, RejectsSeedPassWithoutStages) {
    shared_ptr<render::Dxc> dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }

    ShaderTemplateSeed seed = MakeImGuiSeed();
    seed.Passes.front().Stages.clear();

    ShaderAssetDiagnostic diagnostic;
    EXPECT_FALSE(
        GenerateShaderAssetTemplate(*dxc, seed, MakeOptions(), diagnostic).has_value());
    EXPECT_NE(diagnostic.Message.find("stage"), string::npos);
}

TEST(ShaderAssetTemplateTest, DefaultsAssetNameToTheSourceStem) {
    shared_ptr<render::Dxc> dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }

    ShaderTemplateSeed seed = MakeImGuiSeed();
    seed.Name.clear();
    seed.Passes.front().Name.clear();

    ShaderAssetDiagnostic diagnostic;
    std::optional<ShaderAssetTemplate> generated =
        GenerateShaderAssetTemplate(*dxc, seed, MakeOptions(), diagnostic);
    ASSERT_TRUE(generated.has_value()) << diagnostic.ToString();
    EXPECT_EQ(generated->Asset.Name, "radray_imgui");
    EXPECT_EQ(generated->Asset.Passes.front().Name, "main");
}

#endif

TEST(ShaderAssetTemplateTest, SerializesWithoutTodosWhenThereAreNone) {
    // 序列化不需要 DXC: 它只是把已有的 ShaderAssetTemplate 写出去。
    ShaderAssetTemplate value{};
    value.Asset.Name = "Empty";
    value.Asset.Source = "empty.hlsl";
    ShaderPassDesc pass{};
    pass.Name = "Main";
    pass.Stages = {ShaderStageDesc{render::ShaderStage::Compute, "CSMain"}};
    value.Asset.Passes.push_back(std::move(pass));

    std::optional<string> json = SerializeShaderAssetTemplate(value);
    ASSERT_TRUE(json.has_value());
    // 无待确认项时不写空数组 —— 空的 "_TODO" 只会制造噪音。
    EXPECT_EQ(json->find("_TODO"), string::npos);

    ShaderAssetDiagnostic diagnostic;
    ASSERT_TRUE(ParseShaderAssetDesc(json.value(), diagnostic).has_value())
        << diagnostic.ToString();
}

}  // namespace radray
