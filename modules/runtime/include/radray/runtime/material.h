#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <variant>

#include <radray/nullable.h>
#include <radray/types.h>
#include <radray/render/rhi.h>
#include <radray/runtime/material_tag.h>

namespace radray {

// Material defaults; each Pass explicitly chooses which fields to use or override.
// Attachment formats, sampling and geometry topology are supplied by the Pass.
struct MaterialRenderState {
    render::CullMode Cull{render::CullMode::Back};
    std::optional<render::BlendState> Blend;
    render::ColorWrites ColorWriteMask{render::ColorWrite::All};

    render::CompareFunction DepthCompare{render::CompareFunction::LessEqual};
    render::DepthBiasState DepthBias{};
    std::optional<render::StencilState> Stencil;

    // Keep byte-sized fields together after the naturally aligned state fields.
    bool DepthTestEnable{true};
    bool DepthWriteEnable{true};
    uint8_t StencilReference{0};
};

struct MaterialProperties {
    MaterialTagSet Tags;
    MaterialRenderState RenderState;
};

// 借用 RHI 对象，拥有入口名称字符串；空 PS 表示仅顶点阶段的程序。
struct MaterialShaderProgram {
    string Name;
    Nullable<render::PipelineLayout*> Layout{nullptr};
    Nullable<render::Shader*> VertexShader{nullptr};
    string VertexEntry;
    Nullable<render::Shader*> PixelShader{nullptr};
    string PixelEntry;
};

// 材质所有权与 UE 对照见 docs/architecture/runtime.md。
// 使用前完成准备；被引用期间保持地址稳定、内容不变。
// 程序名称必须唯一；列表表达可用 shader，不决定 Pass 参与或持有 PSO。
struct MaterialResource {
    MaterialProperties Properties;
    vector<MaterialShaderProgram> Programs;

    // 准备阶段解析名称；Programs 不变时索引保持有效。
    std::optional<size_t> FindProgram(std::string_view name) const noexcept;
    Nullable<const MaterialShaderProgram*> GetProgram(size_t index) const noexcept;
};

// 资源参数借用 RHI 对象；空指针表示尚未赋值。
using MaterialParameterValue = std::variant<float, array<float, 4>, Nullable<render::TextureView*>, Nullable<render::Sampler*>>;

struct MaterialParameter {
    string Name;
    MaterialParameterValue DefaultValue;
};

// 参数名称、类型、顺序与默认值在创建后保持不变。
// 借用 Resource；它必须晚于本定义及由其生成的所有 Proxy 销毁。
// 有 MaterialInstance 借用本定义时，不得搬移、替换或销毁它。
class Material {
public:
    // 拒绝空名称和重名；不编译 shader，不校验调用方的 shader 参数 ABI。
    static std::optional<Material> Create(const MaterialResource& resource, std::span<const MaterialParameter> parameters);
    const MaterialResource& GetResource() const noexcept;
    std::span<const MaterialParameter> GetParameters() const noexcept;
    // 索引属于本定义的声明顺序，不能跨 Material 假设其含义一致。
    std::optional<size_t> FindParameter(std::string_view name) const noexcept;
    Nullable<const MaterialParameterValue*> GetDefaultParameter(size_t index) const noexcept;

private:
    Material(const MaterialResource& resource, std::span<const MaterialParameter> parameters);
    const MaterialResource* _resource;
    vector<MaterialParameter> _parameters;
};

// 借用一份地址稳定、内容不变的 Material；不支持父实例链或静态 shader 变体。
class MaterialInstance {
public:
    explicit MaterialInstance(const Material& material);
    const Material& GetMaterial() const noexcept;
    Nullable<const MaterialParameterValue*> GetParameter(size_t index) const noexcept;
    // 拒绝未知索引或类型改变；失败时保留现有覆盖。
    bool SetParameter(size_t index, MaterialParameterValue value);
    bool ResetParameter(size_t index) noexcept;

private:
    const Material* _material;
    vector<std::optional<MaterialParameterValue>> _overrides;
};

// Scene 拥有的渲染快照，不保留用户侧 Material / MaterialInstance 引用。
// RHI 资源和 MaterialResource 仍为借用；调用方须串行组织 Update/Get。
class MaterialRenderProxy {
public:
    // 显式同步：后续实例编辑只有再次 Update 才能进入本快照。
    void Update(const Material& material);
    void Update(const MaterialInstance& instance);
    Nullable<const MaterialResource*> GetResource() const noexcept;
    // 返回的引用不能跨 Update、Proxy 搬移或所属 SparseSet 记录搬移保存。
    Nullable<const MaterialParameterValue*> GetParameter(size_t index) const noexcept;

private:
    Nullable<const MaterialResource*> _resource{nullptr};
    vector<MaterialParameterValue> _parameters;
};

}  // namespace radray
