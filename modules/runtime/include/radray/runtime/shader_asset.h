#pragma once

#include <filesystem>

#include <radray/runtime/asset.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/shader_program.h>

// shader 资产层: 一份 manifest = 一个 Asset。把格式层与对象层接进 AssetManager。
//
// 三层分工见 docs/architecture/shader-pipeline.md。这条分界照 image_data.h (数据格式)
// 与 image_asset.h (Asset) 的既有先例。

namespace radray {

/// 一份 shader manifest 的资产。
///
/// 【粒度 = manifest, 不是 (manifest, pass)】产物布局已定死这条边界: index.json 每份
/// manifest 一个。PipelineLayout 的 per-pass 性质落在资产内部分层 (ShaderPassProgram)。
///
/// 【构造即完整】加载期只做 variant / target 无关的部分 —— desc 解析、PipelineLayout、
/// vertex input storage。字节码归 ShaderPassProgram::GetOrCreateVariant 惰性, 故加载
/// 路径不碰 DXC, AssetManager 的单线程泵不会被 JIT 阻塞。
class ShaderAsset : public Asset {
public:
    ShaderAsset(
        ShaderAssetDesc desc,
        unique_ptr<ShaderResolver> resolver,
        vector<unique_ptr<ShaderPassProgram>> passes,
        ShaderResolveContext* context,
        PipelineLayoutCache* layoutCache) noexcept;
    ~ShaderAsset() noexcept override;

    void OnUnload(AssetManager& manager) override;
    RuntimeTypeId GetTypeId() const noexcept override;

    bool IsValid() const noexcept { return !_passes.empty(); }

    const string& GetName() const noexcept { return _desc.Name; }
    const ShaderAssetDesc& GetDesc() const noexcept { return _desc; }

    /// 返回的指针在【本资产】存活期内稳定 (unique_ptr 后备存储)。持有一份
    /// StreamingAssetRef<ShaderAsset> 即保证它不悬垂。
    Nullable<ShaderPassProgram*> FindPass(std::string_view name) noexcept;
    Nullable<const ShaderPassProgram*> FindPass(std::string_view name) const noexcept;

    size_t GetPassCount() const noexcept { return _passes.size(); }
    Nullable<ShaderPassProgram*> GetPass(size_t index) noexcept;

    /// 建本资产时用的共享设施, 供 dedup 命中时核对 (见 LoadShaderAsset)。
    /// 【只用于比较, 不解引用】LayoutCache 允许先于资产销毁, 故 OnUnload 后可能已悬垂。
    Nullable<ShaderResolveContext*> GetResolveContext() const noexcept { return _context; }
    Nullable<PipelineLayoutCache*> GetLayoutCache() const noexcept { return _layoutCache; }

private:
    ShaderAssetDesc _desc;
    /// 【必须声明在 _passes 之前】program 借用 resolver 裸指针, 析构逆序保证 program 先死。
    unique_ptr<ShaderResolver> _resolver;
    vector<unique_ptr<ShaderPassProgram>> _passes;
    /// 【只记不用】供 dedup 核对, 不解引用, 故允许悬垂。
    ShaderResolveContext* _context{nullptr};
    PipelineLayoutCache* _layoutCache{nullptr};
};

template <>
struct RuntimeTypeTrait<ShaderAsset> {
    static constexpr RuntimeTypeId value{0x6f2a91c4, 0x3e58, 0x4b07, 0x9d, 0x62, 0x14, 0xa7, 0x8c, 0x35, 0xe0, 0x1b};
    using Bases = std::tuple<Asset>;
};

/// 【只放进程级共享设施的指针, 不放 per-load 决策】所有调用点必须传同一个, 传错不会
/// 产生"两套策略", 只会产生一个错误的依赖注入。这条约定由 LoadShaderAsset 机械兑现
/// (dedup 命中时核对并 abort), 不只是注释。
struct ShaderAssetLoadOptions {
    /// 全进程共享的解析上下文 (ShaderRoot / Staleness / AllowJit / Dxc / 源码缓存)。
    /// 【不持有生命周期】必须在资产存活期间保持有效, 通常由 RenderSystem 持有。
    /// 为空则加载失败 —— 不猜 include 根。
    Nullable<ShaderResolveContext*> Context{nullptr};
    /// PipelineLayout 的内容去重缓存, 通常由 RenderSystem 持有。
    /// 【不持有生命周期, 但允许它先于资产销毁】layout 按引用计数自保。
    /// 【为空即加载失败】不提供"绕过缓存直接建 layout"的回退, 那会让所有权有两条路径。
    /// 【它同时决定了资产用哪个 device】故加载 shader 资产不需要另传 device。
    Nullable<PipelineLayoutCache*> LayoutCache{nullptr};
};

/// manifest 路径 -> AssetId。归一化与命名空间隔离的规则见 MakeAssetIdFromPath。
///
/// 【身份是"这份文件"而非"逻辑资产名"】manifest 在源码树与输出目录各有一份, 归一化后
/// 路径仍不同故 id 不同 —— 这是正确的, 它们确实是两份文件, 产物目录也各自独立。
/// 按逻辑名寻址需要另一层名字->路径映射, 属材质层。
AssetId MakeShaderAssetId(const std::filesystem::path& manifestPath);

/// 同步创建 ShaderAsset。读 manifest、建每个 pass 的 PipelineLayout 与 vertex input。
/// 不编译任何字节码。任一 pass 失败则整体失败 (返回 nullptr, 原因写入 outDiag)。
///
/// 刻意不收 render::Device (LayoutCache 已绑定一个) 也不收 AssetManager&。
Nullable<unique_ptr<ShaderAsset>> CreateShaderAsset(
    const std::filesystem::path& manifestPath,
    const ShaderAssetLoadOptions& options,
    ShaderAssetDiagnostic& outDiag) noexcept;

/// 经 AssetManager 加载 (按 id 去重)。
///
/// 【options 必须在发起加载之前校验】dedup 命中时 request.Task 那个协程帧一次都不 resume,
/// 故校验若只写在 CreateShaderAsset 里, 第二次调用带的空 options 会被静默接受。
/// options 不合法时返回无效 ref 且不发起加载。
///
/// dedup 命中且既有资产用的是【另一份】Context / LayoutCache 时 abort —— 那是依赖注入
/// 接错了线, 不是可恢复的运行时状况。
StreamingAssetRef<ShaderAsset> LoadShaderAsset(
    AssetManager& assetManager,
    const std::filesystem::path& manifestPath,
    const ShaderAssetLoadOptions& options);

StreamingAssetRef<ShaderAsset> LoadShaderAsset(
    AssetManager& assetManager,
    const AssetId& assetId,
    const std::filesystem::path& manifestPath,
    const ShaderAssetLoadOptions& options);

}  // namespace radray
