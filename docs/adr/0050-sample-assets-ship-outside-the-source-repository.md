# ADR-0050 样例与测试资产在源码仓库之外分发

状态: 生效
日期: 2026-08
影响: `.gitignore` 的 `assets` 条目、`assets/` 目录、`examples/example_lambert_sphere` 的运行
前置、`docs/architecture/asset-system.md`、`docs/guide/build-test.md`；部分取代 ADR-0040

## 背景

ADR-0040 把 asset 身份权威定为 `<AssetRoot>/assets.json`，并在论证里写下「身份权威必须与源文件
一同进版本控制」。这句话当时描述的是一个真实约束：GUID 是永久身份（ADR-0036），引用方一旦写下
它，它就必须在任意克隆上指向同一份资产；如果清单不与资产一起走，GUID 立刻失效。

但这条论证被当成了「进 RadRay 源码仓库」。实际仓库状态与之相反：`.gitignore` 有一条 `assets`，
`git ls-files assets/` 为空，`assets/` 下的 obj 与 png 从未被跟踪。于是 ADR-0040 的「必须保持为
真」里有一条在写下时就不成立，而 `example_lambert_sphere` 依赖本地已存在的资产才能跑——一个新
克隆拿不到它们，且没有任何文档说明该去哪里取。

## 决策

**样例与测试资产连同 `assets.json` 在 RadRay 源码仓库之外分发；ADR-0040 要求的「与源文件同行」
指同一个资产包内的原子性，不指同一个 git 仓库。**

- `assets/` 整体被 `.gitignore` 忽略，包括清单。二进制资产不进引擎源码仓库。
- 清单与它描述的资产必须始终处于同一版本，并作为一个整体分发。这条不变量是 GUID 稳定性的
  实际依据，与承载渠道无关。
- 游戏项目自行选择渠道（自己的 git、LFS、资产服务器、打包分发），并传入自己的
  `ApplicationRuntimeDescriptor::AssetRoot`。引擎侧零改动。
- 依赖资产的样例不是自动化测试。需要在 CI 中回归的渲染路径必须自建程序化资产，
  例如 `modules/runtime/tests/test_forward_pipeline.cpp` 在内存里构造 quad 与 1x1 贴图。

## 放弃的方案及代价

- **把 `assets/` 纳入版本控制**：与仓库当前状态一致的做法，但要把 obj/png 二进制永久写进引擎
  历史。引擎仓库会随每次样例资产迭代膨胀，而这些资产对使用引擎的项目零价值。ADR-0040 的动机
  （文本、可 merge、冲突集中易解）恰恰是为了避免二进制权威，把二进制资产塞进同一个仓库与那条
  动机方向相反。
- **只把 `assets.json` 纳入版本控制，二进制留在外面**：清单与资产会分处两条版本线，于是
  「清单描述的文件不存在」变成常态而非错误。ADR-0040 已经为缺失文件保留条目与 GUID，那是给
  重构留的余地，不该被当成默认状态。
- **改 ADR-0040 的正文让它与现状一致**：本轮最初就是这么做的，属流程违规。ADR 一旦写下即冻结
  （`docs/adr/README.md`），修订状态行是唯一允许的改动。改正文会让「当时怎么想的」这项唯一价值
  失效——读者再也无法判断某句话是原始判断还是后来的补丁。
- **让样例在缺资产时自建程序化替代**：样例的用途是展示装配真实资产的推荐路径，内建 fallback
  会让它同时展示两条路径，且 fallback 必须与真实路径同步维护。程序化资产属于测试，已落在
  `test_forward_pipeline`。

## 必须保持为真

- `assets/` 及其中的 `assets.json` 不进 RadRay 源码仓库；`git ls-files assets/` 为空。
- 清单与它描述的资产始终同版本、同渠道、原子分发。
- 代码中不出现硬编码资产根；资产根一律由装配方通过 `ApplicationRuntimeDescriptor::AssetRoot`
  传入（ADR-0040 的这条不变量不变）。
- CTest 注册的用例不依赖 `assets/` 下的任何文件。依赖资产的可执行文件只能是样例。
- ADR-0040 除状态行外正文不再改动；资产承载渠道的当前结论以本记录为准。
