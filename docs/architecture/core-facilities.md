> - 适用: 需要某个基础设施但不确定仓库里已有什么；踩到 core 的坑
> - 权威: 本文是 `radraycore` 提供什么、以及怎么正确用它的唯一说明
> - 锚点: `modules/core/include/radray/types.h`, `modules/core/include/radray/nullable.h`, `modules/core/include/radray/coroutine.h`, `modules/core/include/radray/enum_flags.h`

# core 基础设施

`radraycore` 不涉及 GPU。写新代码前先看这里有没有现成的东西。

## 常用头文件

几乎每个 `.cpp` 都会碰到的：

| 头文件 | 提供 |
|---|---|
| `types.h` | 全部容器与字符串别名的唯一出口 |
| `logger.h` | 日志宏、`RADRAY_ABORT`、`RADRAY_ASSERT` |
| `nullable.h` | `Nullable<T>` |
| `enum_flags.h` | `EnumFlags<T>` + `magic_enum` 的唯一出口 |
| `basic_math.h` | Eigen 封装 + 视图/投影辅助 |
| `hash.h` | `HashCode` 增量哈希、`StringHash`、`PodHasher` |
| `coroutine.h` | `task`、`TaskScope`、`ManualCoroutineScheduler` |
| `utility.h` | `StaticCastUniquePtr`、`Unreachable`、`RADRAY_UNUSED` |
| `scope_guard.h` | `ScopeGuard` / `MakeScopeGuard` |

专用的：`json.h`（yyjson）、`lmdb.h`（LMDB）、`binary_io.h`（小端读写）、`file.h`、`environment.h`、
`dynamic_library.h`、`guid.h`、`stopwatch.h`、`text_encoding.h`、`runtime_type.h`、
`allocator.h`（GPU 子分配器，与堆无关）、`memory.h`、`sparse_set.h`、`channel.h`、
`intrusive_ptr.h`、`structured_buffer.h`、`image_data.h`、`vertex_data.h`、
`triangle_mesh.h`、`wavefront_obj.h`、`camera_control.h`、`platform/win32_headers.h`。

## 容器别名

```cpp
template <class T> using vector = std::vector<T, allocator<T>>;
```

**底层就是 std + `std::allocator`。** 没有 EASTL，没有自定义分配器模板。
`unique_ptr` / `shared_ptr` / `weak_ptr` / `make_unique` / `make_shared` /
`enable_shared_from_this` 也都被 `using` 拉进 `radray` 命名空间。

**mimalloc 是链接期覆盖，代码里看不见。** `RADRAY_ENABLE_MIMALLOC`（默认 ON）让
`mimalloc-override` 以 whole-archive 方式链进来，全局替换 `malloc` / `new`。
所以"用了 radray 别名"和"走 mimalloc"是两件独立的事——别名的价值在于统一，不在于分配器。

`AGENTS.md` 要求 STL 容器一律走这些别名。

## Nullable

三个特化：裸指针类、`unique_ptr`、`shared_ptr`。API：

```cpp
Nullable<Buffer*> buf = ...;
if (buf) { buf.Get()->Map(); }        // Get() 返回裸指针
auto owned = maybeOwned.Release();     // 移出所有权，不检查
auto* p = maybe.Unwrap();              // 为空则 throw NullableAccessException
```

**没有 `Value()`**（不同于 `std::optional`）。`Unwrap()` 抛异常是误用检测，不是错误处理路径。

与 `optional` 的分工、以及"裸指针即非空"的约定见 `guide/cpp-conventions.md`。

## 协程

底层是 **stdexec**（`third_party/stdexec`）。`coroutine.h` 是薄封装，
而 `AGENTS.md` 要求**只用 `radray` 别名，不直接写 `exec::` / `stdexec::`**。

| 名字 | 是什么 |
|---|---|
| `task<T>` | `exec::task<T>` |
| `stop_source` / `stop_token` | `stdexec::inplace_stop_source` / `inplace_stop_token` |
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
`AssetWaitAwaitable::await_suspend` 为此把自己模板化，见 `architecture/asset-system.md`。

`ManualCoroutineScheduler` 的记录不能搬动——记录里存着回指调度器的指针（stop callback）。
`GpuSystem::_flights` 因此是 `vector<unique_ptr<FlightSlot>>` 而非 `vector<FlightSlot>`。

`TaskScope` 不可拷贝不可移动，且析构会阻塞。它必须在它所依赖的系统（例如 `GpuSystem`）
之前析构，否则取消时的析构会碰到已死的 device。

## 日志与断言

底层是 spdlog。

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

`PodHasher<T>` / `PodEqual<T>` 是逐字节版，只对 trivially copyable 生效，
且**要求 key 值初始化（`PodKey{}`）清零 padding**。

### 写一个自定义 map key

`unordered_map` 的契约是 **`equal => same hash`**。违反它的表现是"明明存进去了却查不到"，
且不会报错。三条规则：

1. **逐字段 `Add`，不要 `memcmp` 整个对象。** 带 `std::optional` 或任何非平凡成员的类型
   有填充字节，两个 `operator==` 相等的对象可能 padding 不同、散列不等。
   这类 key 不是 trivially copyable，所以也用不了 `PodHasher`。

   `std::optional` 本身不可哈希，喂法是**先喂"有没有值"，有值再喂内容**：

   ```cpp
   hash.Add(entry.ImmutableSampler.has_value() ? 1u : 0u);
   if (entry.ImmutableSampler.has_value()) {
       AddSampler(hash, entry.ImmutableSampler.value());   // 内部同样逐字段
   }
   ```

   嵌套的 `optional` 成员逐层照此展开。`vector` 同理：先喂 `size()`，再逐元素喂。
2. **相等比较逐字段**（`= default` 的 `operator==` 就够），并确认它和哈希看的是同一批字段。
   刻意排除某个字段就要在哈希里同样排除。
3. **在 key 内缓存 `_hash` 时，移动构造与移动赋值必须自己写。** 默认移动会留下
   "空容器 + 原 `_hash`"的源，那个源与一个真正的空 key 内容相等却散列不等，直接违反契约。
   移动后把源的散列值重算（此时源已空，是常数开销）。

`PipelineLayoutKey` 是这三条都踩过的实例，见 `architecture/asset-system.md`。
另一个方向的例子是 `RenderPassCacheKey`：它**刻意不归一化**，因为字段顺序携带语义，
见 `architecture/render-rhi.md`。

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
见 `architecture/render-rhi.md` 的「视口」一节。

辅助：`Align`、`Degree` / `Radian`、`Lerp`、`Clamp`、`AbsDot`、`ComposeTransform` /
`DecomposeTransform`。另有 `Viewport`、`Rect`。Eigen 的向量/矩阵/四元数都有 fmt formatter，
可以直接 `fmt::format("{}", mat)`。

## 其他

- **`scope_guard.h`** — `ScopeGuard` / `MakeScopeGuard`，`Dismiss()` 取消。移动后源自动 dismiss。
- **`lmdb.h`** — LMDB（`third_party/lmdb`，OLDAP-2.8 许可）的薄封装，与 `json.h` /
  `xml.h` 同模式（公开头只前置声明 `MDB_*`，PRIVATE 链接）。`LmdbEnvironment` /
  `LmdbTransaction` / `LmdbCursor` 暴露 byte[] → byte[] 的 Get/Put/Delete/遍历，
  错误收敛为 `LmdbResult`（Ok / NotFound / Failure）。**单写者、单线程**，读事务须在
  写事务 begin 前结束。asset 元数据的运行时存储用它（ADR-0038）。
- **`allocator.h`** — `BuddyAllocator` / `FirstFitAllocator`，都是**偏移量式子分配器**
  （返回 `Allocation{Offset, ...}`），给描述符堆和 GPU 内存用。**不是堆分配器**，那是 `memory.h`。
- **`runtime_type.h`** — 无 RTTI 的类型标识。特化 `RuntimeTypeTrait<T>` 给一个 Guid，
  用 `Bases = std::tuple<...>` 声明继承，`runtime_is_a_v<D, B>` 编译期沿 Bases 图判断。
  **运行期只有 Guid，算不出多继承的基类子对象偏移**——那必须由持有确切静态类型的上下文
  用 `static_cast` 修正（`ServiceRegistry::RegisterBaseAlias` 就是这么做的）。
- **`intrusive_ptr.h`** — `IntrusivePtr` + `AdoptRef` / `RetainRef`，依赖 ADL 的
  `IntrusivePtrAddRef` / `IntrusivePtrRelease`。
- **`structured_buffer.h`** — CPU 侧反射式结构化缓冲。新代码用 `TrySetValue`（fail-fast），
  `SetValue` 是旧接口。
- **`text_encoding.h`** — UTF-8 ⇄ wchar。Windows 走 `MultiByteToWideChar`，
  其他平台走 `mbsrtowcs`（依赖 locale，行为可能不一致，有 TODO）。
- **`image_data.h`** — `ImageData` + PNG/JPEG 读写，底层 **libpng / libjpeg**（不是 stb），
  由 `RADRAY_ENABLE_LIBPNG` / `RADRAY_ENABLE_LIBJPEG` 门控。另有
  `CompareImageRGBA8` / `ImageDiffRGBA8` 供测试对比。
- **`binary_io.h`** — 固定小端。reader 越界返回 false 且不消费输入。
- **`channel.h`** — `BoundedChannel` / `UnboundedChannel`，`Complete()` 后读写都失败。
- **`sparse_set.h`** — 带世代编号的 handle 容器。
- **`guid.h`** — `NewGuid` / `Parse` / `ToString`，有 `format_as` 与 `std::hash` 特化。

## 测试

| 文件 | 套件名 |
|---|---|
| `test_buddy_alloc.cpp` | `BuddyAllocatorTest` |
| `test_first_fit_alloc.cpp` | `Core_Allocator_FirstFit` |
| `test_wavefront_obj.cpp` | `Core_WaveObjTest` |
| `test_str_convert.cpp` | `Core_Utility` |
| `test_img_rw.cpp` | `PNG`（仅 `RADRAY_ENABLE_LIBPNG`，需 `RADRAY_ASSETS_DIR`） |
| `test_nullable.cpp` | `NullableTest` |
| `test_intrusive_ptr.cpp` | `IntrusivePtr` |
| `test_enum_flags.cpp` | `EnumFlagsTest` |
| `test_sparse_set.cpp` | `SparseSetTest` |
| `test_structured_buffer.cpp` | `StructuredBufferTest` |
| `test_pod_hash.cpp` | `PodHashTest`, `HashCodeTest` |
| `test_runtime_type.cpp` | `RuntimeTypeIsA` |
| `test_binary_io.cpp` | `BinaryIoTest` |
| `test_json.cpp` | `JsonTest` |
| `test_json_serializer.cpp` | `JsonSerializerTest` |
| `test_json_deserializer.cpp` | `JsonDeserializerTest` |
| `test_lmdb.cpp` | `LmdbTest` |
