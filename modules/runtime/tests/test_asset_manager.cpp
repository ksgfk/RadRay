#include <gtest/gtest.h>

#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>

#include <radray/runtime/asset_manager.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_resource_recycler.h>

using namespace radray;

namespace {

// ── CPU-only fake asset:loader 同帧内联完成,无 GPU 上传 ──
class CpuAsset : public Asset {
public:
    explicit CpuAsset(int value) noexcept : _value(value) {}
    void OnUnload(IRenderResourceRecycler& recycler) override;
    AssetTypeId GetTypeId() const noexcept override;
    int Value() const noexcept { return _value; }

private:
    int _value;
};

// ── 带 GPU 上传阶段的 fake asset:loader co_await 一次上传,跨帧 ──
class GpuAsset : public Asset {
public:
    explicit GpuAsset(int value) noexcept : _value(value) {}
    void OnUnload(IRenderResourceRecycler& recycler) override { (void)recycler; }
    AssetTypeId GetTypeId() const noexcept override;
    int Value() const noexcept { return _value; }

private:
    int _value;
};

class FakeRenderResource final : public render::RenderBase {
public:
    explicit FakeRenderResource(int* destroyCount) noexcept : _destroyCount(destroyCount) {}
    ~FakeRenderResource() noexcept override {
        if (_destroyCount != nullptr) {
            ++*_destroyCount;
        }
    }

    render::RenderObjectTags GetTag() const noexcept override { return render::RenderObjectTag::UNKNOWN; }
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}

private:
    int* _destroyCount;
};

class RecyclableAsset : public Asset {
public:
    explicit RecyclableAsset(int* destroyCount) noexcept
        : _resource(make_unique<FakeRenderResource>(destroyCount)) {}

    void OnUnload(IRenderResourceRecycler& recycler) override {
        recycler.RecycleRenderResource(std::move(_resource));
    }

    AssetTypeId GetTypeId() const noexcept override;

private:
    unique_ptr<render::RenderBase> _resource;
};

}  // namespace

namespace radray {

template <>
struct RuntimeTypeTrait<CpuAsset> {
    static constexpr RuntimeTypeId value{0x11111111, 0x2222, 0x3333, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb};
    using Bases = std::tuple<Asset>;
};

template <>
struct RuntimeTypeTrait<GpuAsset> {
    static constexpr RuntimeTypeId value{0xcccccccc, 0xdddd, 0xeeee, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    using Bases = std::tuple<Asset>;
};

template <>
struct RuntimeTypeTrait<RecyclableAsset> {
    static constexpr RuntimeTypeId value{0x12345678, 0x9abc, 0x4def, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
    using Bases = std::tuple<Asset>;
};

}  // namespace radray

namespace {

void CpuAsset::OnUnload(IRenderResourceRecycler& recycler) { (void)recycler; }
AssetTypeId CpuAsset::GetTypeId() const noexcept { return runtime_type_id_v<CpuAsset>; }
AssetTypeId GpuAsset::GetTypeId() const noexcept { return runtime_type_id_v<GpuAsset>; }
AssetTypeId RecyclableAsset::GetTypeId() const noexcept { return runtime_type_id_v<RecyclableAsset>; }

AssetId MakeId(uint32_t a) {
    return AssetId{a, 0x0000, 0x4000, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
}

AssetLoadTask LoadCpuAsset(int value) {
    if (value < 0) {
        co_return AssetLoadResult::Failure("negative value");
    }
    co_return AssetLoadResult::Success(make_unique<CpuAsset>(value));
}

AssetLoadTask LoadThrowingAsset() {
    throw std::runtime_error("boom");
    co_return AssetLoadResult::Success(make_unique<CpuAsset>(0));
}

AssetLoadTask LoadGpuAsset(FrameUploadScheduler& frameUploads, int value) {
    // 两阶段“上传”:BeginUpload 拿帧上下文(本测试不解引用 cmd/uploader),再 WaitGpu 跨帧。
    FrameUploadScope frame = co_await frameUploads.BeginUpload();
    (void)frame;
    co_await frame.WaitGpu();
    co_return AssetLoadResult::Success(make_unique<GpuAsset>(value));
}

struct WaitProbe {
    bool Completed{false};
    bool Ready{false};
    bool Faulted{false};
    bool Canceled{false};
};

task<void> WaitForCpuAsset(AssetManager& mgr, StreamingAssetRef<CpuAsset> ref, WaitProbe& probe) {
    co_await mgr.Wait(ref);
    probe.Completed = ref.IsCompleted();
    probe.Ready = ref.IsReady();
    probe.Faulted = ref.IsFaulted();
    probe.Canceled = ref.IsCanceled();
}

task<void> WaitForGpuAsset(AssetManager& mgr, StreamingAssetRef<GpuAsset> ref, WaitProbe& probe) {
    co_await mgr.Wait(ref);
    probe.Completed = ref.IsCompleted();
    probe.Ready = ref.IsReady();
    probe.Faulted = ref.IsFaulted();
    probe.Canceled = ref.IsCanceled();
}

// CPU-only:Load 登记 → Pump 内联完成 → Ready → 直接访问 StreamingAssetRef。
TEST(AssetManagerTest, CpuLoadReadyAfterPump) {
    AssetManager mgr;
    AssetId id = MakeId(1);
    StreamingAssetRef<CpuAsset> ref = mgr.Load<CpuAsset>(AssetLoadRequest{
        .Id = id,
        .Task = LoadCpuAsset(42)});
    ASSERT_TRUE(ref.IsValid());
    EXPECT_FALSE(ref.IsCompleted());  // Load 提交协程,完成结果尚未被 Pump 消费
    EXPECT_FALSE(ref.IsReady());
    EXPECT_FALSE(ref);

    mgr.Pump();
    EXPECT_TRUE(ref.IsCompleted());
    EXPECT_TRUE(ref.IsCompletedSuccessfully());
    ASSERT_TRUE(ref.IsReady());
    EXPECT_EQ(ref->Value(), 42);
}

TEST(AssetManagerTest, WaitCompletesAfterPumpCommitsReadyState) {
    AssetManager mgr;
    TaskScope scope;
    WaitProbe probe;
    AssetId id = MakeId(11);
    StreamingAssetRef<CpuAsset> ref = mgr.Load<CpuAsset>(AssetLoadRequest{
        .Id = id,
        .Task = LoadCpuAsset(42)});

    scope.Spawn(WaitForCpuAsset(mgr, ref, probe));
    EXPECT_FALSE(probe.Completed);

    mgr.Pump();
    EXPECT_TRUE(probe.Completed);
    EXPECT_TRUE(probe.Ready);
    EXPECT_FALSE(probe.Faulted);
    EXPECT_FALSE(probe.Canceled);
}

TEST(AssetManagerTest, WaitOnCompletedAssetReturnsInline) {
    AssetManager mgr;
    AssetId id = MakeId(12);
    StreamingAssetRef<CpuAsset> ref = mgr.Load<CpuAsset>(AssetLoadRequest{
        .Id = id,
        .Task = LoadCpuAsset(24)});
    mgr.Pump();
    ASSERT_TRUE(ref.IsReady());

    TaskScope scope;
    WaitProbe probe;
    scope.Spawn(WaitForCpuAsset(mgr, ref, probe));

    EXPECT_TRUE(probe.Completed);
    EXPECT_TRUE(probe.Ready);
}

// 去重:同 id 再次 Load 复用已就绪资产。
TEST(AssetManagerTest, LoadDeduplicatesById) {
    AssetManager mgr;
    AssetId id = MakeId(2);
    StreamingAssetRef<CpuAsset> t1 = mgr.Load<CpuAsset>(AssetLoadRequest{
        .Id = id,
        .Task = LoadCpuAsset(7)});
    mgr.Pump();
    ASSERT_TRUE(t1.IsCompletedSuccessfully());

    StreamingAssetRef<CpuAsset> t2 = mgr.Load<CpuAsset>(AssetLoadRequest{
        .Id = id,
        .Task = LoadCpuAsset(999)});  // 复用,不重新加载
    EXPECT_EQ(t2.GetHandle(), t1.GetHandle());
    EXPECT_TRUE(t2.IsCompletedSuccessfully());
    EXPECT_EQ(t2->Value(), 7);
    EXPECT_EQ(mgr.GetAssetCount(), 1u);
}

// 失败:loader 显式返回失败 result → Faulted。
TEST(AssetManagerTest, FaultedLoad) {
    AssetManager mgr;
    AssetId id = MakeId(3);
    StreamingAssetRef<CpuAsset> t = mgr.Load<CpuAsset>(AssetLoadRequest{
        .Id = id,
        .Task = LoadCpuAsset(-1)});
    mgr.Pump();
    EXPECT_TRUE(t.IsCompleted());
    EXPECT_TRUE(t.IsFaulted());
    EXPECT_FALSE(t.IsCompletedSuccessfully());
    EXPECT_TRUE(t.IsValid());
    EXPECT_FALSE(t.IsReady());
    EXPECT_EQ(t.Get(), nullptr);
}

// 异常:loader throw → RunLoad catch → Faulted,不崩溃。
TEST(AssetManagerTest, ExceptionLoadFaults) {
    AssetManager mgr;
    AssetId id = MakeId(7);
    StreamingAssetRef<CpuAsset> t = mgr.Load<CpuAsset>(AssetLoadRequest{
        .Id = id,
        .Task = LoadThrowingAsset()});
    mgr.Pump();
    EXPECT_TRUE(t.IsCompleted());
    EXPECT_TRUE(t.IsFaulted());
    EXPECT_FALSE(t.IsReady());
}

// GPU 上传跨帧:Pump 启动 → 挂起 → FrameUploadScheduler upload phase → NotifyFlightComplete
//          → PumpCompletedUploads 恢复协程 → AssetManager::Pump 提交 Ready。
TEST(AssetManagerTest, GpuUploadCrossFrame) {
    FrameUploadScheduler frameUploads;
    AssetManager mgr;
    AssetId id = MakeId(4);
    StreamingAssetRef<GpuAsset> t = mgr.Load<GpuAsset>(AssetLoadRequest{
        .Id = id,
        .Task = LoadGpuAsset(frameUploads, 123)});

    mgr.Pump();
    EXPECT_FALSE(t.IsCompleted());  // 等 GPU

    ResourceUploader uploader{nullptr, 1};
    frameUploads.RunUploadPhase(nullptr, uploader, 0);
    EXPECT_FALSE(t.IsCompleted());

    frameUploads.NotifyFlightComplete(0);
    EXPECT_FALSE(t.IsCompleted());  // 还需 PumpCompletedUploads 恢复协程并由 AssetManager::Pump 提交状态

    frameUploads.PumpCompletedUploads();
    mgr.Pump();
    EXPECT_TRUE(t.IsCompletedSuccessfully());
    EXPECT_EQ(t->Value(), 123);
}

TEST(AssetManagerTest, WaitCompletesAfterGpuLoadReachesTerminalState) {
    FrameUploadScheduler frameUploads;
    AssetManager mgr;
    TaskScope scope;
    WaitProbe probe;
    AssetId id = MakeId(13);
    StreamingAssetRef<GpuAsset> t = mgr.Load<GpuAsset>(AssetLoadRequest{
        .Id = id,
        .Task = LoadGpuAsset(frameUploads, 456)});

    scope.Spawn(WaitForGpuAsset(mgr, t, probe));
    mgr.Pump();
    EXPECT_FALSE(probe.Completed);

    ResourceUploader uploader{nullptr, 1};
    frameUploads.RunUploadPhase(nullptr, uploader, 0);
    frameUploads.NotifyFlightComplete(0);
    frameUploads.PumpCompletedUploads();
    EXPECT_FALSE(probe.Completed);

    mgr.Pump();
    EXPECT_TRUE(probe.Completed);
    EXPECT_TRUE(probe.Ready);
    EXPECT_FALSE(probe.Faulted);
    EXPECT_FALSE(probe.Canceled);
}

// Cancel:在 upload phase 前取消,不录制上传,恢复后提交 Canceled。
TEST(AssetManagerTest, CancelGpuLoadBeforeUploadPhase) {
    FrameUploadScheduler frameUploads;
    AssetManager mgr;
    AssetId id = MakeId(5);
    StreamingAssetRef<GpuAsset> t = mgr.Load<GpuAsset>(AssetLoadRequest{
        .Id = id,
        .Task = LoadGpuAsset(frameUploads, 321)});

    mgr.Pump();
    ASSERT_FALSE(t.IsCompleted());

    t.Cancel();
    frameUploads.PumpCompletedUploads();
    mgr.Pump();

    EXPECT_TRUE(t.IsCompleted());
    EXPECT_TRUE(t.IsCanceled());
    EXPECT_FALSE(t.IsReady());
}

// Cancel:已经录制上传并在等待 fence 时取消,终态保持 Canceled,不会被 fence 完成恢复成 Ready。
TEST(AssetManagerTest, CancelGpuLoadWhileWaitingFence) {
    FrameUploadScheduler frameUploads;
    AssetManager mgr;
    AssetId id = MakeId(8);
    StreamingAssetRef<GpuAsset> t = mgr.Load<GpuAsset>(AssetLoadRequest{
        .Id = id,
        .Task = LoadGpuAsset(frameUploads, 654)});

    mgr.Pump();
    ASSERT_FALSE(t.IsCompleted());

    ResourceUploader uploader{nullptr, 1};
    frameUploads.RunUploadPhase(nullptr, uploader, 0);
    frameUploads.PumpCompletedUploads();
    EXPECT_FALSE(t.IsCompleted());

    t.Cancel();
    frameUploads.PumpCompletedUploads();
    mgr.Pump();

    EXPECT_TRUE(t.IsCompleted());
    EXPECT_TRUE(t.IsCanceled());
    EXPECT_FALSE(t.IsReady());

    frameUploads.NotifyFlightComplete(0);
    frameUploads.PumpCompletedUploads();
    mgr.Pump();
    EXPECT_TRUE(t.IsCanceled());
}

// Unload:就绪资产显式回收后 streaming ref 失效。
TEST(AssetManagerTest, UnloadReadyAsset) {
    AssetManager mgr;
    AssetId id = MakeId(6);
    StreamingAssetRef<CpuAsset> t = mgr.Load<CpuAsset>(AssetLoadRequest{
        .Id = id,
        .Task = LoadCpuAsset(88)});
    mgr.Pump();
    ASSERT_TRUE(t.IsCompletedSuccessfully());

    mgr.Unload(id);
    EXPECT_FALSE(t.IsValid());
    EXPECT_FALSE(t.IsReady());
    EXPECT_EQ(mgr.GetAssetCount(), 0u);
}

TEST(AssetManagerTest, UnloadReadyAssetDestroysRenderResourceWithoutGpuSystem) {
    AssetManager mgr;

    int destroyed = 0;
    AssetId id = MakeId(10);
    StreamingAssetRef<RecyclableAsset> ref = mgr.AddReady<RecyclableAsset>(
        id,
        make_unique<RecyclableAsset>(&destroyed));
    ASSERT_TRUE(ref.IsReady());

    mgr.Unload(id);
    EXPECT_FALSE(ref.IsValid());
    EXPECT_EQ(mgr.GetAssetCount(), 0u);
    EXPECT_EQ(destroyed, 1);
}

// CollectUnreferenced:仍有 ref 存活的资产不被回收。
TEST(AssetManagerTest, CollectUnreferencedKeepsReferencedAsset) {
    AssetManager mgr;
    AssetId id = MakeId(20);
    StreamingAssetRef<CpuAsset> ref = mgr.Load<CpuAsset>(AssetLoadRequest{
        .Id = id,
        .Task = LoadCpuAsset(11)});
    mgr.Pump();
    ASSERT_TRUE(ref.IsReady());

    uint32_t collected = mgr.CollectUnreferenced();
    EXPECT_EQ(collected, 0u);
    EXPECT_TRUE(ref.IsValid());
    EXPECT_EQ(mgr.GetAssetCount(), 1u);
}

// CollectUnreferenced:所有 ref 释放后资产被回收。
TEST(AssetManagerTest, CollectUnreferencedReclaimsUnreferencedAsset) {
    AssetManager mgr;
    AssetId id = MakeId(21);
    {
        StreamingAssetRef<CpuAsset> ref = mgr.Load<CpuAsset>(AssetLoadRequest{
            .Id = id,
            .Task = LoadCpuAsset(22)});
        mgr.Pump();
        ASSERT_TRUE(ref.IsReady());
    }  // ref 析构,refcount 归零

    EXPECT_EQ(mgr.GetAssetCount(), 1u);  // 引用归零但尚未回收
    uint32_t collected = mgr.CollectUnreferenced();
    EXPECT_EQ(collected, 1u);
    EXPECT_EQ(mgr.GetAssetCount(), 0u);
}

// CollectUnreferenced:回收会释放 GPU 资源。
TEST(AssetManagerTest, CollectUnreferencedRecyclesRenderResource) {
    AssetManager mgr;
    int destroyed = 0;
    AssetId id = MakeId(22);
    {
        StreamingAssetRef<RecyclableAsset> ref = mgr.AddReady<RecyclableAsset>(
            id,
            make_unique<RecyclableAsset>(&destroyed));
        ASSERT_TRUE(ref.IsReady());
    }

    EXPECT_EQ(destroyed, 0);
    uint32_t collected = mgr.CollectUnreferenced();
    EXPECT_EQ(collected, 1u);
    EXPECT_EQ(destroyed, 1);
    EXPECT_EQ(mgr.GetAssetCount(), 0u);
}

// 引用计数:拷贝 ref 增加引用,单个析构不触发回收。
TEST(AssetManagerTest, RefCountTracksCopies) {
    AssetManager mgr;
    AssetId id = MakeId(23);
    StreamingAssetRef<CpuAsset> a = mgr.Load<CpuAsset>(AssetLoadRequest{
        .Id = id,
        .Task = LoadCpuAsset(33)});
    mgr.Pump();
    ASSERT_TRUE(a.IsReady());

    {
        StreamingAssetRef<CpuAsset> b = a;  // 拷贝 +1
        EXPECT_EQ(mgr.CollectUnreferenced(), 0u);
        EXPECT_TRUE(b.IsReady());
    }  // b 析构 -1,a 仍持有

    EXPECT_EQ(mgr.CollectUnreferenced(), 0u);
    EXPECT_TRUE(a.IsValid());

    a.Reset();  // 最后一个 ref 释放
    EXPECT_EQ(mgr.CollectUnreferenced(), 1u);
    EXPECT_EQ(mgr.GetAssetCount(), 0u);
}

// Unload:强制回收无视引用计数,仍存活的引用静默失效(不崩溃)。
TEST(AssetManagerTest, ForceUnloadInvalidatesLiveReferences) {
    AssetManager mgr;
    AssetId id = MakeId(25);
    StreamingAssetRef<CpuAsset> a = mgr.Load<CpuAsset>(AssetLoadRequest{
        .Id = id,
        .Task = LoadCpuAsset(55)});
    mgr.Pump();
    StreamingAssetRef<CpuAsset> b = a;  // 两个存活引用
    ASSERT_TRUE(a.IsReady());
    ASSERT_TRUE(b.IsReady());

    mgr.Unload(id);  // 无视 refcount 强制回收(DEBUG 下打 warning)

    EXPECT_FALSE(a.IsValid());
    EXPECT_FALSE(b.IsValid());
    EXPECT_EQ(a.Get(), nullptr);
    EXPECT_EQ(b.Get(), nullptr);
    EXPECT_EQ(mgr.GetAssetCount(), 0u);

    // 存活引用析构走 Release(旧 handle),因 generation 不匹配为 no-op,不崩溃。
}

// CollectUnreferenced:命中在飞 slot 会取消并延迟到终态回收。
TEST(AssetManagerTest, CollectUnreferencedCancelsLoadingSlot) {
    FrameUploadScheduler frameUploads;
    AssetManager mgr;
    AssetId id = MakeId(24);
    {
        StreamingAssetRef<GpuAsset> t = mgr.Load<GpuAsset>(AssetLoadRequest{
            .Id = id,
            .Task = LoadGpuAsset(frameUploads, 44)});
        mgr.Pump();
        ASSERT_FALSE(t.IsCompleted());
    }  // ref 析构,refcount 归零,但 slot 仍在飞

    uint32_t collected = mgr.CollectUnreferenced();
    EXPECT_EQ(collected, 1u);  // 标记 PendingUnload
    EXPECT_EQ(mgr.GetAssetCount(), 1u);  // 终态前不销毁

    frameUploads.PumpCompletedUploads();
    mgr.Pump();
    EXPECT_EQ(mgr.GetAssetCount(), 0u);
}

// 线程安全:多个 worker 线程并发拷贝/析构引用,计数最终一致归零。
TEST(AssetManagerTest, RefCountIsThreadSafeAcrossWorkers) {
    AssetManager mgr;
    AssetId id = MakeId(30);
    StreamingAssetRef<CpuAsset> base = mgr.Load<CpuAsset>(AssetLoadRequest{
        .Id = id,
        .Task = LoadCpuAsset(99)});
    mgr.Pump();
    ASSERT_TRUE(base.IsReady());

    constexpr int kThreads = 8;
    constexpr int kIters = 5000;
    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&]() {
            while (!start.load(std::memory_order_acquire)) {
            }
            for (int i = 0; i < kIters; ++i) {
                // 从主线程创建的引用拷贝(仅触碰共享控制块的原子,不碰 slot 表)。
                StreamingAssetRef<CpuAsset> copy = base;
                StreamingAssetRef<CpuAsset> moved = std::move(copy);
                (void)moved;
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& w : workers) {
        w.join();
    }

    // 所有 worker 引用已析构,只剩 base 一个:资产仍存活,回收不掉。
    EXPECT_TRUE(base.IsValid());
    EXPECT_EQ(mgr.CollectUnreferenced(), 0u);

    base.Reset();
    EXPECT_EQ(mgr.CollectUnreferenced(), 1u);
    EXPECT_EQ(mgr.GetAssetCount(), 0u);
}

// Unload:在加载中卸载会取消任务,终态后销毁 slot。
TEST(AssetManagerTest, UnloadWhileLoadingDestroysSlotAfterCancel) {
    FrameUploadScheduler frameUploads;
    AssetManager mgr;
    AssetId id = MakeId(9);
    StreamingAssetRef<GpuAsset> t = mgr.Load<GpuAsset>(AssetLoadRequest{
        .Id = id,
        .Task = LoadGpuAsset(frameUploads, 777)});

    mgr.Pump();
    ASSERT_FALSE(t.IsCompleted());

    mgr.Unload(id);
    frameUploads.PumpCompletedUploads();
    mgr.Pump();

    EXPECT_FALSE(t.IsValid());
    EXPECT_EQ(mgr.GetAssetCount(), 0u);
}

}  // namespace
