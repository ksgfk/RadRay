#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

#include <radray/nullable.h>
#include <radray/json.h>
#include <radray/render/hlsl.h>
#include <radray/render/rhi.h>
#include <radray/render/spirv.h>
#include <radray/types.h>

// shader 资产元数据 (*.shader.json) 与其 AOT 产物。
//
// == manifest 的存在理由 ==
//
// HLSL 源码 + 后端反射【不足以】构建 PSO。反射能告诉我们"有哪些资源、在哪个
// register/space", 但拿不到以下信息, 它们必须由作者声明:
//
//   1. push constant 身份。DXIL 反射把 [[vk::push_constant]] 的 cbuffer 当普通
//      cbuffer, 完全不知道它特殊; SPIRV 反射知道 size 但没有 D3D 的 register/space。
//      两个后端都缺, 不是"只有 vk 侧标记"。
//   2. 绑定驻留方式。同一个 cbuffer 既可放进 descriptor table, 也可做 root
//      descriptor (D3D12 root CBV / Vulkan UNIFORM_BUFFER_DYNAMIC)。这是作者的
//      性能决策, 不是 shader 的属性。
//   3. immutable / static sampler。纯 pipeline-layout 期概念, 不在字节码里。
//   4. unbounded 数组的实际容量。反射只说"unbounded", 两个后端都拒绝 Count == 0。
//   5. 被 DCE 或 keyword #ifdef 消掉的绑定。反射看不到, 但 layout 必须保留槽位,
//      否则同一 shader 的不同变体无法共用一个 PipelineLayout。
//   6. 顶点属性的 VertexFormat / buffer slot / offset / stride。反射只给 semantic
//      与 component type + mask, 推不出归一化与位宽。
//   7. entry point 名与 keyword 组合域 —— 这些本来就是编译输入。
//
// 数据流方向: manifest 声明 ABI, 反射只做一致性核对。因此
// BuildPipelineLayoutStorage 不需要任何反射数据 / 字节码 / target 信息, 可以在
// 编译任何 shader 之前建好 PipelineLayout, 且结果对 target 与 variant 都不变。
//
// 与 render 层反射 JSON (hlsl.h / spirv.h) 的区别: 那份是机器生成的, 枚举存整数;
// 这份是人写人 diff 的, 枚举存字符串。两者性质不同, 刻意不共用约定。
//
// == AOT 产物 ==
//
// 目录约定: manifest `foo.shader.json` 的产物放在同目录的同名文件夹 `foo/`:
//
//   forward_pass.shader.json
//   forward_pass/
//       index.json                  变体表: key -> blob 相对路径 + cook 元信息
//       dxil/a3f29c1b40e8d715....bin
//       spirv/8c01ae52f7d3b064....bin
//
// blob 采用【内容寻址】: 文件名即 stage artifact key 的 hex。由此当多个 program
// variant 投影到同一份 stage 字节码时 (见 ShaderKeywordGroupDesc::Stages) 自然去重。
// 按 target 分子目录, 使"只发布 DXIL"退化为删除一个目录。
//
// 反射数据【不】落盘: manifest 已经是唯一 ABI 来源, PipelineLayout 不需要反射即可
// 构建。反射仅在 cook 期用于核对声明 ⊇ 反射。
//
// manifest 与产物放在同一处的理由: manifest 描述 ABI 契约, 产物描述该契约在某个
// target 上的编译结果, 两者由同一套 key 绑定。

namespace radray::render {
class Dxc;
}  // namespace radray::render

namespace radray {

// ============================ 常量 ============================

/// 当前 manifest 格式版本。不匹配直接拒绝解析。
inline constexpr uint32_t kShaderAssetFormatVersion = 1;

/// AOT 产物的初始格式版本。参与 source identity、artifact key 以及 blob/index 校验
inline constexpr uint32_t kShaderArtifactFormatVersion = 1;

/// 组内"全部关闭"。仅当该组 IsOptional 为 true 时是合法选择。
/// 同时是单组 keyword 数量的排他上界 (该值被保留, 不可作为下标)。
inline constexpr uint16_t kShaderKeywordOff = 0xFFFF;

// ============================ 枚举 ============================

/// 绑定的驻留方式。反射无法提供, 由 manifest 声明。
enum class ShaderBindingResidency : int32_t {
    /// 描述符表条目。D3D12: descriptor table range; Vulkan: 普通 descriptor。
    DescriptorTable,
    /// 绑定时直接提交 GPU 虚拟地址, 不占描述符堆, 支持 dynamic offset。
    /// D3D12: root CBV/SRV/UAV; Vulkan: *_DYNAMIC descriptor。
    /// 仅 CBuffer / Buffer / RWBuffer 合法, 且 Count 必须为 1。
    RootDescriptor,
};

/// 字节码的来源。用于诊断与测试断言。
enum class ShaderBytecodeSource : uint8_t {
    /// 来自同名目录下的 AOT 产物。
    Artifact,
    /// 来自 DXC 现场编译。
    Jit,
};

/// 源码身份与产物记录不匹配时的处置策略。
enum class ShaderArtifactStaleness : uint8_t {
    /// 开发用: 源码哈希与产物不符即视为未命中, 回退 JIT。改 shader 立刻生效
    /// (含 resolver 实例存活期间的编辑 —— 身份缓存按依赖时间戳失效)。
    Strict,
    /// 发布用: 只按逻辑 key 命中。源码可读且哈希不符时仅告警, 源码缺失时静默接受。
    /// 存在理由: 发布包内没有 DXC 可回退, 源码若被改动一个字节也不该让整包 shader 失效。
    Lenient,
};

// ============================ manifest 数据 ============================

/// 一个描述符绑定的完整声明。这是 ABI 契约, 不是对反射结果的覆盖。
struct ShaderBindingDesc {
    /// HLSL 声明名。用于与反射比对, 以及未来 material 侧按名寻址。
    string Name;
    /// 组内绑定号。等于 HLSL register 号。
    /// 注意 b/t/s/u 在本 ABI 中【共用同一编号空间】(Vulkan 的要求, 见校验规则)。
    uint32_t Binding{0};
    render::ShaderParameterBindingType Type{render::ShaderParameterBindingType::UNKNOWN};
    /// 数组容量。1 表示非数组。unbounded 数组必须在此给出实际容量。
    uint32_t Count{1};
    render::ShaderStages Stages{render::ShaderStage::UNKNOWN};
    ShaderBindingResidency Residency{ShaderBindingResidency::DescriptorTable};
    /// 静态采样器。仅 Sampler 类型且 Count == 1 合法。
    std::optional<render::SamplerDescriptor> ImmutableSampler{};

    friend bool operator==(const ShaderBindingDesc&, const ShaderBindingDesc&) noexcept = default;
};

/// 一个 descriptor set / register space。
/// Group 同时是 D3D12 的 RegisterSpace 与 Vulkan 的 set index —— 这是 RHI 后端已经
/// 硬化的不变量 (见 shaderlib/forward_pipeline/binding_abi.hlsl), manifest 不做重映射。
struct ShaderBindingGroupDesc {
    uint32_t Group{0};
    vector<ShaderBindingDesc> Bindings;

    friend bool operator==(const ShaderBindingGroupDesc&, const ShaderBindingGroupDesc&) noexcept = default;
};

/// push constant。不占 descriptor set 槽位, 故独立于 ShaderBindingGroupDesc,
/// 且"整个 layout 至多一个"由类型系统保证而非计数校验。
///
/// Location 取 HLSL 的 (space, register): D3D12 用它填 32BIT_CONSTANTS 的
/// RegisterSpace / ShaderRegister; Vulkan 只用它做 SetPushConstants 的匹配键
/// (VkPushConstantRange 本身不需要位置)。因此一个字段同时服务两个后端。
struct ShaderPushConstantDesc {
    string Name;
    render::ShaderBindingLocation Location{};
    /// 字节数, 必须非零且 4 字节对齐。
    uint32_t Size{0};
    render::ShaderStages Stages{render::ShaderStage::UNKNOWN};

    friend bool operator==(const ShaderPushConstantDesc&, const ShaderPushConstantDesc&) noexcept = default;
};

/// 顶点属性声明。
struct ShaderVertexAttributeDesc {
    /// HLSL semantic 名, 不含尾部数字 (如 "POSITION")。
    string Semantic;
    uint32_t SemanticIndex{0};
    render::VertexFormat Format{render::VertexFormat::UNKNOWN};
    /// 所属 vertex buffer 的 Binding。
    uint32_t BufferBinding{0};
    /// 在该 buffer 一个元素内的字节偏移。
    uint32_t Offset{0};
    /// SPIRV location。留空则按 Attributes 声明顺序自动编号。
    std::optional<uint32_t> Location{};

    friend bool operator==(const ShaderVertexAttributeDesc&, const ShaderVertexAttributeDesc&) noexcept = default;
};

struct ShaderVertexBufferDesc {
    uint32_t Binding{0};
    uint32_t ArrayStride{0};
    render::VertexStepMode StepMode{render::VertexStepMode::Vertex};

    friend bool operator==(const ShaderVertexBufferDesc&, const ShaderVertexBufferDesc&) noexcept = default;
};

struct ShaderVertexInputDesc {
    vector<ShaderVertexBufferDesc> Buffers;
    vector<ShaderVertexAttributeDesc> Attributes;

    friend bool operator==(const ShaderVertexInputDesc&, const ShaderVertexInputDesc&) noexcept = default;
};

/// 一个编译阶段。
struct ShaderStageDesc {
    render::ShaderStage Stage{render::ShaderStage::UNKNOWN};
    string EntryPoint;

    friend bool operator==(const ShaderStageDesc&, const ShaderStageDesc&) noexcept = default;
};

/// keyword 组。组内 keyword 互斥。
///
/// 只声明【合法组合域】(哪些组合可以被请求), 不描述离线预编译覆盖范围 ——
/// 后者由 ShaderBakeSetDesc 单独声明。域大而烘焙集小是正常状态: 未烘焙的组合
/// 在开发构建走 JIT, 在发布包 (AllowJit == false) 成为显式错误。
struct ShaderKeywordGroupDesc {
    string Name;
    /// 互斥取值, 均为真实宏名。不可含空串 —— "关闭"由 IsOptional 表达, 不是取值之一。
    /// 数量必须小于 kShaderKeywordOff (ShaderVariantKey 的编码约束)。
    vector<string> Keywords;
    /// 允许"该组全部关闭"。false 时必须选中其中一个 keyword。
    bool IsOptional{true};
    /// 该组影响哪些 stage。用于把 pixel-only keyword 从 VS 的编译 key 里投影掉,
    /// 使多个 program variant 复用同一份 VS 字节码。
    render::ShaderStages Stages{render::ShaderStage::Graphics};

    friend bool operator==(const ShaderKeywordGroupDesc&, const ShaderKeywordGroupDesc&) noexcept = default;
};

/// 一条烘焙声明。Expand 与 Combination 恰好一个非空。
///
/// 存在理由: KeywordGroups 声明的合法域通常远大于值得预编译的范围, 而"烘哪些"是
/// 作者的发布决策, 不能由域自动推导。两种写法各有不可替代的场景:
///   - Expand: 一组正交轴的全组合, 对应 Unity 的 multi_compile;
///   - Combination: 单个精确组合, 表达 Expand 表达不出的稀疏点。
struct ShaderBakeRuleDesc {
    /// 参与笛卡尔积的组名。未列出的组取默认值 (可选组全关, 必选组取首个 keyword)。
    vector<string> Expand;
    /// 一个显式组合的 keyword 列表。空数组不合法 (要烘默认变体请用空 Expand 之外
    /// 的方式: 默认变体总在 Expand 的积里, 或干脆不声明 BakeVariants)。
    vector<string> Combination;

    friend bool operator==(const ShaderBakeRuleDesc&, const ShaderBakeRuleDesc&) noexcept = default;
};

/// 一个 pass 的离线烘焙范围。
///
/// 【留空 = 只烘默认变体】。这使未声明 BakeVariants 的旧 manifest 语义不变, 也让
/// "什么都不烘除非作者说要烘"成为默认。未烘焙的组合在开发构建走 JIT, 在发布包
/// (ShaderResolveConfig::AllowJit == false) 成为显式错误。
struct ShaderBakeSetDesc {
    vector<ShaderBakeRuleDesc> Rules;
    /// 剔除规则: 任一条的【全部】keyword 同时出现的组合被丢弃。对应 Unity 的
    /// skip_variants。只作用于 Expand 的积 —— 显式 Combination 是作者点名要的,
    /// 不该被泛化规则悄悄拿掉。
    vector<vector<string>> Skip;

    bool IsEmpty() const noexcept { return Rules.empty(); }

    friend bool operator==(const ShaderBakeSetDesc&, const ShaderBakeSetDesc&) noexcept = default;
};

/// 一个 pass。同一资产的不同 pass 可以有完全不同的 ABI
/// (对比 shaderlib/forward_pipeline 下的 forward_pass 与 shadow_pass)。
struct ShaderPassDesc {
    string Name;
    /// 相对 shader include root 的源文件路径。留空则沿用 ShaderAssetDesc::Source。
    string Source;
    vector<ShaderStageDesc> Stages;
    render::HlslShaderModel ShaderModel{render::HlslShaderModel::SM60};
    /// 无条件生效的宏。与 keyword 的区别: 这些不产生变体。
    vector<string> Defines;

    /// 以下两项直接改变字节码 (-O3/-Od, -all_resources_bound), 因此必须进 manifest:
    /// 它们是 artifact 身份的一部分, 否则 AOT 与 JIT 会编出不同结果却共用同一个 key。
    bool IsOptimize{true};
    /// 允许 unbounded 描述符数组。false 时 DXC 会加 -all_resources_bound。
    bool EnableUnbounded{true};

    /// 本 pass 参与变体的 keyword 组名, 引用 ShaderAssetDesc::KeywordGroups。
    /// 留空 = 全部组。存在理由: 同一资产的 forward pass 与 shadow pass 用的
    /// keyword 集合本就不同, 不该让前者的组给后者生成无意义的变体维度。
    vector<string> KeywordGroups;
    /// 本 pass 的烘焙范围。留空则沿用 ShaderAssetDesc::BakeVariants。
    ShaderBakeSetDesc BakeVariants;

    vector<ShaderBindingGroupDesc> BindingGroups;
    std::optional<ShaderPushConstantDesc> PushConstant{};
    /// 仅 graphics pass 有意义。
    std::optional<ShaderVertexInputDesc> VertexInput{};

    /// 本 pass 声明的所有 stage 的并集。
    render::ShaderStages GetStageMask() const noexcept;
    /// 按 stage 查 entry point。未声明该 stage 返回空。
    std::optional<std::string_view> FindEntryPoint(render::ShaderStage stage) const noexcept;
    Nullable<const ShaderBindingGroupDesc*> FindGroup(uint32_t group) const noexcept;
    Nullable<const ShaderBindingDesc*> FindBinding(uint32_t group, uint32_t binding) const noexcept;

    friend bool operator==(const ShaderPassDesc&, const ShaderPassDesc&) noexcept = default;
};

/// 一份 shader 资产的完整元数据, 对应一个 *.shader.json 文件。
///
/// 刻意【不】包含: PrimitiveState / DepthStencilState / BlendState / ColorTargets /
/// MultiSampleState。这些属 PSO 固定功能段, 不影响字节码, 由材质在建 PSO 时覆盖
/// (见 runtime/render_framework/render_pipeline.h 的 MaterialRenderState)。
/// 也不包含 AssetId —— 落盘身份约定尚未确定。
struct ShaderAssetDesc {
    string Name;
    /// 资产级默认源文件, 相对 shader include root。
    string Source;
    vector<ShaderKeywordGroupDesc> KeywordGroups;
    /// 资产级默认烘焙范围, 供未声明 BakeVariants 的 pass 继承。
    ///
    /// 继承是【不对称】的, 这是刻意的: pass 显式写的规则引用了本 pass 没有的组会
    /// 报错 (显式声明必须被严格核对), 而继承下来的规则遇到本 pass 没有的组会静默
    /// 投影掉 (共享默认值必须能被裁剪, 否则每个 pass 都得复写一遍)。
    ShaderBakeSetDesc BakeVariants;
    vector<ShaderPassDesc> Passes;

    Nullable<const ShaderPassDesc*> FindPass(std::string_view name) const noexcept;

    friend bool operator==(const ShaderAssetDesc&, const ShaderAssetDesc&) noexcept = default;
};

/// 结构化诊断。所有失败路径经此返回, 不使用异常。
struct ShaderAssetDiagnostic {
    string Message;
    string PassName;
    string BindingName;
    std::optional<uint32_t> Group{};
    std::optional<uint32_t> Binding{};
    std::optional<render::ShaderStage> Stage{};

    /// 拼成单行可读文本 (含已填的上下文字段)。
    string ToString() const;
};

// ============================ 变体身份 ============================

/// 一个 program 变体。
///
/// 编码: 长度 == domain 的组数, 槽位序 == 组的声明顺序, 值 == 组内 keyword 下标
/// 或 kShaderKeywordOff。按【组】而非按 keyword 编码, 因为组内互斥已由 manifest
/// 校验保证, 于是"每组选了什么"就是完整身份, 且 keyword 总数不受任何上限约束。
///
/// 【仅在同一个 domain 内可比】: 槽位语义由 domain 的组声明顺序定义, 跨
/// (asset, pass) 比较无意义。比较运算符只用于域内去重与排序。
///
/// 【不要放进每帧路径】: 这是变长结构, 属作者期 / cook 期概念。运行时 PSO 缓存应
/// 改用 ComputeShaderArtifactKey 得到的 ShaderHash —— 那是 POD, 无分配。
struct ShaderVariantKey {
    vector<uint16_t> Selection;

    friend bool operator==(const ShaderVariantKey&, const ShaderVariantKey&) noexcept = default;
    friend auto operator<=>(const ShaderVariantKey&, const ShaderVariantKey&) noexcept = default;
};

/// 一个 (asset, pass) 的合法变体域。从 manifest 构建, 不需要字节码或反射。
///
/// 承担三件事: 校验请求的 keyword 集合、把变体投影到 stage、把变体展开成宏。
/// 【不】负责烘焙范围 —— 那是 ShaderBakeSetDesc 与 ExpandShaderBakeSet 的事。
class ShaderVariantDomain {
public:
    /// 域内的一个 keyword 组。是 ShaderKeywordGroupDesc 经 pass 筛选后的副本。
    struct Group {
        string Name;
        vector<string> Keywords;
        render::ShaderStages Stages{render::ShaderStage::UNKNOWN};
        bool IsOptional{true};
    };

    /// pass 必须属于 asset, 且 asset 已通过 ParseShaderAssetDesc 的自校验。
    /// 只取 pass.KeywordGroups 引用到的组 (留空则取全部)。
    static std::optional<ShaderVariantDomain> Build(
        const ShaderAssetDesc& asset,
        const ShaderPassDesc& pass,
        ShaderAssetDiagnostic& outDiag) noexcept;

    size_t GroupCount() const noexcept { return _groups.size(); }

    /// 按声明顺序取组, 供烘焙集展开使用。
    std::span<const Group> Groups() const noexcept { return _groups; }

    /// keyword 名集合 -> 变体。未知 keyword、同组多选、必选组未选 -> nullopt。
    std::optional<ShaderVariantKey> Resolve(
        std::span<const std::string_view> keywords,
        ShaderAssetDiagnostic& outDiag) const noexcept;

    /// 单个 keyword 的开关。开启会自动顶掉同组的其他选择 (组内互斥)。
    /// enabled == false 且该 keyword 未被选中时是 no-op, 不算失败。
    /// keyword 不属于本域, 或关闭会使必选组落空 -> nullopt。
    std::optional<ShaderVariantKey> WithKeyword(
        const ShaderVariantKey& key,
        std::string_view keyword,
        bool enabled) const noexcept;

    /// 把与该 stage 无关的组槽位归一化为 kShaderKeywordOff。
    ///
    /// 【无论 IsOptional 一律归 Off】: 被投影掉的组在该 stage 不产生任何宏, 故
    /// 规范形式必须是 Off; 若必选组保留其默认选择, 两个本应共用同一份 VS 的变体
    /// 会算出不同的 artifact key, 去重失效。
    ///
    /// 由此: 两个变体投影结果相同 <=> 该 stage 共用同一份字节码。
    ShaderVariantKey ProjectToStage(
        const ShaderVariantKey& key,
        render::ShaderStage stage) const noexcept;

    /// pass.Defines + 投影到该 stage 后选中的 keyword。
    /// 这是 ComputeShaderArtifactKey 与 DxcCompileOptions 的 Defines 输入。
    vector<string> CollectDefines(
        const ShaderVariantKey& key,
        render::ShaderStage stage) const noexcept;

    /// 投影后选中的 keyword 名, 已排序。供 index.json 记录与诊断输出。
    vector<string> DescribeKeywords(
        const ShaderVariantKey& key,
        render::ShaderStage stage) const noexcept;

    /// 默认变体: 可选组全关, 必选组取首个 keyword。
    ShaderVariantKey DefaultVariant() const noexcept;

    /// 长度与每个槽位取值是否都在域内。
    bool IsValid(const ShaderVariantKey& key) const noexcept;

    /// keyword 名 -> (组槽位, 组内下标)。不属于本域返回 nullopt。
    std::optional<std::pair<uint32_t, uint16_t>> FindKeyword(std::string_view keyword) const noexcept;

private:
    /// 按 keyword 名排序的查找表, 二分命中。asset 内 keyword 名全局唯一
    /// (ValidateAsset 已强制), 故一张平表足够。数量级在几十, 排序数组比哈希表
    /// 更省内存且局部性更好。
    struct LookupEntry {
        string Keyword;
        uint32_t GroupIndex{0};
        uint16_t KeywordIndex{0};
    };

    vector<Group> _groups;
    vector<string> _passDefines;
    vector<LookupEntry> _lookup;
};

/// 取该 pass 生效的烘焙声明: pass 自己的, 或继承资产级的。
const ShaderBakeSetDesc& GetEffectiveBakeSet(
    const ShaderAssetDesc& asset,
    const ShaderPassDesc& pass) noexcept;

/// 展开烘焙声明为具体变体列表。规则求并集, 应用 Skip, 排序去重。
///
/// 结果【总是】含默认变体: 一份 shader 至少要能在不开任何 keyword 时工作, 且这
/// 使空 BakeVariants 与"只烘默认"是同一件事。
///
/// 不设数量上限 —— 烘多少由作者的声明决定, 这里不做二次政策。
///
/// isInherited 为 true 时, Expand 里本域不存在的组名被静默投影掉 (见
/// ShaderAssetDesc::BakeVariants 的继承不对称说明); 为 false 时视为错误。
/// Combination 的未知 keyword 与组内多选【始终】是错误, 与继承无关。
std::optional<vector<ShaderVariantKey>> ExpandShaderBakeSet(
    const ShaderVariantDomain& domain,
    const ShaderBakeSetDesc& bake,
    bool isInherited,
    ShaderAssetDiagnostic& outDiag) noexcept;

// ============================ 产物数据 ============================

/// 128 位内容哈希。用于源码身份、artifact key 与 blob 文件名。
struct ShaderHash {
    uint64_t Low{0};
    uint64_t High{0};

    bool IsZero() const noexcept { return Low == 0 && High == 0; }
    /// 32 个小写 hex 字符, 高位在前。用作 blob 文件名。
    string ToHex() const;
    /// 解析 ToHex 的输出。长度或字符非法返回 nullopt。
    static std::optional<ShaderHash> FromHex(std::string_view hex) noexcept;

    friend bool operator==(const ShaderHash&, const ShaderHash&) noexcept = default;
    friend auto operator<=>(const ShaderHash&, const ShaderHash&) noexcept = default;
};

/// 源码身份: 源文件及其 #include 闭包的内容哈希。
///
/// 之所以要自己扫 #include: DXC 只暴露默认 include handler, 编译后拿不到依赖列表,
/// 因此无法事后得知该重编什么。扫描【不】求解 #if, 对被条件排除的 include 也一并
/// 计入 —— 这是安全的方向 (过度失效优于漏失效)。
struct ShaderSourceIdentity {
    ShaderHash Hash{};
    /// 参与哈希的所有文件, 相对 shader root 的 generic 路径, 已排序去重。
    vector<string> Dependencies;

    friend bool operator==(const ShaderSourceIdentity&, const ShaderSourceIdentity&) = default;
};

/// 一个 stage artifact 的身份输入。凡影响字节码者必须在此出现。
struct ShaderArtifactKeyParams {
    ShaderHash SourceIdentity{};
    std::string_view PassName{};
    render::ShaderStage Stage{render::ShaderStage::UNKNOWN};
    std::string_view EntryPoint{};
    render::HlslShaderModel ShaderModel{render::HlslShaderModel::SM60};
    /// 目标字节码类型。DXIL / SPIRV。
    render::ShaderBlobCategory Category{render::ShaderBlobCategory::DXIL};
    /// 已投影到本 stage 的全部宏 (pass.Defines + 选中 keyword)。顺序无关。
    std::span<const string> Defines{};
    bool IsOptimize{true};
    bool EnableUnbounded{true};
    /// 编译器工具链身份 (DXC 版本等)。换编译器必须换 key。
    ShaderHash ToolchainHash{};
};

/// 一条 blob 的元信息。字节码本身单独存文件。
struct ShaderArtifactEntry {
    ShaderHash Key{};
    string PassName;
    /// 编出本条产物的源文件, 相对 shader root 的 generic 路径。
    ///
    /// 一个资产内不同 pass 可以有不同的 Source (见 ShaderPassDesc::Source), 而 key
    /// 是按【该 pass 自己源文件】的身份算的, 故查找时必须能按源文件取到对应身份。
    string Source;
    render::ShaderStage Stage{render::ShaderStage::UNKNOWN};
    string EntryPoint;
    render::ShaderBlobCategory Category{render::ShaderBlobCategory::DXIL};
    /// 相对 artifact 目录的 blob 路径, 如 "dxil/a3f2....bin"。
    string BlobPath;
    /// 该 stage 投影后选中的 keyword, 已排序。空表示全关。
    ///
    /// 【不参与 Key, 也不用于查找】: 运行时按 (pass, stage, category, 投影后的
    /// defines, SourceIdentity, ToolchainHash) 纯函数算出 Key 再查, 故这里只是供
    /// 人工核对与工具使用的可读身份。空数组在序列化时可以省略, 缺失按空数组解析。
    vector<string> Keywords;
    /// 字节码内容哈希, 读取时校验完整性。
    ShaderHash BytecodeHash{};
    uint32_t BytecodeSize{0};

    friend bool operator==(const ShaderArtifactEntry&, const ShaderArtifactEntry&) = default;
};

/// cook 时某个源文件的身份记录。
struct ShaderArtifactSource {
    /// 相对 shader root 的 generic 路径, 与 manifest 的 Source 字段同一口径。
    string Path;
    /// cook 时该源文件 (含 include 闭包) 的身份。
    ShaderHash Identity{};

    friend bool operator==(const ShaderArtifactSource&, const ShaderArtifactSource&) = default;
};

/// 一个 shader 资产的 AOT 产物索引 (index.json)。
struct ShaderArtifactIndex {
    uint32_t FormatVersion{kShaderArtifactFormatVersion};
    /// cook 时 manifest 的 Name, 用于人工核对。
    string AssetName;
    /// cook 时每个源文件各自的身份。运行时用它判断产物是否已过期。
    ///
    /// 【按源文件分别记录】: key 是按 pass 自己源文件的身份算的, 若这里只存一份合并
    /// 哈希, 多源资产在 Strict 下永远判为过期、在 Lenient 下永远算错 key。
    vector<ShaderArtifactSource> Sources;
    ShaderHash ToolchainHash{};
    vector<ShaderArtifactEntry> Entries;

    Nullable<const ShaderArtifactEntry*> Find(ShaderHash key) const noexcept;
    /// 按源文件路径取 cook 时的身份。未记录该源文件返回 nullopt。
    std::optional<ShaderHash> FindSourceIdentity(std::string_view path) const noexcept;
};

/// 从 blob 文件读出的一份字节码。
struct ShaderArtifactBlob {
    ShaderHash Key{};
    render::ShaderStage Stage{render::ShaderStage::UNKNOWN};
    render::ShaderBlobCategory Category{render::ShaderBlobCategory::DXIL};
    /// 独立拥有的字节码。std::allocator 保证的对齐满足 SPIR-V 的 4 字节要求。
    vector<byte> Bytecode;
};

// ============================ 解析与烘焙数据 ============================

struct ShaderResolveConfig {
    /// shader include 根目录 (通常 <exe>/shaderlib)。
    std::filesystem::path ShaderRoot;
    /// manifest 所在路径, 用于定位同名 artifact 目录。
    std::filesystem::path ManifestPath;
    ShaderArtifactStaleness Staleness{ShaderArtifactStaleness::Strict};
    /// 允许在 AOT 未命中时 JIT。发布包应设为 false, 使缺失产物成为显式错误。
    bool AllowJit{true};
};

/// 解析结果。可直接喂给 render::Device::CreateShader。
/// 反射不在其中 —— manifest 是唯一 ABI 来源, PipelineLayout 无需反射即可构建。
struct ShaderBytecode {
    vector<byte> Data;
    render::ShaderBlobCategory Category{render::ShaderBlobCategory::DXIL};
    render::ShaderStage Stage{render::ShaderStage::UNKNOWN};
    ShaderBytecodeSource Source{ShaderBytecodeSource::Jit};
    ShaderHash Key{};

    /// 直接构造 CreateShader 所需的 descriptor。
    render::ShaderDescriptor MakeDescriptor() const noexcept;
};

struct ShaderCookOptions {
    /// shader include 根目录。
    std::filesystem::path ShaderRoot;
    /// manifest 路径。产物写入其同名目录。
    std::filesystem::path ManifestPath;
    /// 要烘焙的字节码类型。通常是 {DXIL, SPIRV}。
    vector<render::ShaderBlobCategory> Categories;
    /// 用反射核对 manifest 声明。默认开启 —— cook 期是发现 manifest 与 HLSL
    /// 不一致的最后时机, 关掉会把错误推迟到运行时建 PSO 失败。
    bool ValidateReflection{true};
    /// 已存在且 key 相同的 blob 不重写。
    bool Incremental{true};
};

struct ShaderCookStats {
    uint32_t Compiled{0};
    uint32_t Reused{0};
    /// 因 stage 投影去重而共享同一 blob 的次数。
    uint32_t Deduplicated{0};
};

struct ShaderCookResult {
    ShaderArtifactIndex Index;
    ShaderCookStats Stats;
    /// 全部失败与告警。空表示完全成功。
    vector<ShaderAssetDiagnostic> Diagnostics;

    bool Succeeded() const noexcept { return Diagnostics.empty(); }
};

// ============================ 功能类 ============================

/// 持有 render::PipelineLayoutDescriptor 所需的全部后备存储。
/// PipelineLayoutDescriptor 内部是 span, 必须有稳定的拥有者。move-only。
class ShaderPipelineLayoutStorage {
public:
    ShaderPipelineLayoutStorage() noexcept = default;
    ShaderPipelineLayoutStorage(const ShaderPipelineLayoutStorage&) = delete;
    ShaderPipelineLayoutStorage& operator=(const ShaderPipelineLayoutStorage&) = delete;
    ShaderPipelineLayoutStorage(ShaderPipelineLayoutStorage&&) noexcept = default;
    ShaderPipelineLayoutStorage& operator=(ShaderPipelineLayoutStorage&&) noexcept = default;

    /// 返回的 descriptor 内 span 指向本对象, 本对象存活且未被移动期间有效。
    render::PipelineLayoutDescriptor Get() const noexcept;

    size_t GroupCount() const noexcept { return _sets.size(); }
    bool HasPushConstant() const noexcept { return _pushConstant.has_value(); }

private:
    friend ShaderPipelineLayoutStorage BuildPipelineLayoutStorage(const ShaderPassDesc& pass);

    /// 每组的 entry 列表。稳定地址由 unique_ptr 保证 (vector 扩容不移动内容)。
    vector<unique_ptr<vector<render::ShaderParameterSetLayoutEntryDescriptor>>> _entries;
    vector<render::ShaderParameterSetLayoutDescriptor> _sets;
    std::optional<render::PushConstantDescriptor> _pushConstant;
};

/// 持有 render::VertexInputState 所需的后备存储。move-only。
class ShaderVertexInputStorage {
public:
    ShaderVertexInputStorage() noexcept = default;
    ShaderVertexInputStorage(const ShaderVertexInputStorage&) = delete;
    ShaderVertexInputStorage& operator=(const ShaderVertexInputStorage&) = delete;
    ShaderVertexInputStorage(ShaderVertexInputStorage&&) noexcept = default;
    ShaderVertexInputStorage& operator=(ShaderVertexInputStorage&&) noexcept = default;

    render::VertexInputState Get() const noexcept;

private:
    friend ShaderVertexInputStorage BuildVertexInputStorage(const ShaderVertexInputDesc& desc);

    /// VertexAttribute::Semantic 是 string_view, 需要稳定的字符串后备存储。
    vector<unique_ptr<string>> _semantics;
    vector<render::VertexBufferLayout> _buffers;
    vector<render::VertexAttribute> _attributes;
};

/// stage 字节码解析器。按 AOT 产物优先、JIT 兜底的顺序取字节码。
///
/// 非线程安全: 内部有惰性加载的 index 缓存与源码身份缓存。多线程编译应各持一个
/// 实例, 或由调用方加锁。
class ShaderResolver {
public:
    /// dxc 为空表示无 JIT 能力 (发布包), 此时 AllowJit 被强制视为 false。
    /// 【不持有生命周期】: dxc 必须在本对象存活期间保持有效, 通常由 RenderSystem 持有。
    ShaderResolver(
        ShaderResolveConfig config,
        Nullable<render::Dxc*> dxc) noexcept;

    /// 解析一个 stage 的字节码。defines 为已投影到该 stage 的完整宏集合。
    /// 失败原因写入 outDiag。
    std::optional<ShaderBytecode> Resolve(
        const ShaderPassDesc& pass,
        render::ShaderStage stage,
        render::ShaderBlobCategory category,
        std::span<const string> defines,
        ShaderAssetDiagnostic& outDiag) noexcept;

    /// 当前配置下源码身份 (惰性计算, 按源文件路径缓存)。
    std::optional<ShaderHash> GetSourceIdentity(
        std::string_view sourcePath,
        ShaderAssetDiagnostic& outDiag) noexcept;

    /// 工具链身份。DXC 版本 + artifact 格式版本。
    ShaderHash GetToolchainHash() const noexcept { return _toolchainHash; }

    bool CanJit() const noexcept;

private:
    /// 一次源码身份计算的缓存。
    ///
    /// 记录整个 include 闭包的时间戳: 命中时逐个 stat 复核, 任一变化即重算。
    /// 光缓存哈希是不够的 —— Strict 承诺"改 shader 立刻生效", 而 resolver 实例常常
    /// 跨越多次 Resolve 存活 (RenderSystem 持有), 只增不失效会让过期产物被判命中。
    struct SourceIdentityCache {
        string SourcePath;
        ShaderHash Hash{};
        vector<std::pair<string, std::filesystem::file_time_type>> Stamps;
    };

    /// 惰性加载 index.json。返回 nullptr 表示无产物目录 (正常情况, 非错误)。
    Nullable<const ShaderArtifactIndex*> GetIndex() noexcept;

    std::optional<ShaderBytecode> LoadFromArtifact(
        ShaderHash key,
        render::ShaderStage stage,
        ShaderAssetDiagnostic& outDiag) noexcept;

    std::optional<ShaderBytecode> CompileWithJit(
        const ShaderPassDesc& pass,
        render::ShaderStage stage,
        render::ShaderBlobCategory category,
        std::span<const string> defines,
        std::string_view sourcePath,
        ShaderHash key,
        ShaderAssetDiagnostic& outDiag) noexcept;

    ShaderResolveConfig _config;
    /// 借用而非拥有。见构造函数注释。
    Nullable<render::Dxc*> _dxc;
    ShaderHash _toolchainHash{};
    std::optional<ShaderArtifactIndex> _index;
    bool _indexLoaded{false};
    vector<SourceIdentityCache> _sourceIdentities;
};

// ============================ manifest 读写 ============================

/// 解析 JSON 文本并执行全量 manifest 自校验。任一规则不通过返回 nullopt。
std::optional<ShaderAssetDesc> ParseShaderAssetDesc(
    std::string_view json,
    ShaderAssetDiagnostic& outDiag) noexcept;

/// 读文件并解析。
std::optional<ShaderAssetDesc> LoadShaderAssetDesc(
    const std::filesystem::path& path,
    ShaderAssetDiagnostic& outDiag) noexcept;

/// 序列化回 JSON 文本 (字符串枚举, pretty)。用于工具链与 round-trip 测试。
std::optional<string> SerializeShaderAssetDesc(const ShaderAssetDesc& desc, bool pretty = true) noexcept;

// ============================ layout 构建 ============================

/// 从 manifest 构建 pipeline layout。【不需要】反射数据、字节码或 target 信息,
/// 因此结果对所有 target 与所有 keyword variant 都相同。
/// 输入必须来自 ParseShaderAssetDesc (已通过自校验)。
ShaderPipelineLayoutStorage BuildPipelineLayoutStorage(const ShaderPassDesc& pass);

ShaderVertexInputStorage BuildVertexInputStorage(const ShaderVertexInputDesc& desc);

// ============================ 反射核对 ============================

/// DXIL 反射一致性校验。方向为【声明 ⊇ 反射】:
/// - 反射出现但 manifest 未声明 -> 失败 (改了 HLSL 忘了改 manifest);
/// - manifest 声明但反射没有 -> 通过 (DCE / keyword #ifdef 的正常结果)。
bool ValidateShaderReflection(
    const ShaderPassDesc& pass,
    render::ShaderStage stage,
    const render::HlslShaderDesc& reflection,
    ShaderAssetDiagnostic& outDiag) noexcept;

/// SPIR-V 反射一致性校验。规则同上。
bool ValidateShaderReflection(
    const ShaderPassDesc& pass,
    render::ShaderStage stage,
    const render::SpirvShaderDesc& reflection,
    ShaderAssetDiagnostic& outDiag) noexcept;

// ============================ 哈希与身份 ============================

/// 稳定的字节内容哈希。跨平台跨编译器结果一致 —— cook 机器与运行机器可能不同,
/// 故刻意不用 radray::HashCode (它按 size_t 宽度分派, 32/64 位结果不同)。
ShaderHash HashShaderBytes(std::span<const byte> data) noexcept;

/// 计算源码身份。sourcePath 相对 shaderRoot。
/// 失败原因写入 outDiag (文件缺失、逃出 root、宏拼接的 #include 等)。
std::optional<ShaderSourceIdentity> ComputeShaderSourceIdentity(
    const std::filesystem::path& shaderRoot,
    std::string_view sourcePath,
    ShaderAssetDiagnostic& outDiag) noexcept;

/// 计算 stage artifact key。Defines 内部会排序去重, 故调用方无需保证顺序。
ShaderHash ComputeShaderArtifactKey(const ShaderArtifactKeyParams& params) noexcept;

/// 从 manifest 的 pass 取出编译该 stage 所需的 key 参数并计算 key。
/// defines 为已投影到该 stage 的完整宏集合。
std::optional<ShaderHash> ComputeShaderArtifactKey(
    const ShaderPassDesc& pass,
    render::ShaderStage stage,
    render::ShaderBlobCategory category,
    std::span<const string> defines,
    ShaderHash sourceIdentity,
    ShaderHash toolchainHash) noexcept;

/// 返回本次构建的工具链身份 (DXC 版本 + artifact 格式版本)。
ShaderHash GetShaderToolchainHash() noexcept;

// ============================ 产物路径 ============================

/// manifest 路径 -> 同名 artifact 目录。
/// "a/forward_pass.shader.json" -> "a/forward_pass"。
/// 去掉全部后缀 (.shader.json 两级), 故不会得到 "forward_pass.shader"。
std::filesystem::path GetShaderArtifactDirectory(const std::filesystem::path& manifestPath);

/// artifact 目录内 blob 的相对路径: "<category>/<key hex>.bin"。
string MakeShaderArtifactBlobPath(render::ShaderBlobCategory category, ShaderHash key);

/// 目标后端默认的字节码类型。
render::ShaderBlobCategory GetShaderBlobCategoryForBackend(render::RenderBackend backend) noexcept;

// ============================ 产物读写 ============================

/// blob 容器的读写。容器带头部 (magic + 版本 + stage/category + 内容哈希),
/// 使单个 blob 文件可独立自验, 不依赖 index.json 的正确性。
bool WriteShaderArtifactBlob(
    const std::filesystem::path& path,
    const ShaderArtifactEntry& entry,
    std::span<const byte> bytecode) noexcept;

/// 读取并校验 blob (magic / 版本 / 内容哈希)。任一不符返回 nullopt。
std::optional<ShaderArtifactBlob> ReadShaderArtifactBlob(
    const std::filesystem::path& path,
    ShaderAssetDiagnostic& outDiag) noexcept;

std::optional<ShaderArtifactIndex> ParseShaderArtifactIndex(
    std::string_view json,
    ShaderAssetDiagnostic& outDiag) noexcept;

std::optional<ShaderArtifactIndex> LoadShaderArtifactIndex(
    const std::filesystem::path& path,
    ShaderAssetDiagnostic& outDiag) noexcept;

std::optional<string> SerializeShaderArtifactIndex(
    const ShaderArtifactIndex& index,
    bool pretty = true) noexcept;

// ============================ AOT 烘焙 ============================
//
// 仅在启用 shader JIT (即有 DXC) 的构建里可用 —— cook 本质就是提前做 JIT。

#if defined(RADRAY_ENABLE_SHADER_JIT)

/// 烘焙一份 shader 资产。本轮只烘焙"全部 keyword 关闭"的默认组合
/// (manifest 的 KeywordGroups 仅声明合法组合域, bake set 属未来工作)。
///
/// 成功时产物目录内容为: index.json + <category>/<key>.bin。
ShaderCookResult CookShaderAsset(
    render::Dxc& dxc,
    const ShaderAssetDesc& asset,
    const ShaderCookOptions& options) noexcept;

/// 读 manifest 后 cook。
ShaderCookResult CookShaderAssetFile(
    render::Dxc& dxc,
    const ShaderCookOptions& options) noexcept;

#endif

// ============================ 格式化 ============================

std::string_view format_as(ShaderBindingResidency v) noexcept;
std::string_view format_as(ShaderBytecodeSource v) noexcept;
std::string_view format_as(ShaderArtifactStaleness v) noexcept;

}  // namespace radray

namespace radray {

// ============================ JSON 定制点 ============================
//
// 这些 codec 负责 JSON schema 与 FormatVersion，不执行跨字段语义校验；需要完整
// shader asset / artifact 校验时应调用 ParseShaderAssetDesc / ParseShaderArtifactIndex。

#define RADRAY_DECLARE_SHADER_JSON_CODEC(Type)                                    \
    template <>                                                                   \
    struct JsonSerializer<Type> {                                                 \
        static bool Write(JsonWriteContext& context, const Type& value) noexcept; \
    };                                                                            \
    template <>                                                                   \
    struct JsonDeserializer<Type> {                                               \
        static bool Read(const JsonValue& json, Type& value) noexcept;            \
    }

RADRAY_DECLARE_SHADER_JSON_CODEC(render::ShaderBindingLocation);
RADRAY_DECLARE_SHADER_JSON_CODEC(render::SamplerDescriptor);
RADRAY_DECLARE_SHADER_JSON_CODEC(ShaderBindingDesc);
RADRAY_DECLARE_SHADER_JSON_CODEC(ShaderBindingGroupDesc);
RADRAY_DECLARE_SHADER_JSON_CODEC(ShaderPushConstantDesc);
RADRAY_DECLARE_SHADER_JSON_CODEC(ShaderVertexAttributeDesc);
RADRAY_DECLARE_SHADER_JSON_CODEC(ShaderVertexBufferDesc);
RADRAY_DECLARE_SHADER_JSON_CODEC(ShaderVertexInputDesc);
RADRAY_DECLARE_SHADER_JSON_CODEC(ShaderStageDesc);
RADRAY_DECLARE_SHADER_JSON_CODEC(ShaderKeywordGroupDesc);
RADRAY_DECLARE_SHADER_JSON_CODEC(ShaderBakeRuleDesc);
RADRAY_DECLARE_SHADER_JSON_CODEC(ShaderBakeSetDesc);
RADRAY_DECLARE_SHADER_JSON_CODEC(ShaderPassDesc);
RADRAY_DECLARE_SHADER_JSON_CODEC(ShaderAssetDesc);
RADRAY_DECLARE_SHADER_JSON_CODEC(ShaderHash);
RADRAY_DECLARE_SHADER_JSON_CODEC(ShaderArtifactEntry);
RADRAY_DECLARE_SHADER_JSON_CODEC(ShaderArtifactSource);
RADRAY_DECLARE_SHADER_JSON_CODEC(ShaderArtifactIndex);

#undef RADRAY_DECLARE_SHADER_JSON_CODEC

}  // namespace radray
