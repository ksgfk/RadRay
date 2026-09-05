# ADR-0056 RTTI 开关归工程统一编译函数所有

状态: 生效；取代 ADR-0055 第 5 条中由 core PUBLIC 传播 RTTI 选项的部分
日期: 2026-09-05
影响: 工程 C++ 编译选项、全部自有 target 的配置入口与外部 consumer 的构建边界

## 背景

ADR-0055 把 RTTI 开关放在 `radraycore` 的 PUBLIC usage requirement 上。core 的
`runtime_type.h` 已只负责稳定 GUID 声明，实际对象查询位于 runtime。当前自有静态库和最终
程序由同一工程构建，`radray_default_compile_flags` 已集中管理语言、异常与平台选项，RTTI
也应由这个入口统一设置。检查中发现 `bench_read_obj` 尚未调用该函数，需要同步补齐。

## 决策

- 在 `radray_default_compile_flags` 中以 `PRIVATE` 设置 `/GR`（MSVC/ClangCL）或
  `-frtti`（其他前端），使用 `COMPILE_LANGUAGE:CXX` 限定语言，覆盖全部构建配置。
- 每个自有库、工具、样例和 benchmark 都显式调用该函数；测试由 `radray_add_test` 代为调用。
- 删除 core 的 PUBLIC RTTI 选项。第三方目标保持各自配置，仓库外使用公共对象查询的
  consumer 负责在自己的构建中开启 RTTI。
- ADR-0055 的对象查询、GUID、资产视图和服务装配决策保持生效；不改变 LTO 策略。

## 放弃的方案及代价

- **继续从 core PUBLIC 传播**：能自动配置任意链接 core 的 consumer，但把工程编译策略挂在
  core 的公共接口上，扩大了仅使用基础设施时的构建要求。
- **全局 `add_compile_options`**：会影响第三方子目录，超出自有 target 的约束范围。
- **新增构建选项 INTERFACE target**：当前已有统一函数，增加另一条接入路径没有实际收益。

## 必须保持为真

- 新增自有 target 必须接入统一编译函数，不在个别目标上关闭 RTTI。
- 跨静态库查询涉及的多态对象与转换代码均开启 RTTI；不能依赖 LTO 消除转换来保证正确性。
- core 的链接接口不再导出 RTTI 选项，Debug 与 Release 均验证最终生成的编译配置和对象查询。
