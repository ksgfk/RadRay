#include <gtest/gtest.h>

#include <stdexcept>

#include <radray/runtime/asset_manager.h>
#include <radray/runtime/gpu_system.h>

using namespace radray;

namespace {

// ── CPU-only fake asset:loader 同帧内联完成,无 GPU 上传 ──
class CpuAsset : public Asset {
public:
    explicit CpuAsset(int value) noexcept : _value(value) {}
    void OnUnload() override;
    AssetTypeId GetTypeId() const noexcept override;
    int Value() const noexcept { return _value; }

private:
    int _value;
};

// ── 带 GPU 上传阶段的 fake asset:loader co_await 一次上传,跨帧 ──
class GpuAsset : public Asset {
public:
    explicit GpuAsset(int value) noexcept : _value(value) {}
    void OnUnload() override {}
    AssetTypeId GetTypeId() const noexcept override;
    int Value() const noexcept { return _value; }

private:
    int _value;
};

}  // namespace

namespace radray {

template <>
struct RuntimeTypeTrait<CpuAsset> {
    static constexpr RuntimeTypeId value{0x11111111, 0x2222, 0x3333, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb};
};

template <>
struct RuntimeTypeTrait<GpuAsset> {
    static constexpr RuntimeTypeId value{0xcccccccc, 0xdddd, 0xeeee, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
};

}  // namespace radray

namespace {

void CpuAsset::OnUnload() {}
AssetTypeId CpuAsset::GetTypeId() const noexcept { return runtime_type_id_v<CpuAsset>; }
AssetTypeId GpuAsset::GetTypeId() const noexcept { return runtime_type_id_v<GpuAsset>; }

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
