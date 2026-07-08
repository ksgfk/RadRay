#pragma once

#include <span>

#include <radray/types.h>
#include <radray/render/common.h>
#include <radray/render/shader_variant_cache.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/shader_asset.h>
#include <radray/runtime/texture_asset.h>
#include <radray/runtime/render_framework/material_constant_binder.h>
#include <radray/runtime/render_framework/render_pipeline.h>

namespace radray {

class SamplerCache;


/// 组件产出的 per-section【绘制决策快照】(对应 UE5 的 FMaterialRenderProxy, 但走不可变快照
/// 而非 enqueue-to-RT 模型)。
///
/// 所有权/生产者:
/// - 权威是【组件】(StaticMeshComponent), 由它决定"下一帧某 section 用哪个材质"并生成快照。
/// - MaterialAsset 仅是【共享只读模板】: 多个 primitive 可引用同一份, MaterialAsset::CreateSnapshot()
///   只是组件调用的生成器, 把当前模板状态冻结成本次绘制用的快照。
///
/// 设计要点:
/// - game 线程在组件 Tick 时【按需】(脏才重建) 生成, 之后不可变; render 线程只读, 无锁无竞争
///   (通过 atomic<shared_ptr<const MaterialRenderSnapshot>> 发布)。
/// - 冻结渲染所需的一切: shader 引用 (稳定 ProgramId + pass) + 启用 keyword + renderQueue
///   + 常量字节块 + 纹理引用 (StreamingAssetRef + 子 view 描述值) + 采样器描述值。全为值/资产引用,
///   零裸指针: 绑定时经 TextureAsset view 缓存 / SamplerCache 换成稳定指针。
/// - shared_ptr 引用计数自动管理生命周期: DrawItem 持一份 shared_ptr, 快照存活期覆盖整个渲染,
///   即便 MaterialAsset 在此期间被 Unload 也不悬垂。
/// - ResolveVariant / ApplyProperties / IsTransparent 从快照自身求解, 不再回查 MaterialAsset。
struct MaterialRenderSnapshot {
    /// 一条常量 property。Name 可以是:
    /// - cbuffer 块名 (整块覆盖, 对应 SetConstantBlock);
    /// - cbuffer 块内字段名 (对应 Unity 的 SetFloat("_Color"), 由 MaterialConstantBinder
    ///   按 shader 反射的字段偏移打进所属块)。
    /// 绑定时经 MaterialConstantBinder 用变体反射解析归属块并整块提交 (push constant 或 CBV)。
    struct ConstantEntry {
        string Name;
        vector<byte> Bytes;
    };
    /// 一条纹理 property。绑定时经 Texture->GetOrCreateSrv(SubView) 取稳定 view 指针:
    /// SubView 默认 (IsDefault) 走默认全量 SRV, 否则取同一贴图的非默认子 view。
    /// 存 asset 引用 + 描述值 (零裸指针): 快照可安全跨帧/跨线程持有, 无悬垂风险。
    struct TextureEntry {
        string Name;
        StreamingAssetRef<TextureAsset> Texture{};
        TextureSubViewDesc SubView{};
    };
    /// 一条采样器 property。存 descriptor 值 (而非裸指针): 绑定时经 SamplerCache 去重取稳定指针,
    /// 因此快照可安全跨帧/跨线程持有, 无悬垂风险 (对齐 UE5 的 sampler cache 语义)。
    struct SamplerEntry {
        string Name;
        render::SamplerDescriptor Desc{};
    };

    /// shader 引用 (非拥有 streaming ref)。ResolveVariant 用它懒编译变体。
    StreamingAssetRef<ShaderAsset> Shader{};
    /// 启用的 keyword 名字 (投影用)。
    vector<string> EnabledKeywords;
    int32_t RenderQueue{static_cast<int32_t>(radray::RenderQueue::Geometry)};
    vector<ConstantEntry> Constants;
    vector<TextureEntry> Textures;
    vector<SamplerEntry> Samplers;

    bool IsTransparent() const noexcept {
        return RenderQueue >= static_cast<int32_t>(radray::RenderQueue::Transparent);
    }

    /// 解析指定 pass 在快照 keyword 集下的变体。shader 空 / pass 越界 / 编译失败返回 nullptr。
    Nullable<const render::CompiledShaderVariant*> ResolveVariant(
        render::ShaderVariantCache& cache,
        uint32_t passIndex,
        render::HlslShaderModel sm = render::HlslShaderModel::SM60) const noexcept;

    /// 把快照的常量 property 收集为打包器输入 (块名 / 字段名 + 字节)。
    /// 供 MaterialConstantBinder::Bind 消费; 返回的 span 借用 out 存储。
    /// out 会被清空重填; 返回项的 Bytes 借用本快照的 Constants 存储 (快照存活期内有效)。
    void CollectConstants(vector<MaterialConstantValue>& out) const noexcept;

    /// 把快照的纹理 / 采样器 property 写入 ShaderParameterTable。sampler 经 samplerCache
    /// 按 descriptor 去重取稳定指针。返回成功写入的 property 数。
    /// 常量 (cbuffer 字段) 不在此处理: 由 MaterialConstantBinder 用变体反射打包。
    uint32_t ApplyResources(render::ShaderParameterTable& table, SamplerCache& samplerCache) const noexcept;

    /// 把快照的 property 值写入 ShaderParameterTable。sampler 经 samplerCache 按 descriptor
    /// 去重取稳定指针。返回成功写入的 property 数。
    /// 注意: 常量走整块 SetBytes (仅当 Name 恰为块名且 size 匹配), 无字段级打包;
    /// 字段级打包需用 MaterialConstantBinder。本方法保留用于 headless 测试 / 兼容路径。
    uint32_t ApplyProperties(render::ShaderParameterTable& table, SamplerCache& samplerCache) const noexcept;
};

}  // namespace radray
