#pragma once

#include <filesystem>

#include <radray/runtime/asset.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/shader_program.h>

// shader 资产层: 一份 manifest = 一个 Asset。
//
// == 三层的分工 ==
//
//   shader_manifest.h  格式层。manifest desc、变体域、产物索引、ShaderResolver、cook。
//                      不含 Asset —— tools/shader_cook 只需要这一层, 不该为此吃下
//                      AssetManager 传递带来的 stdexec 编译开销。
//   shader_program.h   对象层。ShaderPassProgram: PipelineLayout + 字节码缓存。
//                      不含 Asset, 故 material 层可以单独用它。
//   shader_asset.h     资产层 (本文件)。ShaderAsset 把上面两层接进 AssetManager。
//
// 这条分界照 image_data.h (数据格式) 与 image_asset.h (Asset) 的既有先例。

namespace radray {

namespace render {
class Device;
}  // namespace render

/// 一份 shader manifest 的资产。
///
/// 【粒度 = manifest, 不是 (manifest, pass)】: 产物布局已经定死了这条边界 ——
/// index.json 每份 manifest 一个, artifact 目录由 manifest 路径推导,
/// ShaderArtifactIndex::Entries 混装该 manifest 下所有 pass 的 stage (PassName 只是
/// 条目里的一个字段)。若按 pass 切, 两个资产会共享一份 index.json 与一份 resolver
/// 缓存, "谁持有 resolver" 立刻无解。PipelineLayout 的 per-pass 性质落在资产内部
/// 分层 (ShaderPassProgram), 不上升为资产边界。
///
/// 【构造即完整的兑现方式】: Asset 要求放进 AssetManager 时已可用, 而字节码本质按
/// variant 惰性。解法是加载期只做 variant / target 【无关】的部分 —— desc 解析、
/// PipelineLayout、vertex input storage。"完整"指 ABI 与 layout 就绪, 不指所有变体
/// 已编译。字节码归 ShaderPassProgram::GetOrCreateVariant 惰性。
///
/// 【这也让加载路径不碰 DXC】: 加载协程里只有同步文件 IO 与同步 GPU 调用, 两者都短,
/// 所以 AssetManager 的单线程泵不会被 JIT 阻塞。JIT 发生在后续 GetOrCreateVariant,
/// 那是调用方主动触发的。
class ShaderAsset : public Asset {
public:
    ShaderAsset(
        ShaderAssetDesc desc,
        unique_ptr<ShaderResolver> resolver,
        vector<unique_ptr<ShaderPassProgram>> passes) noexcept;
    ~ShaderAsset() noexcept override;

    void OnUnload(IRenderResourceRecycler& recycler) override;
    AssetTypeId GetTypeId() const noexcept override;

    bool IsValid() const noexcept { return !_passes.empty(); }

    const string& GetName() const noexcept { return _desc.Name; }
    const ShaderAssetDesc& GetDesc() const noexcept { return _desc; }

    /// 返回的指针在资产存活期内稳定 (unique_ptr 后备存储), 材质层可直接持有。
    Nullable<ShaderPassProgram*> FindPass(std::string_view name) noexcept;
    Nullable<const ShaderPassProgram*> FindPass(std::string_view name) const noexcept;

    size_t GetPassCount() const noexcept { return _passes.size(); }
    Nullable<ShaderPassProgram*> GetPass(size_t index) noexcept;

private:
    ShaderAssetDesc _desc;
    /// 【必须声明在 _passes 之前】: program 借用 resolver 裸指针, 析构逆序保证
    /// program 先死。unique_ptr 而非值, 是因为 program 存的指针要在资产被 move
    /// 构造后仍然有效 (Asset 本身不可 move, 但保持这条不变量更稳)。
    unique_ptr<ShaderResolver> _resolver;
    vector<unique_ptr<ShaderPassProgram>> _passes;
};

template <>
struct RuntimeTypeTrait<ShaderAsset> {
    static constexpr RuntimeTypeId value{0x6f2a91c4, 0x3e58, 0x4b07, 0x9d, 0x62, 0x14, 0xa7, 0x8c, 0x35, 0xe0, 0x1b};
    using Bases = std::tuple<Asset>;
};

/// 【本结构刻意只有一个字段】: ShaderRoot / Staleness / AllowJit / Dxc 曾在这里各占
/// 一项, 于是每个加载调用点都能自行决定"这是开发构建还是发布包"。那是一个进程级答案,
/// 放在这里就成了第二套真相 —— 而且从未被兑现: AssetId 只哈希 manifest 路径
/// (MakeShaderAssetId), AssetManager 按 id 去重后直接返回既有 handle, 第二次调用携带
/// 的 options 连协程都没启动就被静默丢弃。现在这四项统一落在 ShaderResolveContext。
struct ShaderAssetLoadOptions {
    /// 全进程共享的解析上下文 (ShaderRoot / Staleness / AllowJit / Dxc / 源码缓存)。
    /// 【不持有生命周期】: 必须在资产存活期间保持有效, 通常由 RenderSystem 持有。
    /// 为空则加载失败 —— 不猜 include 根, 见 CreateShaderAsset。
    Nullable<ShaderResolveContext*> Context{nullptr};
};

/// manifest 路径 -> AssetId。前缀 "shader:" 做命名空间隔离, 同一路径在不同资产类型下
/// 必须得到不同 id。
///
/// 【身份是"这份文件"而非"逻辑资产名"】: manifest 在源码树与输出目录各有一份, 绝对
/// 路径不同故 id 不同 —— 这是正确的 (确实是两份文件, 产物目录也各自独立)。按逻辑名
/// 寻址需要另一层名字->路径映射, 属材质层。
AssetId MakeShaderAssetId(const std::filesystem::path& manifestPath);

/// 同步创建 ShaderAsset。读 manifest、建每个 pass 的 PipelineLayout 与 vertex input。
/// 不编译任何字节码。任一 pass 失败则整体失败 (返回 nullptr, 原因写入 outDiag)。
///
/// options.Context 为空即失败。旧版在 ShaderRoot 留空时按"父目录的父目录"猜 include 根,
/// 那个兜底存在恰恰说明当时拿不到唯一真相 —— context 里有确切答案, 不必猜。
Nullable<unique_ptr<ShaderAsset>> CreateShaderAsset(
    render::Device& device,
    const std::filesystem::path& manifestPath,
    const ShaderAssetLoadOptions& options,
    ShaderAssetDiagnostic& outDiag) noexcept;

StreamingAssetRef<ShaderAsset> LoadShaderAsset(
    AssetManager& assetManager,
    render::Device& device,
    const std::filesystem::path& manifestPath,
    const ShaderAssetLoadOptions& options);

StreamingAssetRef<ShaderAsset> LoadShaderAsset(
    AssetManager& assetManager,
    const AssetId& assetId,
    render::Device& device,
    const std::filesystem::path& manifestPath,
    const ShaderAssetLoadOptions& options);

}  // namespace radray
