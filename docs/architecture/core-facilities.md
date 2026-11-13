> - 适用: 需要某个基础设施但不确定仓库里已有什么；踩到 core 的坑
> - 权威: 本文是 `radraycore` 提供什么、以及怎么正确用它的唯一说明
> - 锚点: `modules/core/include/radray/types.h`, `modules/core/include/radray/inline_vector.h`, `modules/core/include/radray/allocator.h`, `modules/core/src/allocator.cpp`, `modules/core/include/radray/memory.h`, `modules/core/src/memory.cpp`, `modules/core/include/radray/nullable.h`, `modules/core/include/radray/coroutine.h`, `modules/core/include/radray/enum_flags.h`, `modules/core/include/radray/basic_math.h`, `modules/core/include/radray/viewport.h`, `modules/core/include/radray/runtime_type.h`, `cmake/Utility.cmake`, `modules/core/src/image_data.cpp`, `modules/core/src/png_codec.h`, `modules/core/src/png_codec.c`, `modules/core/src/jpeg_codec.h`, `modules/core/src/jpeg_codec.c`

# core 基础设施

`radraycore` 不涉及 GPU。写新代码前先看这里有没有现成的东西。

## 常用头文件

几乎每个 `.cpp` 都会碰到的：

| 头文件 | 提供 |
|---|---|
| `types.h` | 项目普通容器与字符串别名的唯一出口 |
| `logger.h` | `radray::format`、日志宏、`RADRAY_ABORT`、`RADRAY_ASSERT` |
| `nullable.h` | `Nullable<T>` |
| `enum_flags.h` | `EnumFlags<T>` + `magic_enum` 的唯一出口 |
| `basic_math.h` | Eigen 封装 + 视图/投影辅助；48 B `AffineTransform` 和 24 B 三维 `Aabb` |
| `hash.h` | `HashCode` 增量哈希、`StringHash`、`PodHasher` |
| `coroutine.h` | `task`、`TaskScope`、`ManualCoroutineScheduler` |
| `utility.h` | `StaticCastUniquePtr`、`Unreachable`、`RADRAY_UNUSED` |
| `scope_guard.h` | `ScopeGuard` / `MakeScopeGuard` |

专用的：`json.h`（yyjson）、`xml.h`（pugixml）、`binary_io.h`（小端读写）、`file.h`、`environment.h`、
`dynamic_library.h`、`guid.h`、`text_encoding.h`、`runtime_type.h`、
`allocator.h`（偏移区间子分配，与堆无关）、`memory.h`、`sparse_set.h`、`channel.h`、
`intrusive_ptr.h`、`structured_buffer.h`、`image_data.h`、`viewport.h`、
`triangle_mesh.h`、`wavefront_obj.h`、`camera_control.h`、`platform/win32_headers.h`。

## 容器别名

```cpp
template <class T> using vector = std::vector<T, allocator<T>>;
```

**普通别名的底层是标准容器，不接受自定义分配器参数。** 没有 EASTL。
`radray::allocator<T>` 在启用 `RADRAY_ENABLE_MIMALLOC` 时使用 `mi_stl_allocator<T>`，
关闭时使用 `std::allocator<T>`；容器与字符串别名统一使用该分配器。
`unique_ptr` / `shared_ptr` / `weak_ptr` / `make_unique` / `make_shared` /
`enable_shared_from_this` 也都被 `using` 拉进 `radray` 命名空间。

PMR 容器直接使用 `std::pmr::vector`、`std::pmr::map` 等标准名称，不在 `radray` 或 `types.h` 中另设别名；
运行时显式传入 memory_resource。
`memory.h/.cpp` 中的 `HeapMemoryResource` 将 PMR 上游连接到 RadRay 的 AlignedAlloc/AlignedFree，启用 mimalloc 时沿用该堆；
它把字节数向对齐要求取整，支持零字节请求，分配失败或大小溢出抛出 std::bad_alloc。资源按实例地址比较相等。
该适配器不缓存、不拥有其下游容器，也不改变进程默认 PMR resource；下游 pool 必须先于上游销毁。
PMR 容器普通复制构造选择默认资源，指定目标域时必须显式提供 allocator；赋值保留目标资源，
不同资源间移动可能逐元素移动，不能 swap 分配器不相等的容器。

`RADRAY_ENABLE_MIMALLOC`（默认 ON）在 Windows `/MD[d]` 下链接动态库 target `mimalloc`，
在 `/MT` 和其他平台下通过 whole-archive 链接 `mimalloc-static`。
标准分配的覆盖交给该依赖提供，core 不包含 `mimalloc-new-delete.h` 或自行定义全局 `new/delete`。
Windows 动态覆盖由最终程序负责部署 DLL 和安排加载；RadRay 不自动复制或强制加载 DLL。
具体配置、宿主约束与覆盖检查见[动态 CRT](../guide/build-test.md#windows-动态-crt-与-mimalloc)
和[静态 CRT](../guide/build-test.md#静态-crt-与-mimalloc)。启用时，容器与字符串的堆分配
通过 `mi_stl_allocator` 直接进入 mimalloc，不依赖 CRT 全局覆盖；开关会改变这些别名的类型，
库与使用者必须保持配置一致。

`memory.h` 的 `Malloc/Free`、`AlignedAlloc/AlignedFree` 在启用 mimalloc 时由 `memory.cpp`
直接调用 `mi_malloc/mi_free`、`mi_malloc_aligned/mi_free`，不依赖 CRT 符号覆盖；Windows `/MD[d]`
也使用这条路径。HeapMemoryResource 因此随该选项使用 mimalloc。关闭选项时保留标准 C 分配，
Windows 对齐分配使用 `_aligned_malloc/_aligned_free`，其他平台使用 `std::aligned_alloc/std::free`。
这些接口返回的内存必须通过对应 RadRay 释放接口配对释放，不能与调用方 CRT 的 free 混用。

普通 STL 容器按 `AGENTS.md` 使用这些别名；PMR 容器直接使用 `std::pmr` 名称并显式传入资源，
不增加 RadRay 容器别名，也不改变 `types.h` 的默认分配器。

### InlineVector

`inline_vector.h` 的 `InlineVector<T, N>` 用于通常只有少量元素的连续序列，`N` 必须大于零。
存储参考 [LLVM SmallVector](https://llvm.org/doxygen/SmallVector_8h_source.html)：对齐的原始内联空间加
data/size/capacity，只有 `[0, size)` 中的元素被构造。未溢出时不创建堆容器或调试元数据，
因此空容器及内联容量内的嵌套容器操作，在 Debug 迭代器检查开启时也不产生容器自身的堆分配；
元素类型自己的分配不受此保证约束。`T` 不要求默认构造或赋值。

溢出和 `reserve` 通过 `radray::allocator<T>` 获取满足元素对齐的存储。扩容使全部迭代器和引用失效；
未扩容的追加保留已有元素的引用。`pop_back` 立即析构末尾元素，`clear` 析构全部有效元素，二者均保留
现有容量，不在长度降到 `N` 时搬回内联区。这样重复清空、填充不会反复分配和搬迁。

复制只复制有效元素；可平凡复制且有平凡复制构造的类型，在同类型普通指针范围上批量复制。
移动堆存储时转移其所有权；移动内联存储时，可平凡复制且有平凡移动构造的类型批量搬移，其余逐元素构造。
源在成功移动后为空且可复用。输入迭代器、转换构造与非平凡类型保持逐元素路径。
移动操作的 `noexcept` 取决于 `T` 的移动构造。扩容插入先在新存储构造新增元素，再搬迁旧元素，
允许参数引用已有元素及其子对象；`assign(first, last)` 也接受自身的子范围和输入迭代器。
重定位优先使用不抛异常的移动，否则在可复制时复制。临时存储通过 RAII 清理；不可复制且移动可能失败的
元素不保证在失败后保留原值。超过 `max_size()` 的容量请求通过 `RADRAY_ABORT` 拒绝。

## SparseSet

`sparse_set.h` 的 `SparseSet<T>` 用稀疏槽位、稠密值数组和空闲链管理可复用句柄。
新槽 Generation 从 0 开始，Index 为 `std::numeric_limits<uint32_t>::max()` 表示无效句柄；Destroy 与 Clear 使被删除的
存活句柄代次递增。代次不检查溢出，调用方约定同一槽位的代次在程序生命周期内不回绕。
Get、Destroy 要求句柄存活，前置条件仅使用 Debug assert；可能失效的句柄使用 IsAlive 或 TryGet。
增删只做必要的局部断言，不运行全表扫描；完整行为一致性校验由 `test_sparse_set.cpp` 的混合操作测试覆盖。
删除用交换末项保持值数组稠密，可能移动其他值；句柄不因此改变，但值的地址与遍历顺序不稳定。
`GetHandleAt(denseIndex)` 将当前 `Values()` 的有效下标 O(1) 转为稳定句柄；稠密下标不能跨删除保存。

`Reserve(capacity)` 同时预留稀疏槽位、稠密值与稠密反向索引，不创建元素。
`ReserveGeometric(requiredCapacity)` 仅在容量不足时按 max(required, capacity + capacity / 2) 扩容，
避免流式小幅增长时反复精确 reserve；增长保持现有值与句柄有效，但可能移动值地址。

## BestFitAllocator

`allocator.h/.cpp` 的 BestFitAllocator 管理固定容量 `[0, capacity)` 的连续整数区间，与 GPU 和字节对齐无关。
Allocate(count) 返回 `optional<BestFitAllocator::Allocation>`，其 Offset / Count 表示起点和长度；零长度或无足够连续空间返回空值。
与同文件的分配器一致，释放接口为 Destroy(allocation)，并通过 `is_allocator<BestFitAllocator, BestFitAllocator::Allocation, uint32_t>` 编译期检查。
内部按 `(count, offset)` 的有序 set 选择最小可容纳空闲段，同尺寸时取较小偏移；按 offset 的 map 在 Destroy 时合并前驱、后继或两侧相邻空闲段。
GetCapacity / GetFreeCount 返回固定容量与当前总空闲数量；总空闲量足够不表示存在足够大的连续段。
Destroy 只接受本分配器返回的完整、仍存活分配，且只能释放一次；不维护所有活跃分配的额外表，Debug 检查边界和空闲段重叠。
分配器不可复制，调用者负责串行访问。Runtime 几何池使用它管理各页的顶点/索引元素范围。

## Nullable

三个特化：裸指针类、`unique_ptr`、`shared_ptr`。API：

```cpp
Nullable<Buffer*> buf = ...;
if (buf) { buf.Get()->Map(); }        // Get() 返回裸指针
auto owned = maybeOwned.Release();     // 移出所有权，不检查
auto* p = maybe.Unwrap();              // 为空则 throw NullableAccessException
```

**没有 `Value()`**（不同于 `std::optional`）。`Unwrap()` 抛异常是误用检测，不是错误处理路径。

与 `optional` 的分工、以及"裸指针即非空"的约定见 [cpp-conventions](../guide/cpp-conventions.md)。

## 协程

底层是 **stdexec**（`third_party/stdexec`）。`coroutine.h` 是薄封装，
而 `AGENTS.md` 要求**只用 `radray` 别名，不直接写 `exec::` / `stdexec::`**。

| 名字 | 是什么 |
|---|---|
| `task<T>` | `exec::task<T>` |
| `stop_source` / `stop_token` | `stdexec::inplace_stop_source` / `inplace_stop_token` |
| `when_all` / `when_all_with_variant` | 通过 `using` 引入同名 stdexec 组合接口，保留原有语义 |
| `when_any` | 通过 `using` 引入 `exec::when_any`，保留原有语义 |
| `TaskScope` | 包 `exec::async_scope`。`Spawn` / `RequestStop` / `Join` / `WaitUntilEmpty` / `GetStopToken`。**析构自动 RequestStop + 等空** |
| `ManualCoroutineRecord` | 手写 awaitable 的等待记录基类 |
| `ManualCoroutineScheduler<TRecord>` | 手动管理待恢复记录，stop 回调自动触发 `CancelRecord` |
| `CurrentStopToken()` | 协程体内取 stop token |
| `GetCoroutineStopToken(handle)` | **从 promise 的 env 取 stop token** |
| `AwaitWithStopToken(task, stop)` | 被取消时返回 `nullopt` |

**`GetCoroutineStopToken` 存在的理由**：手写 awaitable 的 `await_suspend` 只拿到
`coroutine_handle`，而 `coroutine_handle<>` 已经把 promise 的 env 擦除了，
于是取不到 stop token。这个函数探测 promise 里有没有 `get_stop_token`，
没有就返回一个 `stop_requested()` 恒为 false 的默认 token（= 不可取消）。

`ManualCoroutineScheduler` 的记录不能搬动——记录里存着回指调度器的指针（stop callback）。
调用方必须保证调度器及其活跃记录的地址稳定。

记录默认在 stop callback 中立即恢复。持有尚未完成的外部操作时，可以设置
`ResumeOnCancel=false`：stop callback 仍标记 `Canceled`，由记录拥有者在操作安全完成后恢复。
`CancelAll` 是最终清理，仍会强制恢复全部记录；调用前必须先结束这些外部操作。

记录的 `Sequence` 由所属 ManualCoroutineScheduler 分配，在该实例内单调递增且不回绕。
`DispatchReady` 先按无副作用的谓词冻结就绪记录的地址与序号，再恢复续体；回调取消其他记录或
新建记录时，旧地址不会误派发给新的等待。恢复后仍存活的原记录由调度器摘除。
跨多个等待表派发时，先分别捕获 `GetSequenceBoundary()` 的排他截止，再逐表派发。

`TaskScope` 不可拷贝不可移动，且析构会阻塞。它必须在任务依赖的服务之前析构，
并保证完成与取消所需的执行环境仍存活。

## 日志与断言

底层是 spdlog。

`logger.h` 的 `radray::format` 提供窄字符与宽字符重载，分别返回 `radray::string` 和
`radray::wstring`。它通过 `fmt::format_to` 直接写入结果字符串，结果存储使用
`radray::allocator`，不创建中间 `std::string`。格式字面量保留 fmt 的编译期检查，
运行时格式串使用 `fmt::runtime`；已有 fmt formatter 可继续使用。函数不声明 `noexcept`，
格式化与分配错误沿用 fmt 和分配器的异常行为。花括号风格日志也使用此封装。

| 宏 | 语义 |
|---|---|
| `RADRAY_DEBUG_LOG` | **仅 Debug**，Release 下展开为空 |
| `RADRAY_INFO_LOG` / `RADRAY_WARN_LOG` | 常驻 |
| `RADRAY_ERR_LOG` | 常驻，**自带 `source_location`** |
| `RADRAY_ABORT` | Critical 日志 + `std::abort()`，**Release 也保留** |
| `RADRAY_ASSERT(x)` | `assert(x)`，**Release 下消失** |
| `RADRAY_THROW(type, fmt, ...)` | format 后 throw（新代码别用，见硬规则） |

**关键区别**：`RADRAY_ASSERT` 在 Release 被编译掉，所以不要把必须执行的校验写在里面。
需要常驻的不变量检查用 `RADRAY_ABORT` 或返回错误。

`*_CSTYLE` 变体走 printf 风格。Debug 检测一律用 `RADRAY_IS_DEBUG`，不用 `NDEBUG` / `_DEBUG`。

## 性能采样宏

`profiler.h` 是全仓库唯一的插桩入口，后端是 Tracy（`third_party/tracy`）；业务代码不 include `tracy/*`，
只有 render 后端为 GPU 时间戳直接使用 `TracyD3D12.hpp` / `TracyVulkan.hpp`。
`RADRAY_ENABLE_PROFILER` 未定义时所有宏展开为空。开关、viewer 与采样步骤见
[性能测量](../guide/performance.md#性能采样tracy)。

| 宏 | 语义 |
|---|---|
| `RADRAY_PROFILE_SCOPE()` | 以所在函数名命名的作用域 zone |
| `RADRAY_PROFILE_SCOPE_N("name")` | 字符串字面量命名；同一花括号作用域内只能出现一个 `SCOPE*` 宏，需要嵌套时用 `{}` 分隔 |
| `RADRAY_PROFILE_SCOPE_DYN(sv)` | 运行时 `string_view` 作为 **zone 名**（Tracy transient，导出与 GUI 都用该名字，而不是函数名）；与 `SCOPE*` 同样受单作用域限制 |
| `RADRAY_PROFILE_FRAME()` | 标记调用线程的一帧结束；由调用方显式插入，宏不调度帧循环或线程 |
| `RADRAY_PROFILE_PLOT("name", v)` | 数值曲线 |
| `RADRAY_PROFILE_THREAD("name")` | 命名当前线程 |
| `RADRAY_PROFILE_MESSAGE(sv)` | 时间线文本消息 |

zone 的粒度停在帧阶段、pass 与 per-draw 批次，不放进逐元素循环；开启但未连接 viewer 时每个 zone 只剩一次连接状态判断。

## 枚举

```cpp
namespace radray {
template <> struct is_flags<MyFlags> : std::true_type {};
// 成员本身是多 bit 组合时还要：
template <> struct is_compound_enum_flags<MyFlags> : std::true_type {};
}
// 复合标志要求 ADL format_as
std::string_view format_as(MyFlags v) noexcept;
```

`EnumFlags<T>` 提供 `| & ^ ~` 与复合赋值、`HasFlag`、`value()`、`operator T`、
`FormatByName()` / `FormatAsBits()`。

**`enum_flags.h` 是 `magic_enum` 的唯一出口**，别处不要直接 include magic_enum。
封装函数：`EnumName` / `EnumNameOr` / `EnumFlagBitName` / `EnumFlagsName` / `EnumContains` /
`EnumCast` / `EnumFlagsCast`。`EnumNameOr` 是各模块 `format_as` 的主力。

**坑**：`EnumName` / `EnumCast` 的反射范围默认是 `[-128, 127]`。位标志枚举的值会超出，
要用 `EnumFlagBitName` / `EnumFlagsCast`。

**永不重命名枚举成员**（`AGENTS.md`）：名字被 magic_enum 与序列化数据消费。

## 哈希

```cpp
HashCode h;
h.Add(key.Count);
h.Add(key.Name);      // 任何 std::hash 可哈希的类型
return h.ToHashCode();
```

`HashCode` 是 constexpr 可用的增量哈希（xxHash 风格），另有 `HashCode::Combine`。
`HashData` / `HashData64` 走 xxHash 处理字节流。

`StringHash` / `StringEqual` 支持异质查找，让 `unordered_map<string, V, StringHash, StringEqual>`
能直接用 `string_view` 查。

`PodHasher<T>` / `PodEqual<T>` 是逐字节版，只对 trivially copyable 生效。
调用方必须保证整个对象表示稳定，包含 padding；有成员值相等但表示不同的可能时，使用逐字段哈希与比较。

### 写一个自定义 map key

`unordered_map` 的契约是 **`equal => same hash`**。违反它的表现是"明明存进去了却查不到"，
且不会报错。三条规则：

1. **逐字段 `Add`，不要 `memcmp` 整个对象。** key 或其成员可能含 padding，也可能有多种表示对应同一个值；
   两个 `operator==` 相等的对象因而未必具有相同的字节表示。
   不能仅凭使用 `optional` 推断整个 key 是否 trivially copyable 或是否适合逐字节比较。

   对没有成员 hasher 的可选描述符，**先喂"有没有值"，有值再逐字段喂内容**：

   ```cpp
   hash.Add(entry.ImmutableSampler.has_value() ? 1u : 0u);
   if (entry.ImmutableSampler.has_value()) {
       AddSampler(hash, entry.ImmutableSampler.value());   // 内部同样逐字段
   }
   ```

   嵌套的 `optional` 成员逐层照此展开。`vector` 同理：先喂 `size()`，再逐元素喂。
2. **相等比较逐字段**（`= default` 的 `operator==` 就够），并确认它和哈希看的是同一批字段。
   刻意排除某个字段就要在哈希里同样排除。
3. **缓存散列值的 key 在移动后也须保持内容与散列一致。** 默认移动成员可能保留旧散列而改变源内容；
   若移后对象仍可参与比较或查表，就显式恢复它的有效状态，不假定所有容器移后必为空。

RHI 布局直接拥有原生对象，不维护布局 key 或去重缓存，见 [RHI 布局所有权](render-rhi.md#原生布局直接所有权与显式共享)。
附件顺序有渲染语义，`AttachmentSignature` 与 `RenderPassExecutionKeyVulkan` 不得为了复用而重排 color slots；
允许忽略的字段见[直接附件与 Vulkan 原生缓存](render-rhi.md#直接附件与-vulkan-原生缓存)。

## JSON

底层 yyjson，但头文件不暴露第三方类型。定制点：

```cpp
template <> struct JsonSerializer<T> {
    static bool Write(JsonWriteContext& context, const T& value) noexcept;
};
template <> struct JsonDeserializer<T> {
    static bool Read(const JsonValue& value, T& output) noexcept;
};
```

解析文档、可变文档及写出缓冲区都显式传入 `yyjson_alc`。`json.cpp` 内的回调在
启用 `RADRAY_ENABLE_MIMALLOC` 时直接配对调用 `mi_malloc/mi_realloc/mi_free`，
关闭时使用标准 `malloc/realloc/free`；写出缓冲区复用同一个释放回调，避免与
RadRay 通用内存接口的后续实现变化产生不一致。该路径不依赖 CRT 全局分配覆盖，
也不改变直接调用 yyjson 的外部代码所用的分配器。

失败返回 false 且不修改 output。读写上下文：`JsonWriteContext` / `JsonObjectWriter` /
`JsonArrayWriter`、`JsonObjectReader` / `JsonArrayReader`。便捷助手 `JsonMember` +
`SerializeJsonObject` / `DeserializeJsonObject`（成员指针描述）。

内置特化覆盖算术类型（带范围检查）、string、enum（**按成员名**）、`EnumFlags`（**字符串数组**）、
`optional`、`vector`、`array`、`span`。

**坑**：`std::string_view` 刻意不可反序列化（数据随 `JsonDocument` 销毁）。
序列化器拒绝 NaN / Infinity。`JsonValue` 的生命周期依附于 `JsonDocument`。

## 数学

`basic_math.h` 是 **Eigen3 的薄封装**，类型直接用 `Eigen::Vector<T,N>` / `Eigen::Matrix<T,R,C>` /
`Eigen::Quaternion<T>`。

**约定：左手系，深度映射到 `[0,1]`（D3D 风格），矩阵列主序（Eigen 默认）。**
函数名带 `LH` 后缀：`PerspectiveLH`、`OrthoLH`、`LookAtLH`、`LookAtFrontLH`。

这套约定跟 D3D12 对齐。Vulkan 的 NDC Y 轴朝下且 RHI 不替你翻，处理它是调用方的事，
见 [render-rhi](render-rhi.md) 的「视口」一节。

辅助：`Align`、`Degree` / `Radian`、`Lerp`、`Clamp`、`AbsDot`、`ComposeTransform` /
`DecomposeTransform`。Eigen 的向量/矩阵/四元数都有 fmt formatter，
可以直接 `radray::format("{}", mat)`。

`viewport.h` 提供 `Viewport` 和 `Rect`。它们是给 RHI 的 POD，不包含 Eigen，因此 `rhi.h` 只需包含 `viewport.h`，无需包含 `basic_math.h`。
`basic_math.h` 会包含 `viewport.h`，已有数学调用方仍能看见这两个类型。

`AffineTransform` 是 48 B 列主序 affine 3×4 传输表示，默认隐含齐次行 `(0, 0, 0, 1)`；
它与 `Eigen::Matrix4f` 相互转换并保留 shear/负缩放，计算和缓存仍使用 `Matrix4f`。

`Aabb` 是三维 float 轴对齐包围盒，使用两个 `array<float, 3>` 成员 `Min` / `Max`，共 24 B。
构造函数接收最小、最大端点，方法的向量参数和返回值使用 `Eigen::Vector3f`。
默认构造为空盒，适合从顶点逐步累积，不会额外包含原点。端点非有限或任一轴 `Min > Max` 均视为空盒；
点、线和面的退化包围盒仍有效。端点成员公开，修改后沿用同一规则。

| 操作 | 契约 |
|---|---|
| `IsEmpty` | 检查上述空盒条件 |
| `GetCenter` / `GetSize` / `GetExtent` | 中心、全尺寸、半尺寸；空盒返回零向量 |
| `Expand(point)` / `Expand(bounds)` | 原地扩大到包含输入；空盒采用首个有效输入，忽略非有限点和空盒 |
| `Contains(point)` / `Contains(bounds)` | 包含边界；空盒不包含任何对象，也不被判为包含于其他盒 |
| `Intersects` / `Intersection` | 边界接触算相交；求交返回独立 AABB，不相交时返回空盒 |
| `Transform(AffineTransform)` | 返回变换后重新包围的 AABB，支持旋转、负/非均匀/零缩放和 shear；空盒仍为空盒 |

`Transform` 的输入必须是有限仿射变换，不接收透视投影。中心和半尺寸避免中间加减溢出，
其他算术结果仍受 float 范围和精度限制。AABB 不带坐标空间标记，调用方负责区分本地与世界空间。
CPU 测试位于 `modules/core/tests/test_basic_math.cpp` 的 `BasicMathTest`，覆盖空盒、退化边界、
非有限输入、累积合并，以及与八角点参考计算对比的仿射变换。

## 其他

- **`scope_guard.h`** — `ScopeGuard` / `MakeScopeGuard`，`Dismiss()` 取消。移动后源自动 dismiss。
- **`allocator.h`** — `BuddyAllocator`、`FirstFitAllocator` 与 [BestFitAllocator](#bestfitallocator) 都是偏移区间子分配器，
  不拥有被划分的资源，可用于描述符、GPU 内存或几何元素范围；实际堆内存分配由 `memory.h` 提供。
- **`runtime_type.h`** — 独立、稳定的 GUID 声明设施。`RuntimeTypeId` 是 `Guid` 的别名；
  需要稳定标识的类型特化 `RuntimeTypeTrait<T>::value`，显式读取 `runtime_type_id_v<T>` 时会先按
  `remove_cvref_t` 归一化并要求 GUID 非空。它不描述继承、不保存对象描述符，也不建立 GUID 与
  RTTI 的全局映射。对象关系查询使用指针形式 `dynamic_cast`，精确动态类型使用 `typeid`，
  仅为这些查询不需要声明 GUID。
- **RTTI 是工程编译约定** — `radray_default_compile_flags` 为每个自有 target 以 `PRIVATE`
  方式设置 MSVC/ClangCL 的 `/GR` 或其他前端的 `-frtti`，仅作用于 C++ 编译。新增库、测试、
  工具、样例与 benchmark 都必须显式调用该函数。core 不通过链接接口
  传播 RTTI 选项，仓库外使用公共对象查询的 consumer 需自行开启 RTTI。把开关放在现有的
  工程编译函数中，可以统一自有目标而不改变第三方配置，也不把工程策略挂在 core 的公共接口上。
- **`intrusive_ptr.h`** — `IntrusivePtr<T>` 通过 `IntrusivePtrTraits<T>` 调用对象的 `Retain()` / `Release()`；
  traits 可特化以适配其他引用协议，或将不完整类型的引用操作移到其定义可见的位置。
  `RefCountedBase<T>` 与 `ThreadSafeRefCountedBase<T>` 初始计数均为 0，对象本身不可复制、移动，
  通过虚析构和归零时的 `delete static_cast<const T*>(this)` 完成销毁；溢出、下溢及带引用析构均常驻检查。
  调用方或工厂负责分配和构造，显式 `IntrusivePtr<T>{pointer}` 取得首个或新增强引用；
  构造参数使用 `Nullable<T*>` 表达可空输入。复制增引用，移动（包括派生到基类转换）只转移引用。
  `Reset()` 先清空自身，再释放旧引用；没有 ADL 钩子、adopt 接口或裸指针所有权释放接口。
  非原子基类要求串行操作；原子基类使用 relaxed 增引用、acq_rel 减引用，`UseCount()` 仅提供计数快照，
  不同步对象数据或同一个智能指针变量。跨线程传递引用仍须同步，裸指针只有在存活保证下才能取得强引用。
  该组织参考 [LLVM IntrusiveRefCntPtr](https://llvm.org/doxygen/IntrusiveRefCntPtr_8h_source.html)，不依赖 LLVM；
  上传目标的强引用交接与帧槽保活见 [Runtime GPU 资源上传](runtime.md#gpu-资源与批量上传)。
- **`structured_buffer.h`** — CPU 侧反射式结构化缓冲。新代码用 `TrySetValue`（fail-fast），
  `SetValue` 是旧接口。
- **`text_encoding.h`** — Windows 使用 `MultiByteToWideChar` / `WideCharToMultiByte` 的 `CP_UTF8` 模式转换。
  其他平台使用 `mbsrtowcs` / `wcstombs`，编码由当前 C locale 决定，不保证 UTF-8。
- **`image_data.h`** — `ImageData` + PNG 读写 / JPEG 读取，底层 **libpng / IJG libjpeg**（不是 stb），
  由 `RADRAY_ENABLE_LIBPNG` / `RADRAY_ENABLE_LIBJPEG` 门控。另有
  `CompareImageRGBA8` / `ImageDiffRGBA8` 供测试对比。
  PNG 的跳转边界位于私有 `modules/core/src/png_codec.h` / `png_codec.c`，以 C11 编译并通过 C ABI 调用。
  读取头部、读取像素与写入分别建立仍在调用栈中的 `setjmp`；codec 及回调只依赖 C 接口。
  错误与首条警告复制到调用方 context 的固定缓冲，辅助函数普通返回后才记录日志。
  `LoadPNG` 从流当前位置缓冲全部剩余压缩数据，支持不可 seek 的流；回调只作边界检查和字节复制。
  输入缓冲和解码像素由外层 C++ 持有，流缓冲与分配异常正常传播，非法 PNG 返回空值。
  C context 直接返回尺寸、通道数、位深和已检查的字节数，`ImageFormat` 只在外层映射。像素阶段借用连续
  存储的首地址，由 codec 按行计算地址、处理翻转和 Adam7 各轮，不构造或传递行指针数组。
  codec 按 `RADRAY_ENABLE_MIMALLOC` 直接调用 mimalloc C API 或配对的 `malloc/free`，与 core 的分配选择一致。
  `WritePNG` 支持 RGB8/RGBA8，外层持有 C 文件句柄，写回调只调用 C stdio；编码、写入和关闭
  失败返回 `false`。两条路径在成功或失败后都先销毁 libpng 状态，再释放其借用的外层资源；保留 `/EHsc`。
  JPEG 的读取边界位于私有 `modules/core/src/jpeg_codec.h` / `jpeg_codec.c`，同样以 C11 编译并通过 C ABI 调用。
  不透明 decoder 持有 libjpeg 对象、错误管理器与跳转缓冲，头部和像素阶段分别建立当前调用中的 `setjmp`。
  致命错误在 C 内跳转并返回失败，错误与首条警告保存在调用方 context；外层 C++ 记录日志并负责销毁。
  `LoadJPEG` 从流当前位置缓冲全部剩余压缩数据：可 seek 时按已知长度一次分配并直接读取，正常返回时恢复原位置；
  不可 seek 时与 PNG 共用分块缓冲逻辑。读取不改流状态，支持开启异常的流。输入与最终像素由 C++ 持有，
  流缓冲与分配异常正常传播，致命 JPEG 错误返回空值；可恢复警告
  沿用 libjpeg 的继续解码行为，例如缺少末尾 EOI。输出为 RGB8，灰度输入扩展为 RGB；可选 alpha 饱和到 255。
  RGB 行直接解码到最终缓冲，RGBA 行先写 RGB 再从后向前原地补 alpha；翻转仅改变目标行地址，不分配临时扫描行。
  decoder 自身按 `RADRAY_ENABLE_MIMALLOC` 使用 `mi_zalloc/mi_free` 或 `calloc/free`，libjpeg 内部内存仍由其内存管理器配对释放。
- **`binary_io.h`** — 固定小端。reader 越界返回 false 且不消费输入。
- **`channel.h`** — `BoundedChannel` / `UnboundedChannel`，`Complete()` 后写入失败；已有消息仍可读完，排空后读取失败。
- **`sparse_set.h`** — 带世代编号的 handle 容器。
- **`guid.h`** — `NewGuid` / `Parse` / `ToString`，有 `format_as` 与 `std::hash` 特化。

## 测试

测试源文件在 `modules/core/tests/`，按被测头文件命名。除 `test_inline_vector.cpp` 外都链进 `test_radray_core`；
该文件替换全局 `operator new`，单独成 exe。`ctest -R` 使用套件名，编写约定见[编写测试](../guide/testing.md)。

| 文件 | 套件名 |
|---|---|
| `test_allocator.cpp` | `BuddyAllocatorTest`、`FirstFitAllocatorTest`、`BestFitAllocator` |
| `test_basic_math.cpp` | `BasicMathTest` |
| `test_binary_io.cpp` | `BinaryIoTest` |
| `test_channel.cpp` | `ChannelTest` |
| `test_coroutine.cpp` | `ManualCoroutineSchedulerTest`、`TaskScopeTest`、`AwaitWithStopTokenTest`、`GetCoroutineStopTokenTest` |
| `test_enum_flags.cpp` | `EnumFlagsTest` |
| `test_file.cpp` | `FileTest` |
| `test_guid.cpp` | `GuidTest` |
| `test_hash.cpp` | `HashTest` |
| `test_image_data.cpp` | `ImageDataTest` |
| `test_img_rw.cpp` | `PngTest`、`PngTest/PngRoundTripTest`（仅 `RADRAY_ENABLE_LIBPNG`） |
| `test_jpeg.cpp` | `JpegTest`、`JpegTest/JpegAlphaTest`（仅 `RADRAY_ENABLE_LIBJPEG`） |
| `test_inline_vector.cpp` | `InlineVectorTest`（独立 exe `test_inline_vector`） |
| `test_intrusive_ptr.cpp` | `IntrusivePtrTest`、`IntrusivePtrDeathTest` |
| `test_json.cpp` | `JsonTest`、`JsonSerializerTest`、`JsonDeserializerTest` |
| `test_logger.cpp` | `FormatTest` |
| `test_memory.cpp` | `MemoryTest` |
| `test_memory_override.cpp` | `MimallocOverrideTest`（仅 Windows 静态 CRT 且启用 mimalloc） |
| `test_nullable.cpp` | `NullableTest` |
| `test_runtime_type.cpp` | `RuntimeTypeIdTest` |
| `test_scope_guard.cpp` | `ScopeGuardTest` |
| `test_sparse_set.cpp` | `SparseSetTest` |
| `test_structured_buffer.cpp` | `StructuredBufferTest` |
| `test_text_encoding.cpp` | `TextEncodingTest` |
| `test_utility.cpp` | `UtilityTest` |
| `test_wavefront_obj.cpp` | `WavefrontObjTest` |
| `test_xml.cpp` | `XmlTest` |
