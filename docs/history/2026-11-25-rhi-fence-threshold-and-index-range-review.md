> - 适用: 复查 RHI 映射校验对跳值 Fence 与直接索引绘制的处理
> - 权威: 基线 `3dbf528d` 上的双后端复现与修复记录；现行约定见 [RHI 与后端](../architecture/render-rhi.md)
> - 锚点: `modules/gpu_impl/src/rhi_validation.cpp`, `modules/gpu_impl/tests/test_buffer.cpp`, `modules/gpu_impl/tests/test_compute_binding.cpp`, `modules/gpu_impl/tests/test_render_pass.cpp`

# Fence 等待阈值与直接索引读取的复核

修订版[实施计划](2026-11-25-rhi-concurrency-and-async-runtime-plan.md)的 S2 验收覆盖了等待 Fence 后立即读取，以及部分刷新后合法绘制。以[首次实施](2026-11-25-rhi-async-runtime-implementation.md)的提交 `3dbf528d` 为基线，在 Windows x64、RTX 3060 上开启 RHI 与原生 GPU 校验，D3D12 和 Vulkan 均复现了以下两处边界问题。

- 同一队列对同一区间写入两次，第二次提交把新的 Fence 从 0 推进到 3。CPU 等待 1 后失效并读出正确内容，随后再次 Map、Unmap 却被要求重复失效。原校验只归并信号值小于或等于等待值的提交，漏掉了首次跨过该阈值的提交。本地 Fence 现在记录每次提交首次满足的等待阈值，覆盖 CPU 直接等待和下游队列等待。仍在调用原生 Submit 的信号在返回前保留，失败后撤销。外部可共享的 Fence 还可能由其他进程推进，只使用已知完成值。
- 新建 8192 字节的 Upload 索引缓冲，只刷新前 4096 字节。直接 `DrawIndexed` 从字节 4096 读取三个 16 位索引时，旧校验放行了提交。现在按绑定偏移、首索引、索引宽度和数量检查确切区间。测试还验证补刷这 6 个字节后重试同一条命令，读回得到正确的整幅三角形图像。

两处均已修复。Profiler 开启和关闭的独立构建各通过 11 个相关用例，覆盖 CPU 等待、跨队列等待、受阻的后续覆写、原生提交失败回滚、刷新后的合法绘制和未刷新索引的拒绝。

完整 Debug 套件共 1078 项，1074 项通过、0 项失败、4 项跳过。跳过项为 D3D12 根签名预算用例的 Vulkan 参数，以及适配器不支持的 Vulkan 向 D3D12 导出 Buffer、Texture 和 Fence 三条路线。
